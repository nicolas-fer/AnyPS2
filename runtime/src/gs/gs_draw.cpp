// Rasterizador de referência do GS (software).
//
// Convenções do hardware seguidas aqui:
//  * Coordenadas em ponto fixo 12.4; o centro do pixel (x, y) fica em
//    (16x, 16y). Triângulos usam a regra top-left; sprites cobrem
//    [x0, x1) × [y0, y1).
//  * Sombreamento flat usa a cor do último vértice (o que disparou o
//    desenho); sprites usam cor e Z do segundo vértice.
//  * S/T/Q são interpolados linearmente na tela e divididos por pixel
//    (correção de perspectiva); UV (FST=1) é interpolado direto.
//  * Pipeline de pixel na ordem do GS: textura → TFX → fog → teste de alfa
//    → teste de alfa de destino → teste de Z → blending → dither → máscara
//    → escrita.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/gs/gs_bands.h"
#include "anyps2/runtime/gs/gs_coverage.h"
#include "anyps2/runtime/gs/gs_trace.h"
#include "anyps2/runtime/host_profile.h"

namespace anyps2::rt::gs {

namespace {
// Contador de uma faixa só: carga + store (sem lock add), lido de relance pelo produtor.
inline void bump(std::atomic<std::uint64_t>& c, std::uint64_t n = 1) {
    c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

// Balde do histograma de pixels por desenho: 0 para zero, senão 1 + log2 (cortado).
unsigned pixBucket(std::uint64_t n) {
    unsigned b = 0;
    while (n && b + 1 < BandStats::kPixBuckets) {
        ++b;
        n >>= 1;
    }
    return b;
}

// Mede 1 de cada 64 chamadas (só com ANYPS2_PROFILE ligado): conta cada chamada e
// cronometra a de número múltiplo de 64. Desligado, não faz nada além de um teste.
class SampledTimer {
public:
    SampledTimer(std::atomic<std::uint64_t>& calls, std::atomic<std::uint64_t>& timed, std::atomic<std::uint64_t>& ns,
                 bool on)
        : timed_(timed), ns_(ns) {
        if (!on) return;
        const std::uint64_t n = calls.load(std::memory_order_relaxed) + 1;
        calls.store(n, std::memory_order_relaxed);
        if ((n & 63) == 0) {
            active_ = true;
            t0_ = std::chrono::steady_clock::now();
        }
    }
    ~SampledTimer() {
        if (!active_) return;
        bump(ns_, static_cast<std::uint64_t>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0_).count()));
        bump(timed_);
    }
    SampledTimer(const SampledTimer&) = delete;
    SampledTimer& operator=(const SampledTimer&) = delete;

private:
    std::atomic<std::uint64_t>& timed_;
    std::atomic<std::uint64_t>& ns_;
    bool active_ = false;
    std::chrono::steady_clock::time_point t0_{};
};

bool isT4(std::uint32_t psm) {
    return psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH;
}
bool isIndexed(std::uint32_t psm) {
    return psm == PSMT8 || psm == PSMT8H || isT4(psm);
}

unsigned texFmtClass(std::uint32_t psm) {
    switch (psm) {
        case PSMT4: case PSMT4HL: case PSMT4HH: return TexReport::FmtT4;
        case PSMT8: case PSMT8H: return TexReport::FmtT8;
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return TexReport::FmtCT16;
        case PSMCT32: case PSMZ32: return TexReport::FmtCT32;
        default: return TexReport::FmtOther;
    }
}
}  // namespace

TexReport Gs::texReport() const {
    TexReport r;
    for (const LaneTex& l : laneTex_) {
        for (unsigned i = 0; i < TexReport::FmtCount; ++i) r.fetch[i] += l.fetch[i].load(std::memory_order_relaxed);
        r.samples += l.samples.load(std::memory_order_relaxed);
        r.linear += l.linear.load(std::memory_order_relaxed);
        r.trilinear += l.trilinear.load(std::memory_order_relaxed);
        r.sampleTimed += l.sampleTimed.load(std::memory_order_relaxed);
        r.sampleNs += l.sampleNs.load(std::memory_order_relaxed);
        r.shadeCalls += l.shadeCalls.load(std::memory_order_relaxed);
        r.shadeTimed += l.shadeTimed.load(std::memory_order_relaxed);
        r.shadeNs += l.shadeNs.load(std::memory_order_relaxed);
    }
    for (unsigned i = 0; i < BandStats::kPixBuckets; ++i) {
        r.pixHist[i] = bandStats_.pixHist[i];
        r.pixHistTex[i] = bandStats_.pixHistTex[i];
    }
    return r;
}

namespace {

constexpr std::uint64_t bits(std::uint64_t v, unsigned lo, unsigned n) {
    return (v >> lo) & ((1ull << n) - 1);
}

int clamp255(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

std::uint32_t expand16(std::uint32_t v, std::uint32_t ta0, std::uint32_t ta1, bool aem) {
    const std::uint32_t rgb = ((v & 0x1F) << 3) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 19);
    std::uint32_t a;
    if (v & 0x8000) a = ta1;
    else a = (aem && (v & 0x7FFF) == 0) ? 0 : ta0;
    return rgb | (a << 24);
}

}  // namespace

struct Gs::DrawEnv {
    unsigned type = 0;
    bool iip = false, tme = false, fge = false, abe = false, fst = false;
    unsigned ctxt = 0;
    // Faixa que executa o desenho: só sombreia as linhas que lhe cabem (gs_bands.h).
    // O produtor deixa 0 de 1 (todas as linhas); cada faixa põe a sua antes de desenhar.
    unsigned lane = 0, lanes = 1;
    bool owns(int y) const { return rowInLane(lane, lanes, y); }
    // Frame/Z
    std::uint32_t fbp = 0, fbw = 0, fpsm = 0, fbmsk = 0;
    // FBMSK cobre todos os bits do formato: o FRAME não muda (o valor escrito seria o lido).
    // O desenho não o escreve nem o lê (salvo DATE, que lê o alfa de destino).
    bool fbNone = false;
    std::uint32_t zbp = 0, zpsm = 0;
    bool zmsk = false;
    int scax0 = 0, scax1 = 0, scay0 = 0, scay1 = 0;
    // Testes
    bool ate = false, date = false, datm = false, zte = false;
    unsigned atst = 0, aref = 0, afail = 0, ztst = 0;
    // Blending
    unsigned ba = 0, bb = 0, bc = 0, bd = 0;
    int bfix = 0;
    bool pabe = false, colclamp = true, fba = false, dthe = false;
    int dimx[4][4] = {};
    unsigned scanmsk = 0;
    // Textura
    std::uint32_t tbp[7] = {}, tbw[7] = {};
    std::uint32_t tpsm = 0, tw = 0, th = 0, cpsm = 0, csa = 0;
    // Versão da CLUT que vale para este desenho (imutável; ver ClutBuffer).
    std::shared_ptr<const ClutBuffer> clut;
    bool tcc = false;
    unsigned tfx = 0;
    unsigned mxl = 0, mmin = 0, lodL = 0;
    bool mmag = false, lcm = false;
    float lodK = 0;
    // Sem mipmap (MXL = 0) e com o mesmo filtro para ampliar e reduzir, o
    // nível de detalhe não muda a amostra: o log2 por pixel é dispensado.
    bool lodMatters = true;
    unsigned wms = 0, wmt = 0, minu = 0, maxu = 0, minv = 0, maxv = 0;
    std::uint32_t ta0 = 0, ta1 = 0;
    bool aem = false;
    int fogR = 0, fogG = 0, fogB = 0;
    // Paleta expandida da faixa (Gs::palette), posta por rasterize depois do resolve da
    // CLUT; nula fora dos indexados. O desenho em si é const, daí o mutable.
    mutable const std::uint32_t* pal = nullptr;
    // Versões do cache de texels crus (gs_texcache.h), uma por nível, fixadas por este
    // desenho: o despejo do índice nunca libera o que está em voo. Nulo = sem cache (cache
    // desligado, ou desenho sozinho com selfTexture/selfFrameZ/selfWrap), e a leitura vai
    // direto à VRAM.
    std::shared_ptr<TexLevel> tex[7];
    std::uint64_t drawId = 0;  // número do desenho (mensagem do VERIFY)
    // Alcance de leitura do nível 0 declarado por Gs::draw (texReach0), quando é menor que a
    // textura inteira; só o VERIFY o confere (texelRaw), para pegar um retângulo pequeno demais.
    bool reachKnown = false;
    unsigned reachU0 = 0, reachU1 = 0, reachV0 = 0, reachV1 = 0;
};

struct Gs::Fragment {
    std::uint32_t z = 0;
    int r = 0, g = 0, b = 0, a = 0;
    float s = 0, t = 0, q = 1;  // STQ (FST=0)
    float u = 0, v = 0;         // UV em texels (FST=1)
    int fog = 255;
};

