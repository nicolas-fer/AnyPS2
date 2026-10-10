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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/gs/gs_coverage.h"
#include "anyps2/runtime/gs/gs_trace.h"
#include "anyps2/runtime/host_profile.h"

namespace anyps2::rt::gs {

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
    // Frame/Z
    std::uint32_t fbp = 0, fbw = 0, fpsm = 0, fbmsk = 0;
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

void Gs::draw(std::uint32_t pc) {
    // Só o trabalho do EE entra no perfil: o HostProfile não é seguro entre
    // threads, e o rasterizador roda no worker.
    const HostProfile::Scope prof(HostProfile::Gs);
    DrawEnv e;
    setupEnv(e, pc);
    ++drawCount_;
    // O tempo do GS usa a contagem analítica, feita aqui na ordem do programa
    // (o worker ainda não desenhou nada disto).
    const DrawWindow w{e.scax0, e.scax1, e.scay0, e.scay1, e.scanmsk};
    const Vertex v0 = queue_[0], v1 = queue_[1], v2 = queue_[2];
    const std::uint64_t covered = coveredPixels(w, e.type, v0, v1, v2);
    pixels_ += covered;
    coveredTotal_ += covered;
    // Diagnóstico (gs_trace.h): sem ANYPS2_GS_PROBE/ANYPS2_GS_DRAWLOG o rastreador é nulo.
    const bool traced = trace_ && trace_->beginDraw(*this, pc, e.type, w, v0, v1, v2);
    switch (e.type) {
        case 0: submit([this, e, v0] { drawPoint(e, v0); }); break;
        case 1: case 2: submit([this, e, v0, v1] { drawLine(e, v0, v1); }); break;
        case 3: case 4: case 5: submit([this, e, v0, v1, v2] { drawTriangle(e, v0, v1, v2); }); break;
        case 6: submit([this, e, v0, v1] { drawSprite(e, v0, v1); }); break;
        default: break;
    }
    if (traced) trace_->endDraw(*this);
}

// ---------------------------------------------------------------------------
// Texturas
// ---------------------------------------------------------------------------

std::uint32_t Gs::fetchTexel(const DrawEnv& e, unsigned level, int u, int v) const {
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
    const std::uint32_t bp = e.tbp[level], bw = e.tbw[level];
    const std::uint32_t raw = vram_.readPixel(e.tpsm, bp, bw, x, y);
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
                return static_cast<std::uint32_t>(clut_[i]) | (static_cast<std::uint32_t>(clut_[i + 256]) << 16);
            }
            return expand16(clut_[idx & 511], e.ta0, e.ta1, e.aem);
        }
    }
}

std::uint32_t Gs::sampleTexture(const DrawEnv& e, float u, float v, float lod) const {
    // u, v em texels do nível 0.
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
    if (x < e.scax0 || x > e.scax1 || y < e.scay0 || y > e.scay1) return;
    if ((e.scanmsk == 2 && (y & 1)) || (e.scanmsk == 3 && !(y & 1))) return;
    // Mesmo ponto em que gs_coverage conta: confere a conta analítica.
    ++shaded_;
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
    bool writeFb = true, writeZ = !e.zmsk, writeAlpha = true;
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
    const std::uint32_t dst = vram_.readPixel(e.fpsm, e.fbp, e.fbw, ux, uy);

    // Teste de alfa de destino
    if (e.date) {
        const bool bit = fbits == 16 ? (dst & 0x8000) != 0 : (dst & 0x80000000u) != 0;
        if (e.fpsm != PSMCT24 && e.fpsm != PSMZ24 && bit != e.datm) return;
    }

    // Teste de Z (ZTE=0 é proibido no hardware; tratado como ALWAYS)
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

    if (writeFb) {
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
    if (writeZ && (e.zte || !e.zmsk)) vram_.writePixel(e.zpsm, e.zbp, e.fbw, ux, uy, z);
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

    for (int py = minY; py <= maxY; ++py, row0 += dy0, row1 += dy1, row2 += dy2) {
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