void Gs::setupEnv(DrawEnv& e, std::uint32_t pc) {
    const std::uint64_t prim = regs_[PRIM];
    const std::uint64_t attr = (regs_[PRMODECONT] & 1) ? prim : regs_[PRMODE];
    e.type = static_cast<unsigned>(prim & 7);
    e.iip = bits(attr, 3, 1);
    e.tme = bits(attr, 4, 1);
    e.fge = bits(attr, 5, 1);
    e.abe = bits(attr, 6, 1);
    if (bits(attr, 7, 1)) warnOnce("PRIM.AA1 (antialiasing de bordas) não é emulado; desenhando sem AA");
    e.fst = bits(attr, 8, 1);
    e.ctxt = static_cast<unsigned>(bits(attr, 9, 1));
    const Context& k = ctx_[e.ctxt];

    e.fbp = static_cast<std::uint32_t>(bits(k.frame, 0, 9)) * 32;
    e.fbw = static_cast<std::uint32_t>(bits(k.frame, 16, 6));
    e.fpsm = static_cast<std::uint32_t>(bits(k.frame, 24, 6));
    e.fbmsk = static_cast<std::uint32_t>(k.frame >> 32);
    {
        // Bits do FBMSK que o formato guarda (nos de 16 bits, os 5 altos de cada canal e o alfa).
        const std::uint32_t used = (e.fpsm == PSMCT24 || e.fpsm == PSMZ24)   ? 0x00FFFFFFu
                                   : psmStorageBits(e.fpsm) == 16              ? 0x80F8F8F8u
                                                                               : 0xFFFFFFFFu;
        e.fbNone = (e.fbmsk & used) == used;
    }
    switch (e.fpsm) {
        case PSMCT32: case PSMCT24: case PSMCT16: case PSMCT16S: case PSMZ32: case PSMZ24: case PSMZ16:
        case PSMZ16S:
            break;
        default:
            unsupported("desenho em FRAME com formato " + psmName(e.fpsm), pc);
    }
    e.zbp = static_cast<std::uint32_t>(bits(k.zbuf, 0, 9)) * 32;
    e.zpsm = 0x30u | static_cast<std::uint32_t>(bits(k.zbuf, 24, 4));
    e.zmsk = bits(k.zbuf, 32, 1);
    e.scax0 = static_cast<int>(bits(k.scissor, 0, 11));
    e.scax1 = static_cast<int>(bits(k.scissor, 16, 11));
    e.scay0 = static_cast<int>(bits(k.scissor, 32, 11));
    e.scay1 = static_cast<int>(bits(k.scissor, 48, 11));

    e.ate = bits(k.test, 0, 1);
    e.atst = static_cast<unsigned>(bits(k.test, 1, 3));
    e.aref = static_cast<unsigned>(bits(k.test, 4, 8));
    e.afail = static_cast<unsigned>(bits(k.test, 12, 2));
    e.date = bits(k.test, 14, 1);
    e.datm = bits(k.test, 15, 1);
    e.zte = bits(k.test, 16, 1);
    e.ztst = static_cast<unsigned>(bits(k.test, 17, 2));
    if (e.zte && e.ztst != 1 && !isValidPsm(e.zpsm)) unsupported("ZBUF com formato inválido " + psmName(e.zpsm), pc);

    e.ba = static_cast<unsigned>(bits(k.alpha, 0, 2));
    e.bb = static_cast<unsigned>(bits(k.alpha, 2, 2));
    e.bc = static_cast<unsigned>(bits(k.alpha, 4, 2));
    e.bd = static_cast<unsigned>(bits(k.alpha, 6, 2));
    e.bfix = static_cast<int>(bits(k.alpha, 32, 8));
    if (e.abe && (e.ba == 3 || e.bb == 3 || e.bc == 3 || e.bd == 3)) {
        unsupported("ALPHA com seletor reservado (3)", pc);
    }
    e.pabe = regs_[PABE] & 1;
    e.colclamp = regs_[COLCLAMP] & 1;
    e.fba = k.fba & 1;
    e.dthe = regs_[DTHE] & 1;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const auto v = static_cast<int>(bits(regs_[DIMX], static_cast<unsigned>(y * 16 + x * 4), 3));
            e.dimx[y][x] = v >= 4 ? v - 8 : v;
        }
    }
    e.scanmsk = static_cast<unsigned>(regs_[SCANMSK] & 3);
    const std::uint64_t fogcol = regs_[FOGCOL];
    e.fogR = static_cast<int>(bits(fogcol, 0, 8));
    e.fogG = static_cast<int>(bits(fogcol, 8, 8));
    e.fogB = static_cast<int>(bits(fogcol, 16, 8));

    if (e.tme) {
        const std::uint64_t t0 = k.tex0, t1 = k.tex1, cl = k.clamp;
        e.tbp[0] = static_cast<std::uint32_t>(bits(t0, 0, 14));
        e.tbw[0] = static_cast<std::uint32_t>(bits(t0, 14, 6));
        e.tpsm = static_cast<std::uint32_t>(bits(t0, 20, 6));
        if (!isValidPsm(e.tpsm)) unsupported("textura com " + psmName(e.tpsm), pc);
        e.tw = static_cast<std::uint32_t>(bits(t0, 26, 4));
        e.th = static_cast<std::uint32_t>(bits(t0, 30, 4));
        if (e.tw > 10) e.tw = 10;
        if (e.th > 10) e.th = 10;
        e.tcc = bits(t0, 34, 1);
        e.tfx = static_cast<unsigned>(bits(t0, 35, 2));
        e.cpsm = static_cast<std::uint32_t>(bits(t0, 51, 4));
        e.clut = clut_;
        e.csa = static_cast<std::uint32_t>(bits(t0, 56, 5));
        e.lcm = bits(t1, 0, 1);
        e.mxl = static_cast<unsigned>(bits(t1, 2, 3));
        if (e.mxl > 6) unsupported("TEX1.MXL = " + std::to_string(e.mxl) + " (máximo 6)", pc);
        e.mmag = bits(t1, 5, 1);
        e.mmin = static_cast<unsigned>(bits(t1, 6, 3));
        if (e.mmin > 5) unsupported("TEX1.MMIN = " + std::to_string(e.mmin) + " (reservado)", pc);
        if (e.mxl > 0 && bits(t1, 9, 1)) {
            unsupported("TEX1.MTBA (endereços de mipmap automáticos) ainda não suportado", pc);
        }
        e.lodL = static_cast<unsigned>(bits(t1, 19, 2));
        auto k12 = static_cast<int>(bits(t1, 32, 12));
        if (k12 & 0x800) k12 -= 0x1000;
        e.lodK = static_cast<float>(k12) / 16.0f;
        const bool minLinear = e.mmin == 1 || e.mmin >= 4;
        e.lodMatters = e.mxl > 0 || minLinear != e.mmag;
        const std::uint64_t m1 = k.miptbp1, m2 = k.miptbp2;
        for (unsigned i = 0; i < 3; ++i) {
            e.tbp[1 + i] = static_cast<std::uint32_t>(bits(m1, i * 20, 14));
            e.tbw[1 + i] = static_cast<std::uint32_t>(bits(m1, i * 20 + 14, 6));
            e.tbp[4 + i] = static_cast<std::uint32_t>(bits(m2, i * 20, 14));
            e.tbw[4 + i] = static_cast<std::uint32_t>(bits(m2, i * 20 + 14, 6));
        }
        e.wms = static_cast<unsigned>(bits(cl, 0, 2));
        e.wmt = static_cast<unsigned>(bits(cl, 2, 2));
        e.minu = static_cast<unsigned>(bits(cl, 4, 10));
        e.maxu = static_cast<unsigned>(bits(cl, 14, 10));
        e.minv = static_cast<unsigned>(bits(cl, 24, 10));
        e.maxv = static_cast<unsigned>(bits(cl, 34, 10));
        const std::uint64_t texa = regs_[TEXA];
        e.ta0 = static_cast<std::uint32_t>(bits(texa, 0, 8));
        e.aem = bits(texa, 15, 1);
        e.ta1 = static_cast<std::uint32_t>(bits(texa, 32, 8));
    }
}

void Gs::rasterize(const DrawEnv& e, const Vertex& v0, const Vertex& v1, const Vertex& v2) {
    // Uma vez por desenho (e por faixa), fora do laço de texels: a versão da CLUT é
    // preguiçosa e as barreiras enfileiradas antes deste desenho já preencheram as
    // sobreposições de que ela depende.
    if (e.clut && isIndexed(e.tpsm)) {
        e.clut->resolve();
        e.pal = palette(e, isT4(e.tpsm) ? 16u : 256u);
    }
    switch (e.type) {
        case 0: drawPoint(e, v0); break;
        case 1: case 2: drawLine(e, v0, v1); break;
        case 3: case 4: case 5: drawTriangle(e, v0, v1, v2); break;
        case 6: drawSprite(e, v0, v1); break;
        default: break;
    }
}

namespace {

// Retângulo (inclusivo) de texels do nível 0 que um desenho busca, já depois do modo de wrap.
struct TexRect {
    unsigned u0 = 0, u1 = 0, v0 = 0, v1 = 0;
};

// Parâmetros de Gs::draw que o alcance exato usa (o DrawEnv é privado da classe).
struct ReachParams {
    unsigned type = 0, tw = 0, th = 0;
    unsigned wms = 0, wmt = 0, minu = 0, maxu = 0, minv = 0, maxv = 0;
    bool fst = false;
    bool linear = false;            // algum filtro do desenho pode ser bilinear (MMAG ou MMIN)
    unsigned fullU = 1, fullV = 1;  // alcance cheio (colunas, linhas): a volta quando não há como ser exato
};

bool smallFinite(double v) {
    return std::isfinite(v) && std::fabs(v) < 1.0e6;
}

// Faixa de coordenadas inteiras [lo, hi] (antes do wrap) -> faixa de texels depois do
// modo de wrap, como em texelRaw. Quando o modo não permite uma faixa contínua (REPEAT que
// dá a volta, REGION_REPEAT com faixa longa, REGION_CLAMP inválido) devolve o alcance cheio.
void wrapAxis(long long lo, long long hi, unsigned mode, unsigned size, unsigned mn, unsigned mx, unsigned full,
              unsigned& outLo, unsigned& outHi) {
    outLo = 0;
    outHi = full - 1;
    switch (mode) {
        case 0: {  // REPEAT
            if (hi - lo + 1 >= static_cast<long long>(size)) return;
            const auto m = static_cast<long long>(size - 1);
            const auto a = static_cast<unsigned>(lo & m), b = static_cast<unsigned>(hi & m);
            if (a <= b) {
                outLo = a;
                outHi = b;
            }
            return;
        }
        case 1:  // CLAMP
            outLo = static_cast<unsigned>(std::clamp(lo, 0ll, static_cast<long long>(size) - 1));
            outHi = static_cast<unsigned>(std::clamp(hi, 0ll, static_cast<long long>(size) - 1));
            return;
        case 2:  // REGION_CLAMP (nível 0: MINU/MAXU sem deslocamento)
            if (mn > mx) {  // região invertida: o clamp devolve MINU (c < MINU) ou MAXU
                outLo = mx;
                outHi = mn;
                return;
            }
            outLo = static_cast<unsigned>(std::clamp(lo, static_cast<long long>(mn), static_cast<long long>(mx)));
            outHi = static_cast<unsigned>(std::clamp(hi, static_cast<long long>(mn), static_cast<long long>(mx)));
            return;
        default: {  // REGION_REPEAT: (c & MSK) | FIX não é monotônico; faixas curtas, por enumeração
            if (hi - lo + 1 > 256) return;
            unsigned mi = ~0u, ma = 0;
            for (long long c = lo; c <= hi; ++c) {
                const auto r = static_cast<unsigned>((c & static_cast<long long>(mn)) | static_cast<long long>(mx));
                mi = std::min(mi, r);
                ma = std::max(ma, r);
            }
            outLo = mi;
            outHi = ma;
            return;
        }
    }
}

// Retângulo de texels do nível 0 que o primitivo realmente busca. Falso quando não há como
// limitar com segurança (STQ com Q variável, valores fora da faixa, primitivo vazio): quem
// chama fica com a textura inteira. Sprites repetem a conta de drawSprite/shadePixel (a
// coluna e a linha extremas do retângulo cortado pelo SCISSOR; a interpolação é monótona),
// então o resultado é exato; pontos, linhas e triângulos usam o mínimo e o máximo dos
// vértices com 1 texel de margem (arredondamento da interpolação). O bilinear soma o
// texel seguinte (taps iu e iu+1 depois do deslocamento de meio texel). Só vale para o
// nível 0: os níveis de mipmap seguintes ficam com o alcance cheio.
bool texReach0(const ReachParams& p, const DrawWindow& w, const Vertex& v0, const Vertex& v1, const Vertex& v2,
               TexRect& out) {
    const float fw = static_cast<float>(1u << p.tw), fh = static_cast<float>(1u << p.th);
    float fuLo = 0, fuHi = 0, fvLo = 0, fvHi = 0;  // coordenadas em texels (floats exatos do sprite)
    double duLo = 0, duHi = 0, dvLo = 0, dvHi = 0;
    const bool sprite = p.type == 6;
    if (sprite) {
        const SpriteRect r = spriteRect(w, v0, v1);
        if (r.empty()) return false;
        const double spanX = v1.x - v0.x, spanY = v1.y - v0.y;
        const float q = v1.q;
        auto colAt = [&](int px) {
            const double tx = spanX != 0 ? (px * 16.0 - v0.x) / spanX : 0.0;
            if (p.fst) {
                const double a = static_cast<double>(v0.u) / 16.0, b = static_cast<double>(v1.u) / 16.0;
                return static_cast<float>(a + (b - a) * tx);
            }
            const double a = static_cast<double>(v0.s), b = static_cast<double>(v1.s);
            const auto s = static_cast<float>(a + (b - a) * tx);
            return s / q * fw;
        };
        auto rowAt = [&](int py) {
            const double ty = spanY != 0 ? (py * 16.0 - v0.y) / spanY : 0.0;
            if (p.fst) {
                const double a = static_cast<double>(v0.v) / 16.0, b = static_cast<double>(v1.v) / 16.0;
                return static_cast<float>(a + (b - a) * ty);
            }
            const double a = static_cast<double>(v0.t), b = static_cast<double>(v1.t);
            const auto t = static_cast<float>(a + (b - a) * ty);
            return t / q * fh;
        };
        const float ua = colAt(r.minX), ub = colAt(r.maxX), va = rowAt(r.minY), vb = rowAt(r.maxY);
        fuLo = std::min(ua, ub);
        fuHi = std::max(ua, ub);
        fvLo = std::min(va, vb);
        fvHi = std::max(va, vb);
        if (!smallFinite(static_cast<double>(fuLo)) || !smallFinite(static_cast<double>(fuHi)) ||
            !smallFinite(static_cast<double>(fvLo)) || !smallFinite(static_cast<double>(fvHi))) {
            return false;
        }
    } else {
        const Vertex* vs[3] = {&v0, &v1, &v2};
        const unsigned n = p.type == 0 ? 1u : (p.type == 1 || p.type == 2) ? 2u : 3u;
        duLo = dvLo = 1.0e30;
        duHi = dvHi = -1.0e30;
        for (unsigned i = 0; i < n; ++i) {
            double u, v;
            if (p.fst) {
                u = static_cast<double>(vs[i]->u) / 16.0;
                v = static_cast<double>(vs[i]->v) / 16.0;
            } else {
                // S/Q e T/Q só são limitados se o Q é o mesmo nos vértices (a interpolação
                // de S/T fica entre os extremos); com Q variável a divisão por pixel não é.
                const float q = vs[i]->q;
                if (q != vs[0]->q || q == 0.0f || !std::isfinite(q)) return false;
                u = static_cast<double>(vs[i]->s) / static_cast<double>(q) * static_cast<double>(fw);
                v = static_cast<double>(vs[i]->t) / static_cast<double>(q) * static_cast<double>(fh);
            }
            if (!smallFinite(u) || !smallFinite(v)) return false;
            duLo = std::min(duLo, u);
            duHi = std::max(duHi, u);
            dvLo = std::min(dvLo, v);
            dvHi = std::max(dvHi, v);
        }
    }
    long long ulo, uhi, vlo, vhi;
    if (sprite) {
        if (p.linear) {
            ulo = static_cast<long long>(std::floor(fuLo - 0.5f));
            uhi = static_cast<long long>(std::floor(fuHi - 0.5f)) + 1;
            vlo = static_cast<long long>(std::floor(fvLo - 0.5f));
            vhi = static_cast<long long>(std::floor(fvHi - 0.5f)) + 1;
        } else {
            ulo = static_cast<long long>(std::floor(fuLo));
            uhi = static_cast<long long>(std::floor(fuHi));
            vlo = static_cast<long long>(std::floor(fvLo));
            vhi = static_cast<long long>(std::floor(fvHi));
        }
    } else {
        const double off = p.linear ? 0.5 : 0.0;
        ulo = static_cast<long long>(std::floor(duLo - off)) - 1;
        uhi = static_cast<long long>(std::floor(duHi - off)) + (p.linear ? 2 : 1);
        vlo = static_cast<long long>(std::floor(dvLo - off)) - 1;
        vhi = static_cast<long long>(std::floor(dvHi - off)) + (p.linear ? 2 : 1);
    }
    wrapAxis(ulo, uhi, p.wms, 1u << p.tw, p.minu, p.maxu, p.fullU, out.u0, out.u1);
    wrapAxis(vlo, vhi, p.wmt, 1u << p.th, p.minv, p.maxv, p.fullV, out.v0, out.v1);
    out.u1 = std::min(out.u1, 2047u);
    out.v1 = std::min(out.v1, 2047u);
    return out.u0 <= out.u1 && out.v0 <= out.v1;
}

// Função pixel→endereço do FRAME/ZBUF e da textura: só formatos com a mesma função (CT32 e CT24,
// Z32 e Z24, e cada um dos de 16 bits) leem o texel (u, v) no endereço do pixel (u, v).
unsigned mapKey(std::uint32_t psm) {
    switch (psm) {
        case PSMCT32: case PSMCT24: return 1;
        case PSMZ32: case PSMZ24: return 2;
        case PSMCT16: return 3;
        case PSMCT16S: return 4;
        case PSMZ16: return 5;
        case PSMZ16S: return 6;
        default: return 0;
    }
}

}  // namespace

// Regra de ordem entre desenhos (gs_bands.h): um desenho só entra nas faixas
// junto com os pendentes se não colide com eles na VRAM. Para isso o produtor
// anota as páginas que o desenho escreve (FRAME, e ZBUF quando há escrita de Z)
// e lê (texturas, níveis de mipmap incluídos); o que cai em cima de algo
// pendente vira uma barreira. Se a textura cai em cima do próprio FRAME/ZBUF
// do desenho, o desenho inteiro roda como operação global, numa faixa só.
// CLUT e transferências já são barreiras (submit), então não entram aqui.
void Gs::draw(std::uint32_t pc) {
    // Só o trabalho do produtor entra no perfil, na trilha da thread que o chama (a do EE, ou a
    // do consumidor do VU1): o HostProfile não é seguro entre threads além das duas trilhas, e
    // o rasterizador roda nas faixas.
    const HostProfile::Scope prof(HostProfile::Gs);
    DrawEnv e;
    setupEnv(e, pc);
    ++drawCount_;
    // O tempo do GS usa a contagem analítica, feita aqui na ordem do programa
    // (as faixas ainda não desenharam nada disto).
    const DrawWindow w{e.scax0, e.scax1, e.scay0, e.scay1, e.scanmsk};
    const Vertex v0 = queue_[0], v1 = queue_[1], v2 = queue_[2];
    const std::uint64_t covered = coveredPixels(w, e.type, v0, v1, v2);
    pixels_ += covered;
    coveredTotal_ += covered;
    if (HostProfile::enabled()) {
        const unsigned bucket = pixBucket(covered);
        ++bandStats_.pixHist[bucket];
        if (e.tme) ++bandStats_.pixHistTex[bucket];
    }
    if (e.type > 6) return;  // tipo 7 já foi recusado em vertexKick
    const RowRange rows = drawRows(w, e.type, v0, v1, v2);
    if (rows.empty()) return;  // nada cai dentro do SCISSOR: nada a fazer

    // Diagnóstico (gs_trace.h): sem ANYPS2_GS_PROBE/ANYPS2_GS_DRAWLOG o rastreador
    // é nulo. beginDraw lê o pixel antes e endDraw depois do desenho enfileirado.
    const bool traced = trace_ && trace_->beginDraw(*this, pc, e.type, w, v0, v1, v2);

    // Acessos à VRAM. Escritas de FRAME/ZBUF usam a caixa do desenho (cortada pelo
    // SCISSOR); leituras de textura, o maior texel que o modo de CLAMP alcança.
    const RowRange colRange = drawCols(w, e.type, v0, v1, v2);
    if (colRange.empty()) return;
    const auto rowLo = static_cast<unsigned>(rows.lo), rowHi = static_cast<unsigned>(rows.hi);
    const auto colLo = static_cast<unsigned>(std::max(colRange.lo, 0)), colHi = static_cast<unsigned>(colRange.hi);
    const unsigned cols = colHi + 1;
    // O mapeamento (base em páginas, largura, formato) identifica as escritas que
    // caem na mesma faixa; o conjunto de páginas é exato (pageSet).
    auto access = [](const PageSet& p, bool write, unsigned surface, std::uint32_t bp, std::uint32_t bw,
                     std::uint32_t psm) {
        VramAccess a;
        a.pages = p;
        a.write = write;
        a.surface = surface;
        a.base = bp / 32;
        a.bw = bw;
        a.psm = psm;
        a.bp = bp;
        return a;
    };
    VramAccess writes[2];
    unsigned nw = 0;
    VramAccess reads[9];  // FRAME (só DATE com FRAME mascarado), ZBUF e até 7 níveis de textura
    unsigned nr = 0;
    // FRAME todo mascarado: não é escrito (entra como escrita vazia, para writes[0] continuar sendo
    // o FRAME); com DATE o alfa de destino ainda é lido.
    writes[nw++] = access(e.fbNone ? PageSet{} : pageSet(e.fpsm, e.fbp, e.fbw, rowLo, rowHi, colLo, colHi), true, 0,
                          e.fbp, e.fbw, e.fpsm);
    if (e.fbNone && e.date) {
        reads[nr++] = access(pageSet(e.fpsm, e.fbp, e.fbw, rowLo, rowHi, colLo, colHi), false, 0, e.fbp, e.fbw, e.fpsm);
    }
    if (e.zte && !e.zmsk) {
        writes[nw++] = access(pageSet(e.zpsm, e.zbp, e.fbw, rowLo, rowHi, colLo, colHi), true, 1, e.zbp, e.fbw, e.zpsm);
    } else if (e.zte && e.ztst >= 2) {
        reads[nr++] = access(pageSet(e.zpsm, e.zbp, e.fbw, rowLo, rowHi, colLo, colHi), false, 1, e.zbp, e.fbw, e.zpsm);
    }
    // Nível 0: o retângulo que o primitivo de fato busca (texReach0), quando conhecido; os
    // demais níveis ficam com o alcance cheio. texIdx = posição do acesso do nível 0.
    unsigned texIdx = ~0u;
    TexRect texRect;
    if (e.tme) {
        for (unsigned l = 0; l <= e.mxl; ++l) {
            const unsigned wl = std::max(1u, (1u << e.tw) >> l), hl = std::max(1u, (1u << e.th) >> l);
            // Maior coordenada + 1 por modo (fetchTexel): REPEAT/CLAMP ficam no
            // tamanho do nível; REGION_CLAMP vai até MAX >> nível; REGION_REPEAT
            // é (c & MSK) | FIX, no máximo MSK | FIX. O endereço é cortado em 2047.
            auto reach = [l](unsigned mode, unsigned size, unsigned mn, unsigned mx) {
                const unsigned r = mode == 2 ? (std::max(mn, mx) >> l) + 1 : mode == 3 ? (mn | mx) + 1 : size;
                return std::min(std::max(r, 1u), 2048u);
            };
            const unsigned colsU = reach(e.wms, wl, e.minu, e.maxu);
            const unsigned rowsV = reach(e.wmt, hl, e.minv, e.maxv);
            TexRect r{0, colsU - 1, 0, rowsV - 1};
            if (l == 0) {
                ReachParams rp;
                rp.type = e.type;
                rp.tw = e.tw;
                rp.th = e.th;
                rp.wms = e.wms;
                rp.wmt = e.wmt;
                rp.minu = e.minu;
                rp.maxu = e.maxu;
                rp.minv = e.minv;
                rp.maxv = e.maxv;
                rp.fst = e.fst;
                rp.linear = e.mmag || e.mmin == 1 || e.mmin >= 4;
                rp.fullU = colsU;
                rp.fullV = rowsV;
                TexRect x;
                if (texReach0(rp, w, v0, v1, v2, x)) {
                    r = x;
                    texIdx = nr;
                    texRect = x;
                    e.reachKnown = true;
                    e.reachU0 = x.u0;
                    e.reachU1 = x.u1;
                    e.reachV0 = x.v0;
                    e.reachV1 = x.v1;
                }
            }
            reads[nr++] = access(pageSet(e.tpsm, e.tbp[l], e.tbw[l], r.v0, r.v1, r.u0, r.u1), false, 2,
                                 e.tbp[l], e.tbw[l], e.tpsm);
        }
    }

    // Colisão dentro do próprio desenho: textura sobre o próprio FRAME/ZBUF, ou
    // FRAME e ZBUF sobrepostos. Pixels de faixas diferentes se leriam ou se
    // escreveriam na ordem errada, então o desenho vai sozinho.
    // Também vai sozinho o desenho cujo pixel→endereço não é injetivo: com x além
    // da largura do buffer (FBW·64; FBW = 0) o endereço dá a volta para as linhas
    // de baixo — (64, 0) e (0, 32) caem no mesmo lugar com FBW = 1 — e o mesmo
    // acontece se o buffer passa do fim da VRAM. Pixels de linhas (e faixas)
    // diferentes escreveriam o mesmo endereço sem ordem entre si.
    bool selfWrap = e.fbw == 0 || cols > e.fbw * 64;
    for (unsigned i = 0; i < nw; ++i) {
        selfWrap = selfWrap || writes[i].pages.full();
    }
    // Leitura e escrita no mesmo buffer (mesma base, largura e função pixel→endereço, com o
    // endereço injetivo: sem volta de largura nem do fim da VRAM) comparam-se em pixels: o texel
    // (u, v) é o pixel (u, v). Retângulos disjuntos não se tocam, mesmo na mesma página.
    auto reachDisjoint = [&](const VramAccess& rd, const VramAccess& wr) {
        if (texIdx == ~0u || e.fbw == 0 || rd.bp != wr.bp || rd.bw != wr.bw) return false;
        const unsigned key = mapKey(e.tpsm);
        if (key == 0 || key != mapKey(wr.psm)) return false;
        if (texRect.u1 >= wr.bw * 64) return false;
        const unsigned pageH = key >= 3 ? 64u : 32u;
        const std::uint64_t tiles = std::max(texRect.v1, rowHi) / pageH + 1;
        if (std::uint64_t{wr.bp} + tiles * wr.bw * 32 > std::uint64_t{PageSet::kPages} * 32) return false;
        return texRect.u1 < colLo || texRect.u0 > colHi || texRect.v1 < rowLo || texRect.v0 > rowHi;
    };
    bool selfTexture = false, selfFrameZ = false, reachSpared = false;
    for (unsigned i = 0; i < nw; ++i) {
        for (unsigned j = 0; j < nr; ++j) {
            if (!clashes(reads[j], writes[i])) continue;
            if (j == texIdx && reachDisjoint(reads[j], writes[i])) {
                reachSpared = true;  // as páginas se cruzam, mas os texels lidos não são os escritos
                continue;
            }
            (reads[j].surface == 2 ? selfTexture : selfFrameZ) = true;
        }
        for (unsigned j = i + 1; j < nw; ++j) selfFrameZ = selfFrameZ || clashes(writes[i], writes[j]);
    }
    // Só diagnóstico (contadores de CLUT): faixas que escrevem e caixa dos acessos.
    const std::uint32_t drawLanes = laneMask(rows.lo, rows.hi, worker_->lanes());
    for (unsigned i = 0; i < nw + nr; ++i) {
        VramAccess& a = i < nw ? writes[i] : reads[i - nw];
        a.laneMask = drawLanes;
        if (i < nw || a.surface == 1) {
            a.rx0 = static_cast<std::uint16_t>(std::min(colLo, 2047u));
            a.ry0 = static_cast<std::uint16_t>(std::min(rowLo, 2047u));
            a.rx1 = static_cast<std::uint16_t>(std::min(colHi, 2047u));
            a.ry1 = static_cast<std::uint16_t>(std::min(rowHi, 2047u));
            a.rectValid = !a.pages.full();
        }
    }
    // Gerações de página (bumpPages): as escritas deste desenho entram na ordem do
    // programa, antes de qualquer desenho seguinte. A simulação do cache olha antes,
    // com as gerações que a textura tinha quando o desenho a fixou. Desenhos sozinhos
    // (selfX) também escrevem; só não usam cache.
    const bool anySelf = selfWrap || selfTexture || selfFrameZ;
    // Desenho que só roda nas faixas porque o alcance exato da textura não cruza o que ele
    // escreve: as páginas se cruzam, e o preenchimento do cache por bloco (que pode pegar pixels
    // que outra faixa está escrevendo) não é seguro. Fica sem cache: a leitura vai direto à
    // VRAM, nos texels do retângulo, que ninguém escreve neste desenho.
    const bool noCache = anySelf || reachSpared;
    // HOST→LOCAL em modo fila com palavras ainda no buffer do produtor: se este desenho toca
    // o destino, elas entram na fila antes dele (ele as vê, e os desenhos seguintes também;
    // uma escrita dele não é sobrescrita por palavras anteriores a ele). Tem de vir antes
    // de qualquer versão de textura ser fixada: a submissão pode trocar as gerações.
    flushHostQueueIfTouches(writes, nw);
    flushHostQueueIfTouches(reads, nr);
    if (e.tme && (HostProfile::enabled() || texSimForced_)) {
        simTexture(e, selfWrap      ? static_cast<int>(TexSimReport::OffWrap)
                      : selfTexture || reachSpared ? static_cast<int>(TexSimReport::OffTexture)
                      : selfFrameZ  ? static_cast<int>(TexSimReport::OffFrameZ)
                                    : -1);
    }
    if (texCache_ && e.tme) {
        // Antes das escritas deste desenho: a versão de cada nível é a que valia quando
        // ele foi enfileirado. Desenhos sozinhos ficam sem cache (nível nulo).
        e.drawId = drawCount_;
        if (noCache) {
            ++texCacheOff_;
        } else {
            ++texCacheDraws_;
            for (unsigned l = 0; l <= e.mxl; ++l) e.tex[l] = texLevelFor(e, l);
        }
    }
    bumpPages(writes[0].pages, WkFrame);
    if (nw > 1) bumpPages(writes[1].pages, WkZbuf);
    if (e.tme && !noCache && hostOpenValid_) {
        // A versão da textura fica fixada por este desenho; se a transferência HOST→LOCAL
        // aberta ainda vai escrever nela, a próxima palavra invalida a versão. O teste
        // usa o alcance de leitura do desenho (reads[]), que com REGION_CLAMP/REGION_REPEAT
        // difere da grade w×h da versão; vale porque só o que o desenho alcança pode ser
        // preenchido por ele: um bloco cabe numa página lógica, e o alcance cobre toda
        // página que tenha um texel alcançado (fora da grade a leitura vai direto à VRAM).
        for (unsigned j = 0; j < nr; ++j) {
            if (reads[j].surface == 2 && accessesOverlap(hostOpen_, reads[j])) {
                hostGenDirty_ = true;
                break;
            }
        }
    }
    if (reachSpared && !anySelf) ++bandStats_.reachSaved;
    if (anySelf) {
        // Um único motivo por desenho, na ordem de prioridade abaixo.
        if (selfWrap) {
            ++bandStats_.self[BandStats::SelfWrap];
        } else if (selfTexture) {
            ++bandStats_.self[BandStats::SelfTexture];
            bandStats_.selfPairs.add(e.fbp / 32, e.tbp[0]);
        } else {
            ++bandStats_.self[BandStats::SelfFrameZ];
        }
        // Para quem acessa a VRAM direto do produtor (trackOutstanding): a barreira
        // do próprio desenho conclui estes acessos.
        for (unsigned i = 0; i < nw; ++i) noteOutstanding(writes[i]);
        for (unsigned j = 0; j < nr; ++j) noteOutstanding(reads[j]);
        submit([this, e, v0, v1, v2] { rasterize(e, v0, v1, v2); }, BandStats::SubmitDraw);
        noteHostHazard(writes, nw);
        noteHostHazard(reads, nr);
        if (traced) trace_->endDraw(*this);
        return;
    }

    // Colisão com o que está pendente: barreira antes do desenho (as faixas
    // terminam o que já têm). Depois, o desenho entra como pendente.
    bool conflict = false;
    if (pending_.full()) {
        conflict = true;
        ++bandStats_.conflict[BandStats::ConflictFull];
    }
    for (unsigned i = 0; i < nw && !conflict; ++i) {
        if (pending_.clashesWith(writes[i])) {
            conflict = true;
            ++bandStats_.conflict[BandStats::ConflictWrite];
        }
    }
    for (unsigned j = 0; j < nr && !conflict; ++j) {
        if (pending_.clashesWith(reads[j])) {
            conflict = true;
            ++bandStats_.conflict[BandStats::ConflictTexture];
            if (reads[j].surface == 2) bandStats_.conflictPairs.add(e.fbp / 32, e.tbp[0]);
        }
    }
    // Só conta como escrita depois de leitura de CLUT a que entraria no pending_ sem
    // barreira: se este desenho já abre uma barreira por outro motivo, a escrita viria
    // depois dela e, sem a barreira de CLUT, não criaria barreira nenhuma a mais.
    if (!conflict && !clutReads_.empty()) noteClutWar(writes, nw);
    if (conflict) {
        clearPending();
        ++bandStats_.barrierKind[BandStats::BkConflict];
        worker_->barrier([] {}, BandStats::BkConflict);
    }
    for (unsigned i = 0; i < nw; ++i) pending_.add(writes[i]);
    for (unsigned j = 0; j < nr; ++j) pending_.add(reads[j]);
    // A próxima barreira que vier depois destas tarefas conclui estes acessos.
    for (unsigned i = 0; i < nw; ++i) noteOutstanding(writes[i]);
    for (unsigned j = 0; j < nr; ++j) noteOutstanding(reads[j]);

    // Uma tarefa só para as faixas que têm linhas do desenho; cada uma recebe o
    // seu índice e sombreia só as suas linhas.
    const std::uint32_t mask = drawLanes;
    const auto task = std::make_shared<const GsWorker::Task>(
        [this, e, v0, v1, v2](unsigned lane, unsigned lanes) {
            DrawEnv d = e;
            d.lane = lane;
            d.lanes = lanes;
            rasterize(d, v0, v1, v2);
        });
    ++bandStats_.bandDraws;
    for (unsigned lane = 0; lane < worker_->lanes(); ++lane) {
        if (mask & (1u << lane)) {
            ++bandStats_.lanesTouched;
            worker_->push(lane, task);
        }
    }
    noteHostHazard(writes, nw);
    noteHostHazard(reads, nr);
    if (traced) trace_->endDraw(*this);
}

// ---------------------------------------------------------------------------
// Texturas
// ---------------------------------------------------------------------------

// Simulação do cache de texturas (só produtor, só com perfil ou nos testes): a chave
// é o formato, o tamanho e a base/largura de cada nível usado; o valor, as gerações
// das páginas que a textura cobre na última vez. Igual = acerto. Sem texels, sem
// atômicos: custa só o que o produtor já faz por desenho texturizado.
void Gs::simTexture(const DrawEnv& e, int offReason) {
    ++texSim_.texDraws;
    if (offReason >= 0) {
        ++texSim_.off[offReason];
        return;
    }
    TexSimKey key;
    key.v[0] = e.tpsm;
    key.v[1] = e.tw;
    key.v[2] = e.th;
    key.v[3] = e.mxl;
    PageSet pages;
    for (unsigned l = 0; l <= e.mxl && l < 7; ++l) {
        key.v[4 + l] = e.tbp[l];
        key.v[11 + l] = e.tbw[l];
        const unsigned wl = std::max(1u, (1u << e.tw) >> l), hl = std::max(1u, (1u << e.th) >> l);
        pages.merge(pageSet(e.tpsm, e.tbp[l], e.tbw[l], 0, hl - 1, 0, wl - 1));
    }
    std::vector<std::uint32_t>& cur = texSimScratch_;
    cur.clear();
    for (unsigned i = 0; i < PageSet::kWords; ++i) {
        for (std::uint64_t m = pages.w[i]; m; m &= m - 1)
            cur.push_back(pageState_[i * 64 + static_cast<unsigned>(std::countr_zero(m))].gen);
    }
    constexpr std::size_t kMaxKeys = 8192;  // simulação: limite grosseiro, recomeça se passar
    if (texSimSeen_.size() >= kMaxKeys) texSimSeen_.clear();
    auto [it, inserted] = texSimSeen_.try_emplace(key);
    std::vector<std::uint32_t>& old = it->second;
    if (inserted) {
        ++texSim_.miss[TexSimReport::MissFirst];
        old = cur;
        return;
    }
    if (old == cur) {
        ++texSim_.hits;
        return;
    }
    // Motivo: a escrita mais recente entre as páginas cuja geração mudou.
    std::uint64_t bestEpoch = 0;
    std::uint32_t kind = WkFrame;
    std::size_t n = 0;
    for (unsigned i = 0; i < PageSet::kWords; ++i) {
        for (std::uint64_t m = pages.w[i]; m; m &= m - 1, ++n) {
            const PageState& s = pageState_[i * 64 + static_cast<unsigned>(std::countr_zero(m))];
            if (n < old.size() && old[n] != s.gen && s.epoch >= bestEpoch) {
                bestEpoch = s.epoch;
                kind = s.kind;
            }
        }
    }
    static constexpr TexSimReport::Miss kMap[] = {TexSimReport::MissFrame, TexSimReport::MissZbuf,
                                                  TexSimReport::MissHost, TexSimReport::MissCopy};
    ++texSim_.miss[kMap[kind & 3]];
    old = cur;
}

// Palavra crua do texel: o wrap (REPEAT/CLAMP/REGION_*) e a leitura da VRAM no
// formato da textura, sem CLUT nem TEXA. Separada da expansão para que um cache de
// texels crus (e a paleta memorizada) possa trocar só a segunda parte.
std::uint32_t Gs::texelRaw(const DrawEnv& e, unsigned level, int u, int v) const {
    const int w = std::max(1, (1 << e.tw) >> level);
    const int h = std::max(1, (1 << e.th) >> level);
    auto wrap = [](int c, int size, unsigned mode, unsigned mn, unsigned mx, unsigned lvl) {
        switch (mode) {
            case 0: return c & (size - 1);                                 // REPEAT
            case 1: return std::clamp(c, 0, size - 1);                     // CLAMP
            case 2: return std::clamp(c, static_cast<int>(mn >> lvl), static_cast<int>(mx >> lvl));  // REGION_CLAMP
            default: return (c & static_cast<int>(mn)) | static_cast<int>(mx);  // REGION_REPEAT
        }
    };
    u = wrap(u, w, e.wms, e.minu, e.maxu, level);
    v = wrap(v, h, e.wmt, e.minv, e.maxv, level);
    const auto x = static_cast<std::uint32_t>(u) & 2047, y = static_cast<std::uint32_t>(v) & 2047;
    if (texVerify_ && level == 0 && e.reachKnown && (x < e.reachU0 || x > e.reachU1 || y < e.reachV0 || y > e.reachV1)) {
        verifyMismatches_.fetch_add(1, std::memory_order_relaxed);
        if (verifyAbort_) {
            std::fprintf(stderr,
                         "[texcache VERIFY] desenho %llu leu o texel (%u, %u) fora do alcance declarado [%u..%u]x[%u..%u]\n",
                         static_cast<unsigned long long>(e.drawId), x, y, e.reachU0, e.reachU1, e.reachV0, e.reachV1);
            std::fflush(stderr);
            std::abort();
        }
    }
    if (const TexLevel* t = e.tex[level].get()) {
        std::uint32_t raw = 0;
        const TexLevel::Got got = t->get(vram_, x, y, raw);
        if (HostProfile::enabled()) {
            LaneTex& lt = laneTex_[e.lane];
            bump(got == TexLevel::Got::Hit ? lt.cacheHit : got == TexLevel::Got::Filled ? lt.cacheFill : lt.cacheFallback);
        }
        if (got != TexLevel::Got::Outside) {
            if (texVerify_) {
                const std::uint32_t direct = vram_.readPixel(e.tpsm, e.tbp[level], e.tbw[level], x, y);
                if (direct != raw) texVerifyFail(e, level, x, y, raw, direct);
            }
            return raw;
        }
    }
    return vram_.readPixel(e.tpsm, e.tbp[level], e.tbw[level], x, y);
}

// VERIFY: o cache devolveu um valor diferente da VRAM. Imprime o desenho, o nível e
// o texel e, salvo nos testes (setTexVerifyAbort(false)), aborta na primeira diferença.
void Gs::texVerifyFail(const DrawEnv& e, unsigned level, unsigned x, unsigned y, std::uint32_t cached,
                       std::uint32_t direct) const {
    verifyMismatches_.fetch_add(1, std::memory_order_relaxed);
    if (!verifyAbort_) return;
    std::fprintf(stderr,
                 "[texcache VERIFY] diferença no desenho %llu, nível %u, texel (%u, %u): cache 0x%08X, VRAM 0x%08X "
                 "(PSM 0x%02X, TBP %u, TBW %u, %ux%u)\n",
                 static_cast<unsigned long long>(e.drawId), level, x, y, cached, direct, e.tpsm, e.tbp[level],
                 e.tbw[level], std::max(1u, (1u << e.tw) >> level), std::max(1u, (1u << e.th) >> level));
    std::fflush(stderr);
    std::abort();
}

// Versão do nível no cache para este desenho (produtor). A versão corrente vale se
// nenhuma escrita ocorreu desde que as gerações das suas páginas foram conferidas
// (writeEpoch_ igual) ou se as gerações continuam iguais; senão nasce uma versão nova.
std::shared_ptr<TexLevel> Gs::texLevelFor(const DrawEnv& e, unsigned level) {
    const unsigned wl = std::max(1u, (1u << e.tw) >> level), hl = std::max(1u, (1u << e.th) >> level);
    const TexKey key{e.tpsm, e.tbp[level], e.tbw[level], wl, hl};
    TexCache::Entry* en = texCache_->find(key);
    if (en && en->validEpoch == writeEpoch_) {
        texCache_->noteReuse();
        return en->level;
    }
    PageSet fresh;
    if (!en) fresh = pageSet(e.tpsm, e.tbp[level], e.tbw[level], 0, hl - 1, 0, wl - 1);
    const PageSet& pages = en ? en->pages : fresh;
    std::vector<std::uint32_t>& cur = texGenScratch_;
    cur.clear();
    for (unsigned i = 0; i < PageSet::kWords; ++i) {
        for (std::uint64_t m = pages.w[i]; m; m &= m - 1) {
            cur.push_back(pageState_[i * 64 + static_cast<unsigned>(std::countr_zero(m))].gen);
        }
    }
    if (en && en->gens == cur) {
        en->validEpoch = writeEpoch_;
        texCache_->noteReuse();
        return en->level;
    }
    const PageSet keep = pages;  // `pages` pode apontar para a entrada que create substitui
    return texCache_->create(key, keep, cur, writeEpoch_).level;
}

TexCacheReport Gs::texCacheReport() const {
    TexCacheReport r;
    if (!texCache_) return r;
    r.enabled = true;
    for (const LaneTex& l : laneTex_) {
        r.blockHits += l.cacheHit.load(std::memory_order_relaxed);
        r.blockFills += l.cacheFill.load(std::memory_order_relaxed);
        r.fallbacks += l.cacheFallback.load(std::memory_order_relaxed);
    }
    const TexCache::Stats& st = texCache_->stats();
    r.texDraws = texCacheDraws_;
    r.offDraws = texCacheOff_;
    r.versions = st.versions;
    r.reused = st.reused;
    r.evictions = st.evictions;
    r.bytes = st.bytes;
    r.peakBytes = st.peakBytes;
    r.entries = st.entries;
    r.liveBytes = texCache_->liveBytes();
    r.peakLiveBytes = texCache_->peakLiveBytes();
    r.budget = texCache_->budget();
    return r;
}

// Palavra crua -> cor RGBA de 32 bits: TEXA nos formatos sem alfa de 8 bits, CLUT
// nos indexados.
std::uint32_t Gs::expandTexel(const DrawEnv& e, std::uint32_t raw) {
    switch (e.tpsm) {
        case PSMCT32: case PSMZ32: return raw;
        case PSMCT24: case PSMZ24: {
            const std::uint32_t a = (e.aem && raw == 0) ? 0 : e.ta0;
            return raw | (a << 24);
        }
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
            return expand16(raw, e.ta0, e.ta1, e.aem);
        default: {
            // Índice na CLUT. O CSA desloca a entrada em 16 em qualquer formato
            // indexado, como na carga (loadClut); no CPSM=32 dá a volta em 256.
            const unsigned idx = e.csa * 16 + raw;
            if (e.cpsm == PSMCT32) {
                const unsigned i = idx & 255;
                return static_cast<std::uint32_t>(e.clut->v[i]) | (static_cast<std::uint32_t>(e.clut->v[i + 256]) << 16);
            }
            return expand16(e.clut->v[idx & 511], e.ta0, e.ta1, e.aem);
        }
    }
}

// Paleta expandida da faixa para a CLUT deste desenho, reconstruída só quando a chave
// (versão da CLUT, CPSM, CSA, TEXA/AEM) muda. O TEXA não entra na chave com CPSM=32
// (a entrada de 32 bits já traz o alfa). Exige a CLUT resolvida.
const std::uint32_t* Gs::palette(const DrawEnv& e, unsigned n) const {
    LanePal& m = lanePal_[e.lane];
    const bool texa = e.cpsm != PSMCT32;
    const std::uint32_t ta0 = texa ? e.ta0 : 0, ta1 = texa ? e.ta1 : 0;
    const bool aem = texa && e.aem;
    if (m.n < n || m.clut != e.clut || m.cpsm != e.cpsm || m.csa != e.csa || m.ta0 != ta0 || m.ta1 != ta1 ||
        m.aem != aem) {
        m.clut = e.clut;
        m.cpsm = e.cpsm;
        m.csa = e.csa;
        m.ta0 = ta0;
        m.ta1 = ta1;
        m.aem = aem;
        m.n = n;
        ++m.builds;
        for (unsigned i = 0; i < n; ++i) m.pal[i] = expandTexel(e, i);
    }
    return m.pal.data();
}

std::uint32_t Gs::fetchTexel(const DrawEnv& e, unsigned level, int u, int v) const {
    if (HostProfile::enabled()) bump(laneTex_[e.lane].fetch[texFmtClass(e.tpsm)]);
    const std::uint32_t raw = texelRaw(e, level, u, v);
    return e.pal ? e.pal[raw] : expandTexel(e, raw);
}

void Gs::fillEnv(DrawEnv& e, const TexelProbe& p) {
    for (unsigned i = 0; i < 7; ++i) {
        e.tbp[i] = p.tbp[i];
        e.tbw[i] = p.tbw[i];
    }
    e.tpsm = p.tpsm;
    e.tw = p.tw;
    e.th = p.th;
    e.cpsm = p.cpsm;
    e.csa = p.csa;
    e.clut = p.clut;
    e.wms = p.wms;
    e.wmt = p.wmt;
    e.minu = p.minu;
    e.maxu = p.maxu;
    e.minv = p.minv;
    e.maxv = p.maxv;
    e.ta0 = p.ta0;
    e.ta1 = p.ta1;
    e.aem = p.aem;
}

// Só para testes (gs_texel): monta um DrawEnv só com os campos da textura.
std::uint32_t Gs::probeTexel(const TexelProbe& p, unsigned level, int u, int v) {
    DrawEnv e;
    fillEnv(e, p);
    if (e.clut && isIndexed(e.tpsm)) e.pal = palette(e, isT4(e.tpsm) ? 16u : 256u);
    if (texCache_) e.tex[level] = texLevelFor(e, level);
    return fetchTexel(e, level, u, v);
}

void Gs::probePalette(const TexelProbe& p, unsigned n, std::uint32_t* out) const {
    DrawEnv e;
    fillEnv(e, p);
    const std::uint32_t* pal = palette(e, n);
    for (unsigned i = 0; i < n; ++i) out[i] = pal[i];
}

std::uint32_t Gs::sampleTexture(const DrawEnv& e, float u, float v, float lod) const {
    // u, v em texels do nível 0.
    LaneTex& lt = laneTex_[e.lane];
    const bool prof = HostProfile::enabled();
    const SampledTimer timer(lt.samples, lt.sampleTimed, lt.sampleNs, prof);
    bool linear = e.mmag;
    unsigned level = 0;
    unsigned level2 = 0;
    float blend = 0;
    if (lod > 0) {
        switch (e.mmin) {
            case 0: linear = false; break;
            case 1: linear = true; break;
            default: {
                linear = e.mmin >= 4;
                const float maxl = static_cast<float>(e.mxl);
                const float l = std::clamp(lod, 0.0f, maxl);
                if (e.mmin == 2 || e.mmin == 4) {
                    level = static_cast<unsigned>(std::min(maxl, std::floor(l + 0.5f)));
                } else {
                    level = static_cast<unsigned>(std::floor(l));
                    level2 = std::min<unsigned>(level + 1, e.mxl);
                    blend = l - std::floor(l);
                }
                break;
            }
        }
    }
    if (prof) {
        if (linear) bump(lt.linear);
        if (blend > 0 && level2 != level) bump(lt.trilinear);
    }
    auto sampleLevel = [&](unsigned lvl) -> std::uint32_t {
        const float scale = 1.0f / static_cast<float>(1u << lvl);
        const float lu = u * scale, lv = v * scale;
        if (!linear) {
            return fetchTexel(e, lvl, static_cast<int>(std::floor(lu)), static_cast<int>(std::floor(lv)));
        }
        const float fu = lu - 0.5f, fv = lv - 0.5f;
        const int iu = static_cast<int>(std::floor(fu)), iv = static_cast<int>(std::floor(fv));
        // Pesos em 1/16 de texel, como o filtro do GS.
        const int wu = static_cast<int>((fu - static_cast<float>(iu)) * 16.0f);
        const int wv = static_cast<int>((fv - static_cast<float>(iv)) * 16.0f);
        const std::uint32_t t00 = fetchTexel(e, lvl, iu, iv), t10 = fetchTexel(e, lvl, iu + 1, iv);
        const std::uint32_t t01 = fetchTexel(e, lvl, iu, iv + 1), t11 = fetchTexel(e, lvl, iu + 1, iv + 1);
        std::uint32_t out = 0;
        for (unsigned s = 0; s < 32; s += 8) {
            const int c00 = static_cast<int>((t00 >> s) & 0xFF), c10 = static_cast<int>((t10 >> s) & 0xFF);
            const int c01 = static_cast<int>((t01 >> s) & 0xFF), c11 = static_cast<int>((t11 >> s) & 0xFF);
            const int top = c00 * (16 - wu) + c10 * wu;
            const int bot = c01 * (16 - wu) + c11 * wu;
            const int c = (top * (16 - wv) + bot * wv) >> 8;
            out |= static_cast<std::uint32_t>(c) << s;
        }
        return out;
    };
    const std::uint32_t c1 = sampleLevel(level);
    if (blend <= 0 || level2 == level) return c1;
    const std::uint32_t c2 = sampleLevel(level2);
    const int wb = static_cast<int>(blend * 16.0f);
    std::uint32_t out = 0;
    for (unsigned s = 0; s < 32; s += 8) {
        const int a = static_cast<int>((c1 >> s) & 0xFF), b = static_cast<int>((c2 >> s) & 0xFF);
        out |= static_cast<std::uint32_t>((a * (16 - wb) + b * wb) >> 4) << s;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Pipeline de pixel
// ---------------------------------------------------------------------------

void Gs::shadePixel(const DrawEnv& e, int x, int y, Fragment& f) {
    if (!e.owns(y)) return;  // linha de outra faixa
    if (x < e.scax0 || x > e.scax1 || y < e.scay0 || y > e.scay1) return;
    if ((e.scanmsk == 2 && (y & 1)) || (e.scanmsk == 3 && !(y & 1))) return;
    // Mesmo ponto em que gs_coverage conta: confere a conta analítica.
    ++laneShaded_[e.lane].shaded;
    LaneTex& lt = laneTex_[e.lane];
    const SampledTimer shadeTimer(lt.shadeCalls, lt.shadeTimed, lt.shadeNs, HostProfile::enabled());
    int r = f.r, g = f.g, b = f.b, a = f.a;

    if (e.tme) {
        float u, v, lod;
        if (e.fst) {
            u = f.u;
            v = f.v;
            lod = e.lodK;
        } else {
            const float q = f.q;
            u = f.s / q * static_cast<float>(1u << e.tw);
            v = f.t / q * static_cast<float>(1u << e.th);
            lod = e.lcm || !e.lodMatters
                      ? e.lodK
                      : std::log2(1.0f / std::fabs(q)) * static_cast<float>(1u << e.lodL) + e.lodK;
        }
        const std::uint32_t t = sampleTexture(e, u, v, lod);
        const int tr = static_cast<int>(t & 0xFF), tg = static_cast<int>((t >> 8) & 0xFF);
        const int tb = static_cast<int>((t >> 16) & 0xFF), ta = static_cast<int>(t >> 24);
        switch (e.tfx) {
            case 0:  // MODULATE
                r = std::min(255, (r * tr) >> 7);
                g = std::min(255, (g * tg) >> 7);
                b = std::min(255, (b * tb) >> 7);
                if (e.tcc) a = std::min(255, (a * ta) >> 7);
                break;
            case 1:  // DECAL
                r = tr;
                g = tg;
                b = tb;
                if (e.tcc) a = ta;
                break;
            case 2:  // HIGHLIGHT
                r = std::min(255, ((r * tr) >> 7) + a);
                g = std::min(255, ((g * tg) >> 7) + a);
                b = std::min(255, ((b * tb) >> 7) + a);
                if (e.tcc) a = std::min(255, ta + a);
                break;
            default:  // HIGHLIGHT2
                r = std::min(255, ((r * tr) >> 7) + a);
                g = std::min(255, ((g * tg) >> 7) + a);
                b = std::min(255, ((b * tb) >> 7) + a);
                if (e.tcc) a = ta;
                break;
        }
    }
    if (e.fge) {
        r = (f.fog * r + (255 - f.fog) * e.fogR) >> 8;
        g = (f.fog * g + (255 - f.fog) * e.fogG) >> 8;
        b = (f.fog * b + (255 - f.fog) * e.fogB) >> 8;
    }

    // Teste de alfa
    // Sem o teste de Z (ZTE=0) o Z não é escrito, como no GSdx: muitos jogos
    // deixam o ZBUF apontando para o próprio FRAME nos desenhos 2D.
    bool writeFb = true, writeZ = e.zte && !e.zmsk, writeAlpha = true;
    if (e.ate) {
        bool pass = true;
        const auto ua = static_cast<unsigned>(a);
        switch (e.atst) {
            case 0: pass = false; break;
            case 1: pass = true; break;
            case 2: pass = ua < e.aref; break;
            case 3: pass = ua <= e.aref; break;
            case 4: pass = ua == e.aref; break;
            case 5: pass = ua >= e.aref; break;
            case 6: pass = ua > e.aref; break;
            default: pass = ua != e.aref; break;
        }
        if (!pass) {
            switch (e.afail) {
                case 0: return;                          // KEEP
                case 1: writeZ = false; break;           // FB_ONLY
                case 2: writeFb = false; break;          // ZB_ONLY
                default: writeZ = false; writeAlpha = false; break;  // RGB_ONLY
            }
        }
    }

    const auto ux = static_cast<std::uint32_t>(x), uy = static_cast<std::uint32_t>(y);
    const unsigned fbits = psmStorageBits(e.fpsm);
    // FRAME todo mascarado e sem DATE: o destino não é lido (o desenho não declara acesso ao FRAME).
    const std::uint32_t dst = e.fbNone && !e.date ? 0u : vram_.readPixel(e.fpsm, e.fbp, e.fbw, ux, uy);

    // Teste de alfa de destino
    if (e.date) {
        const bool bit = fbits == 16 ? (dst & 0x8000) != 0 : (dst & 0x80000000u) != 0;
        if (e.fpsm != PSMCT24 && e.fpsm != PSMZ24 && bit != e.datm) return;
    }

    // Teste de Z (ZTE=0 desliga o teste e a escrita de Z)
    std::uint32_t z = f.z;
    if (e.zpsm == PSMZ24) z = std::min(z, 0xFFFFFFu);
    else if (e.zpsm == PSMZ16 || e.zpsm == PSMZ16S) z = std::min(z, 0xFFFFu);
    if (e.zte) {
        switch (e.ztst) {
            case 0: return;
            case 1: break;
            case 2:
                if (z < vram_.readPixel(e.zpsm, e.zbp, e.fbw, ux, uy)) return;
                break;
            default:
                if (z <= vram_.readPixel(e.zpsm, e.zbp, e.fbw, ux, uy)) return;
                break;
        }
    }

    if (writeFb && !e.fbNone) {
        // Destino como RGBA8
        int dr, dg, db, da;
        if (fbits == 16) {
            dr = static_cast<int>((dst & 0x1F) << 3);
            dg = static_cast<int>(((dst >> 5) & 0x1F) << 3);
            db = static_cast<int>(((dst >> 10) & 0x1F) << 3);
            da = (dst & 0x8000) ? 0x80 : 0;
        } else {
            dr = static_cast<int>(dst & 0xFF);
            dg = static_cast<int>((dst >> 8) & 0xFF);
            db = static_cast<int>((dst >> 16) & 0xFF);
            da = (e.fpsm == PSMCT24 || e.fpsm == PSMZ24) ? 0x80 : static_cast<int>(dst >> 24);
        }
        if (e.abe && (!e.pabe || (a & 0x80))) {
            auto sel = [&](unsigned s, int cs, int cd) { return s == 0 ? cs : (s == 1 ? cd : 0); };
            const int c = e.bc == 0 ? a : (e.bc == 1 ? da : e.bfix);
            auto blendCh = [&](int cs, int cd) {
                return (((sel(e.ba, cs, cd) - sel(e.bb, cs, cd)) * c) >> 7) + sel(e.bd, cs, cd);
            };
            r = blendCh(r, dr);
            g = blendCh(g, dg);
            b = blendCh(b, db);
        }
        if (e.dthe && fbits == 16) {
            const int d = e.dimx[y & 3][x & 3];
            r += d;
            g += d;
            b += d;
        }
        if (e.colclamp) {
            r = clamp255(r);
            g = clamp255(g);
            b = clamp255(b);
        } else {
            r &= 0xFF;
            g &= 0xFF;
            b &= 0xFF;
        }
        const int outA = e.fba ? (a | 0x80) : a;
        std::uint32_t value, mask = e.fbmsk;
        if (!writeAlpha) mask |= 0xFF000000u;
        if (fbits == 16) {
            value = static_cast<std::uint32_t>((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((outA & 0x80) << 8));
            const std::uint32_t m16 = ((mask >> 3) & 0x1F) | (((mask >> 11) & 0x1F) << 5) |
                                      (((mask >> 19) & 0x1F) << 10) | ((mask >> 16) & 0x8000);
            value = (dst & m16) | (value & ~m16 & 0xFFFF);
        } else {
            value = static_cast<std::uint32_t>(r) | (static_cast<std::uint32_t>(g) << 8) |
                    (static_cast<std::uint32_t>(b) << 16) | (static_cast<std::uint32_t>(outA & 0xFF) << 24);
            value = (dst & mask) | (value & ~mask);
            if (e.fpsm == PSMCT24 || e.fpsm == PSMZ24) value &= 0xFFFFFFu;
        }
        vram_.writePixel(e.fpsm, e.fbp, e.fbw, ux, uy, value);
    }
    if (writeZ) vram_.writePixel(e.zpsm, e.zbp, e.fbw, ux, uy, z);
}

// ---------------------------------------------------------------------------
// Primitivas
// ---------------------------------------------------------------------------

namespace {

// Atributos interpoláveis de um vértice em ponto flutuante.
struct Attr {
    double z, r, g, b, a, s, t, q, u, v, fog;
};

Attr attrOf(const Vertex& v) {
    return {static_cast<double>(v.z), static_cast<double>(v.r), static_cast<double>(v.g),
            static_cast<double>(v.b), static_cast<double>(v.a), static_cast<double>(v.s),
            static_cast<double>(v.t), static_cast<double>(v.q), static_cast<double>(v.u) / 16.0,
            static_cast<double>(v.v) / 16.0, static_cast<double>(v.fog)};
}

Attr lerp3(const Attr& a, const Attr& b, const Attr& c, double wa, double wb, double wc) {
    auto m = [&](double Attr::*f) { return a.*f * wa + b.*f * wb + c.*f * wc; };
    return {m(&Attr::z), m(&Attr::r), m(&Attr::g), m(&Attr::b), m(&Attr::a), m(&Attr::s),
            m(&Attr::t), m(&Attr::q), m(&Attr::u), m(&Attr::v), m(&Attr::fog)};
}

std::uint32_t toZ(double z) {
    if (z <= 0) return 0;
    if (z >= 4294967295.0) return 0xFFFFFFFFu;
    return static_cast<std::uint32_t>(z);
}

int toColor(double c) {
    return clamp255(static_cast<int>(c));
}

}  // namespace

void Gs::drawPoint(const DrawEnv& e, const Vertex& v) {
    Fragment f;
    const Attr a = attrOf(v);
    f.z = v.z;
    f.r = v.r;
    f.g = v.g;
    f.b = v.b;
    f.a = v.a;
    f.s = v.s;
    f.t = v.t;
    f.q = v.q;
    f.u = static_cast<float>(a.u);
    f.v = static_cast<float>(a.v);
    f.fog = v.fog;
    shadePixel(e, (v.x + 8) >> 4, (v.y + 8) >> 4, f);
}

void Gs::drawLine(const DrawEnv& e, const Vertex& v0, const Vertex& v1) {
    const int steps = lineSteps(v0, v1);
    const Attr a0 = attrOf(v0), a1 = attrOf(v1);
    for (int i = 0; i < std::max(steps, 1); ++i) {
        const LineSample s = lineSample(v0, v1, steps, i);
        const double t = s.t;
        const int px = s.px, py = s.py;
        const Attr at = lerp3(a0, a1, a1, 1 - t, t, 0);
        Fragment f;
        f.z = toZ(at.z);
        if (e.iip) {
            f.r = toColor(at.r);
            f.g = toColor(at.g);
            f.b = toColor(at.b);
            f.a = toColor(at.a);
        } else {
            f.r = v1.r;
            f.g = v1.g;
            f.b = v1.b;
            f.a = v1.a;
        }
        f.s = static_cast<float>(at.s);
        f.t = static_cast<float>(at.t);
        f.q = static_cast<float>(at.q);
        f.u = static_cast<float>(at.u);
        f.v = static_cast<float>(at.v);
        f.fog = toColor(at.fog);
        shadePixel(e, px, py, f);
    }
}

void Gs::drawTriangle(const DrawEnv& e, const Vertex& v0, const Vertex& v1, const Vertex& v2) {
    // Geometria (ordem, caixa, arestas e regra top-left) vem de gs_coverage, que
    // também conta os pixels; as duas coisas não podem divergir.
    const TriangleSetup t = triangleSetup(DrawWindow{e.scax0, e.scax1, e.scay0, e.scay1, e.scanmsk}, v0, v1, v2);
    if (!t.valid) return;
    const int minX = t.minX, maxX = t.maxX, minY = t.minY, maxY = t.maxY;
    const std::int64_t bias0 = t.bias[0], bias1 = t.bias[1], bias2 = t.bias[2];
    const Attr A = attrOf(t.v[0]), B = attrOf(t.v[1]), C = attrOf(t.v[2]);
    const double inv = 1.0 / static_cast<double>(t.area);
    const Vertex& last = v2;  // cor flat: último vértice
    // Só o que o pixel vai usar é interpolado (mesma fórmula do lerp3, então
    // os mesmos valores).
    const bool needST = e.tme && !e.fst, needUV = e.tme && e.fst;

    // Funções de aresta incrementais: cada pixel à direita soma dx, cada
    // linha abaixo soma dy (inteiros, mesmo valor de edge() no pixel).
    std::int64_t row0 = t.row[0], row1 = t.row[1], row2 = t.row[2];
    const std::int64_t dx0 = t.dx[0], dx1 = t.dx[1], dx2 = t.dx[2];
    const std::int64_t dy0 = t.dy[0], dy1 = t.dy[1], dy2 = t.dy[2];

    // Uma linha que não é da faixa pula só o sombreamento: os acumuladores de
    // aresta (row0..2) andam no cabeçalho do laço, então as linhas seguintes
    // ficam iguais às do desenho inteiro.
    for (int py = minY; py <= maxY; ++py, row0 += dy0, row1 += dy1, row2 += dy2) {
        if (!e.owns(py)) continue;
        std::int64_t w0 = row0, w1 = row1, w2 = row2;
        for (int px = minX; px <= maxX; ++px, w0 += dx0, w1 += dx1, w2 += dx2) {
            if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0) continue;
            const double wa = static_cast<double>(w0) * inv, wb = static_cast<double>(w1) * inv,
                         wc = static_cast<double>(w2) * inv;
            auto m = [&](double Attr::*fld) { return A.*fld * wa + B.*fld * wb + C.*fld * wc; };
            Fragment f;
            f.z = toZ(m(&Attr::z));
            if (e.iip) {
                f.r = toColor(m(&Attr::r));
                f.g = toColor(m(&Attr::g));
                f.b = toColor(m(&Attr::b));
                f.a = toColor(m(&Attr::a));
            } else {
                f.r = last.r;
                f.g = last.g;
                f.b = last.b;
                f.a = last.a;
            }
            if (needST) {
                f.s = static_cast<float>(m(&Attr::s));
                f.t = static_cast<float>(m(&Attr::t));
                f.q = static_cast<float>(m(&Attr::q));
            }
            if (needUV) {
                f.u = static_cast<float>(m(&Attr::u));
                f.v = static_cast<float>(m(&Attr::v));
            }
            if (e.fge) f.fog = toColor(m(&Attr::fog));
            shadePixel(e, px, py, f);
        }
    }
}

void Gs::drawSprite(const DrawEnv& e, const Vertex& v0, const Vertex& v1) {
    const SpriteRect r = spriteRect(DrawWindow{e.scax0, e.scax1, e.scay0, e.scay1, e.scanmsk}, v0, v1);
    if (r.empty()) return;
    const int minX = r.minX, maxX = r.maxX, minY = r.minY, maxY = r.maxY;
    const Attr a0 = attrOf(v0), a1 = attrOf(v1);
    const double spanX = v1.x - v0.x, spanY = v1.y - v0.y;
    // S/U só dependem da coluna e T/V só da linha: calculados uma vez.
    const auto cols = static_cast<std::size_t>(maxX - minX + 1);
    std::vector<float> colS(cols), colU(cols);
    for (int px = minX; px <= maxX; ++px) {
        const double tx = spanX != 0 ? (px * 16.0 - v0.x) / spanX : 0.0;
        colS[static_cast<std::size_t>(px - minX)] = static_cast<float>(a0.s + (a1.s - a0.s) * tx);
        colU[static_cast<std::size_t>(px - minX)] = static_cast<float>(a0.u + (a1.u - a0.u) * tx);
    }
    Fragment f;
    f.z = v1.z;
    f.r = v1.r;
    f.g = v1.g;
    f.b = v1.b;
    f.a = v1.a;
    f.fog = v1.fog;
    f.q = static_cast<float>(a1.q);
    for (int py = minY; py <= maxY; ++py) {
        if (!e.owns(py)) continue;
        const double ty = spanY != 0 ? (py * 16.0 - v0.y) / spanY : 0.0;
        const auto rowT = static_cast<float>(a0.t + (a1.t - a0.t) * ty);
        const auto rowV = static_cast<float>(a0.v + (a1.v - a0.v) * ty);
        for (int px = minX; px <= maxX; ++px) {
            Fragment p = f;
            p.s = colS[static_cast<std::size_t>(px - minX)];
            p.u = colU[static_cast<std::size_t>(px - minX)];
            p.t = rowT;
            p.v = rowV;
            shadePixel(e, px, py, p);
        }
    }
}

}  // namespace anyps2::rt::gs
