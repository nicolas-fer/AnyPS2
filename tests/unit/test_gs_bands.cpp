// Testes das faixas do GS (gs_bands.cpp e gs_worker.cpp com várias threads).
// Com faixas, a imagem tem de ser idêntica à do caminho síncrono
// (ANYPS2_GS_THREAD=0), byte a byte na VRAM inteira, para qualquer número de
// faixas; e a contagem analítica tem de continuar batendo com o rasterizador.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/gs/gs_bands.h"
#include "anyps2/runtime/gs/gs_coverage.h"
#include "minitest.h"

#ifdef _WIN32
#include <stdlib.h>
#endif

using namespace anyps2::rt;
using namespace anyps2::rt::gs;

namespace {

// Gerador determinístico (sem depender da biblioteca padrão para repetir).
struct Lcg {
    std::uint64_t s;
    std::uint32_t next(std::uint32_t n) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>((s >> 33) % n);
    }
    std::uint64_t word() {
        return (std::uint64_t{next(1u << 16)} << 48) | (std::uint64_t{next(1u << 16)} << 32) |
               (std::uint64_t{next(1u << 16)} << 16) | next(1u << 16);
    }
};

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

std::uint64_t frameReg(std::uint64_t fbp, std::uint64_t fbw, std::uint64_t psm, std::uint64_t mask = 0) {
    return fbp | (fbw << 16) | (psm << 24) | (mask << 32);
}
std::uint64_t scissorReg(std::uint64_t x0, std::uint64_t x1, std::uint64_t y0, std::uint64_t y1) {
    return x0 | (x1 << 16) | (y0 << 32) | (y1 << 48);
}
std::uint64_t xyz(std::uint64_t x16, std::uint64_t y16, std::uint64_t z = 0) {
    return x16 | (y16 << 16) | (z << 32);
}
std::uint64_t xyzPx(std::uint64_t x, std::uint64_t y, std::uint64_t z = 0) {
    return xyz(x * 16, y * 16, z);
}
std::uint64_t rgbaq(unsigned r, unsigned g, unsigned b, unsigned a) {
    return r | (g << 8) | (b << 16) | (std::uint64_t{a} << 24) | (std::uint64_t{0x3F800000u} << 32);
}
std::uint64_t prim(unsigned type, bool iip = false, bool tme = false, bool abe = false, bool fst = false) {
    return type | (iip ? 8u : 0u) | (tme ? 16u : 0u) | (abe ? 64u : 0u) | (fst ? 256u : 0u);
}
// TEX0: TBP (blocos), TBW, PSM, TW/TH (log2), CBP, CPSM, CLD e CSA.
std::uint64_t tex0(std::uint64_t tbp, std::uint64_t tbw, std::uint64_t psm, std::uint64_t tw, std::uint64_t th,
                   std::uint64_t cbp = 0, std::uint64_t cpsm = 0, std::uint64_t cld = 0, std::uint64_t csa = 0) {
    return tbp | (tbw << 14) | (psm << 20) | (tw << 26) | (th << 30) | (cbp << 37) | (cpsm << 51) | (csa << 56) |
           (cld << 61);
}
// UV (10.4) de uma coordenada de textura em texels.
std::uint64_t uv(unsigned u, unsigned v) {
    return (std::uint64_t{u * 16u}) | (std::uint64_t{v * 16u} << 16);
}

void hostToLocal(Gs& g, std::uint32_t dbp, std::uint32_t dbw, std::uint32_t psm, std::uint32_t x, std::uint32_t y,
                 std::uint32_t w, std::uint32_t h, const std::vector<std::uint64_t>& data) {
    g.writeRegister(BITBLTBUF, (std::uint64_t{dbp} << 32) | (std::uint64_t{dbw} << 48) | (std::uint64_t{psm} << 56), 0);
    g.writeRegister(TRXPOS, (std::uint64_t{x} << 32) | (std::uint64_t{y} << 48), 0);
    g.writeRegister(TRXREG, w | (std::uint64_t{h} << 32), 0);
    g.writeRegister(TRXDIR, 0, 0);
    for (std::uint64_t d : data) g.writeRegister(HWREG, d, 0);
}

// Vértice com UV antes do kick (desenhos com FST=1).
void vertex(Gs& g, unsigned x, unsigned y, std::uint64_t color, std::uint64_t z = 0, std::uint64_t uvv = 0,
            bool xyzf = false) {
    g.writeRegister(RGBAQ, color, 0);
    g.writeRegister(UV, uvv, 0);
    g.writeRegister(XYZ2, xyzPx(x, y, z), 0);
    (void)xyzf;
}

// Resultado de uma renderização: a VRAM inteira e as contagens.
struct Render {
    std::vector<std::uint8_t> vram;
    std::uint64_t covered = 0;
    std::uint64_t shaded = 0;
};

Render capture(Gs& g) {
    // As renderizações mudam o ambiente do processo; as suítes seguintes têm de
    // ver o padrão.
    setEnv("ANYPS2_GS_THREAD", "");
    setEnv("ANYPS2_GS_THREADS", "");
    Render r;
    const std::uint8_t* d = g.vram().data();
    r.vram.assign(d, d + Vram::kSize);
    r.covered = g.pixelsCovered();
    r.shaded = g.pixelsShaded();
    return r;
}

// Cena com faixas, texturas com CLUT, transferências HOST→LOCAL e LOCAL→LOCAL no
// meio, renderização para textura, uma textura que lê o próprio FRAME deslocado e
// uma textura escrita por um desenho anterior em outra faixa.
Render renderScene(bool threaded, unsigned lanes) {
    setEnv("ANYPS2_GS_THREAD", threaded ? "1" : "0");
    setEnv("ANYPS2_GS_THREADS", std::to_string(lanes).c_str());
    Gs g(nullptr);
    CHECK_EQ(g.threaded(), threaded);
    CHECK_EQ(g.lanes(), threaded ? lanes : 1u);

    // Framebuffer de 128 px (fbw=2) em FBP 0, Z em ZBP 16 com ZTE e GEQUAL.
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
    g.writeRegister(ZBUF_1, 16, 0);
    g.writeRegister(SCISSOR_1, scissorReg(0, 127, 0, 127), 0);
    g.writeRegister(TEST_1, (1ull << 16) | (2ull << 17), 0);
    g.writeRegister(XYOFFSET_1, 0, 0);
    g.writeRegister(PRMODECONT, 1, 0);
    g.writeRegister(COLCLAMP, 1, 0);
    g.writeRegister(ALPHA_1, 0 | (2ull << 2) | (1ull << 6) | (0x80ull << 32), 0);

    // Triângulo Gouraud grande cruzando as faixas, e um flat com Z menor (falha em parte).
    g.writeRegister(PRIM, prim(3, true), 0);
    vertex(g, 2, 2, rgbaq(200, 0, 0, 0), 0x1000000);
    vertex(g, 120, 8, rgbaq(0, 200, 0, 0), 0x1000000);
    vertex(g, 40, 120, rgbaq(0, 0, 200, 0), 0x1000000);
    g.writeRegister(PRIM, prim(3, false), 0);
    vertex(g, 30, 20, rgbaq(9, 200, 9, 0), 0x10);
    vertex(g, 100, 30, rgbaq(9, 9, 200, 0), 0x10);
    vertex(g, 60, 110, rgbaq(255, 255, 9, 0), 0x2000000);

    // Sprite com alfa e SCANMSK, cruzando faixas; depois sem máscara.
    g.writeRegister(SCISSOR_1, scissorReg(4, 60, 1, 58), 0);
    g.writeRegister(SCANMSK, 2, 0);
    g.writeRegister(PRIM, prim(6, false, false, true), 0);
    vertex(g, 10, 10, rgbaq(255, 128, 0, 0x60));
    vertex(g, 50, 30, rgbaq(255, 128, 0, 0x60));
    g.writeRegister(SCANMSK, 0, 0);
    g.writeRegister(SCISSOR_1, scissorReg(0, 127, 0, 127), 0);

    // Linhas e pontos.
    g.writeRegister(PRIM, prim(1, true), 0);
    vertex(g, 0, 127, rgbaq(255, 255, 255, 0));
    vertex(g, 127, 0, rgbaq(9, 9, 9, 0));
    g.writeRegister(PRIM, prim(0), 0);
    vertex(g, 33, 33, rgbaq(1, 2, 3, 0));
    vertex(g, 90, 100, rgbaq(4, 5, 6, 0));

    // Renderização para textura e uso logo depois: FRAME em FBP 32 (páginas 32..35),
    // 64 px de largura, com triângulos que cruzam as faixas; a textura de 64x64 é
    // lida do que outra faixa acabou de escrever. Repetido com cores novas, para
    // que uma corrida apareça na comparação com o caminho síncrono.
    for (unsigned k = 0; k < 16; ++k) {
        const auto c = static_cast<unsigned>(k * 15);
        g.writeRegister(FRAME_1, frameReg(32, 1, PSMCT32), 0);
        g.writeRegister(PRIM, prim(4, true), 0);
        vertex(g, 0, 0, rgbaq(255, c, 0, 0));
        vertex(g, 63, 0, rgbaq(0, 255, c, 0));
        vertex(g, 0, 63, rgbaq(c, 0, 255, 0));
        vertex(g, 63, 63, rgbaq(255, 255, 255 - c, 0));
        g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);

        g.writeRegister(TEX0_1, tex0(1024, 1, PSMCT32, 6, 6), 0);
        g.writeRegister(PRIM, prim(3, false, true, false, true), 0);
        vertex(g, 60, 70 + k, rgbaq(255, 255, 255, 0x80), 0, uv(0, 0));
        vertex(g, 120, 70 + k, rgbaq(255, 255, 255, 0x80), 0, uv(63, 0));
        vertex(g, 90, 120, rgbaq(255, 255, 255, 0x80), 0, uv(31, 63));
    }

    // HOST→LOCAL no meio do quadro: uma palavra por dois pixels de 32 bits.
    std::vector<std::uint64_t> host;
    for (unsigned i = 0; i < 32; ++i) host.push_back(0x1111111122222222ull * (i + 1));
    hostToLocal(g, 0, 2, PSMCT32, 100, 10, 8, 8, host);

    // CLUT (16x16 PSMCT32 em CBP 1536, página 48) e textura PSMT8 128x64 em TBP 1280
    // (página 40, TBW 2), com CLD=1: a carga da CLUT é uma barreira.
    std::vector<std::uint64_t> clut;
    Lcg r{0xC1A7};
    for (unsigned i = 0; i < 128; ++i) clut.push_back(r.word());
    hostToLocal(g, 1536, 1, PSMCT32, 0, 0, 16, 16, clut);
    std::vector<std::uint64_t> idx;
    for (unsigned i = 0; i < 1024; ++i) idx.push_back(r.word());
    hostToLocal(g, 1280, 2, 0x13, 0, 0, 128, 64, idx);
    g.writeRegister(TEX0_1, tex0(1280, 2, 0x13, 7, 6, 1536, PSMCT32, 1), 0);
    g.writeRegister(PRIM, prim(3, true, true, false, true), 0);
    vertex(g, 5, 90, rgbaq(255, 255, 255, 0x80), 0, uv(0, 0));
    vertex(g, 60, 80, rgbaq(255, 255, 255, 0x80), 0, uv(100, 20));
    vertex(g, 20, 125, rgbaq(255, 255, 255, 0x80), 0, uv(40, 60));

    // Textura que é o próprio FRAME deslocado de uma página (FBP 0, TBP 32):
    // o desenho lê pixels escritos pelos anteriores e os que ele mesmo escreve.
    g.writeRegister(TEX0_1, tex0(32, 2, PSMCT32, 7, 5), 0);
    g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
    vertex(g, 10, 64, rgbaq(255, 255, 255, 0x80), 0, uv(0, 0));
    vertex(g, 100, 100, rgbaq(255, 255, 255, 0x80), 0, uv(90, 36));

    // LOCAL→LOCAL: copia um bloco de 16x16 do FRAME para outra posição do próprio FRAME.
    g.writeRegister(BITBLTBUF, 0ull | (2ull << 16) | (std::uint64_t{PSMCT32} << 24) | (8ull << 32) |
                                   (2ull << 48) | (std::uint64_t{PSMCT32} << 56),
                    0);
    g.writeRegister(TRXPOS, (16ull) | (16ull << 16) | (80ull << 32) | (8ull << 48), 0);
    g.writeRegister(TRXREG, 16 | (16ull << 32), 0);
    g.writeRegister(TRXDIR, 2, 0);

    // Mais desenhos depois das transferências: flat e sprites nas faixas.
    g.writeRegister(PRIM, prim(5, false), 0);
    vertex(g, 0, 0, rgbaq(7, 7, 7, 0), 0x3000000);
    vertex(g, 127, 0, rgbaq(70, 7, 7, 0), 0x3000000);
    vertex(g, 0, 127, rgbaq(7, 70, 7, 0), 0x3000000);
    vertex(g, 127, 127, rgbaq(7, 7, 70, 0), 0x3000000);
    g.writeRegister(PRIM, prim(6, true, false, true), 0);
    vertex(g, 40, 40, rgbaq(1, 2, 3, 0x40));
    vertex(g, 120, 120, rgbaq(250, 240, 230, 0x40));

    return capture(g);
}

// Desenhos aleatórios de todos os tipos, com FRAME/ZBUF/texturas em páginas que se
// sobrepõem de forma arbitrária, para exercitar o produtor (barreiras e globais).
Render renderRandom(bool threaded, unsigned lanes, std::uint64_t seed, int draws) {
    setEnv("ANYPS2_GS_THREAD", threaded ? "1" : "0");
    setEnv("ANYPS2_GS_THREADS", std::to_string(lanes).c_str());
    Gs g(nullptr);
    Lcg r{seed};
    static constexpr unsigned kVerts[7] = {1, 2, 2, 3, 3, 3, 2};
    static constexpr std::uint64_t kFbp[4] = {0, 32, 64, 96};
    static constexpr std::uint64_t kTbp[5] = {0, 32, 64, 96, 128};
    // Cada sorteio num passo à parte: a ordem dos sorteios faz parte da semente.
    for (int n = 0; n < draws; ++n) {
        const std::uint64_t fbp = kFbp[r.next(4)];
        const std::uint64_t fbw = 1 + r.next(3);
        const std::uint64_t psm = r.next(2) ? PSMCT32 : PSMCT16;
        g.writeRegister(FRAME_1, frameReg(fbp, fbw, psm), 0);
        const std::uint64_t zbp = 16 + 32 * r.next(3);
        const std::uint64_t zmsk = r.next(2);
        g.writeRegister(ZBUF_1, zbp | (zmsk << 32), 0);
        const unsigned x0 = r.next(128), y0 = r.next(128);
        const unsigned x1 = x0 + r.next(128 - x0), y1 = y0 + r.next(128 - y0);
        g.writeRegister(SCISSOR_1, scissorReg(x0, x1, y0, y1), 0);
        g.writeRegister(SCANMSK, r.next(4), 0);
        const std::uint64_t zte = r.next(2);
        const std::uint64_t ztst = 1 + r.next(3);
        g.writeRegister(TEST_1, (zte << 16) | (ztst << 17), 0);
        const unsigned type = r.next(7);
        const bool tme = r.next(3) == 0;
        const bool abe = r.next(2) == 1;
        const bool iip = r.next(2) == 1;
        g.writeRegister(PRIM, prim(type, iip, tme, abe, true), 0);
        if (tme) {
            const std::uint64_t tbp = kTbp[r.next(5)];
            const std::uint64_t tbw = 1 + r.next(3);
            const std::uint64_t tpsm = r.next(2) ? PSMCT32 : PSMCT16;
            const std::uint64_t tw = 4 + r.next(3);
            const std::uint64_t th = 4 + r.next(3);
            g.writeRegister(TEX0_1, tex0(tbp, tbw, tpsm, tw, th), 0);
            const std::uint64_t wms = r.next(4);
            const std::uint64_t wmt = r.next(4);
            g.writeRegister(CLAMP_1, wms | (wmt << 2), 0);
        }
        for (unsigned k = 0; k < kVerts[type]; ++k) {
            const unsigned vx = r.next(128), vy = r.next(128);
            const unsigned cr = r.next(256), cg = r.next(256), cb = r.next(256), ca = r.next(256);
            const std::uint64_t z = r.next(1u << 24);
            const unsigned u = r.next(96), v = r.next(96);
            vertex(g, vx, vy, rgbaq(cr, cg, cb, ca), z, uv(u, v));
        }
        if (r.next(8) == 0) g.writeRegister(FRAME_1, frameReg(fbp, fbw, PSMCT32), 0);  // muda o formato sem desenhar
    }
    return capture(g);
}

}  // namespace

// ---------------------------------------------------------------------------
// Faixas: regras de propriedade e de colisão
// ---------------------------------------------------------------------------

TEST_CASE(gs_bands, lane_mask_follows_16_row_blocks) {
    CHECK_EQ(laneMask(0, 15, 2), 1u);            // só o bloco 0 → faixa 0
    CHECK_EQ(laneMask(16, 31, 2), 2u);           // bloco 1 → faixa 1
    CHECK_EQ(laneMask(8, 40, 2), 3u);            // blocos 0..2 → faixas 0 e 1
    CHECK_EQ(laneMask(0, 63, 4), 0xFu);          // todas
    CHECK_EQ(laneMask(16, 31, 4), 2u);           // bloco 1 → faixa 1 de 4
    CHECK_EQ(laneMask(5, 4, 4), 0u);             // intervalo vazio
    CHECK_EQ(laneMask(0, 2047, 16), 0xFFFFu);    // toda a tela, 16 faixas
    CHECK(rowInLane(1, 4, 16));
    CHECK(!rowInLane(1, 4, 15));
    CHECK(rowInLane(0, 1, 1234));  // uma faixa só pega tudo
}

namespace {

PageSet pagesRange(std::uint32_t lo, std::uint32_t hi) {
    PageSet p;
    p.setRange(lo, hi);
    return p;
}

std::uint32_t countPages(const PageSet& s) {
    std::uint32_t n = 0;
    for (std::uint32_t i = 0; i < PageSet::kPages; ++i) n += s.test(i) ? 1u : 0u;
    return n;
}

}  // namespace

TEST_CASE(gs_bands, page_set_ranges_and_intersection) {
    PageSet s;
    CHECK(s.empty());
    s.setRange(60, 70);  // cruza a fronteira de palavra
    s.setRange(511, 511);
    CHECK_EQ(countPages(s), 12u);
    CHECK(s.test(60) && s.test(63) && s.test(64) && s.test(70) && s.test(511));
    CHECK(!s.test(59) && !s.test(71) && !s.test(510));
    CHECK(s.intersects(pagesRange(70, 80)));
    CHECK(!s.intersects(pagesRange(71, 510)));
    CHECK(pagesRange(0, 511).full());
    CHECK(!s.full());
    PageSet t = pagesRange(128, 191);  // uma palavra inteira
    CHECK_EQ(countPages(t), 64u);
    t.merge(s);
    CHECK_EQ(countPages(t), 76u);
}

TEST_CASE(gs_bands, clashes_rules) {
    VramAccess w{};
    w.write = true;
    w.surface = 0;
    w.base = 0;
    w.bw = 2;
    w.psm = PSMCT32;
    w.pages = pagesRange(0, 9);
    VramAccess same = w;
    same.pages = pagesRange(5, 12);
    VramAccess other = w;
    other.surface = 1;  // ZBUF sobre as mesmas páginas: outro mapeamento
    VramAccess read{};
    read.pages = pagesRange(3, 4);
    read.surface = 2;
    VramAccess far = read;
    far.pages = pagesRange(40, 50);

    CHECK(!clashes(w, same));       // mesmo mapeamento: a faixa de cada pixel é a mesma
    CHECK(clashes(w, other));       // mapeamentos diferentes sobrepostos
    CHECK(clashes(w, read));        // escrita e leitura sobrepostas
    CHECK(clashes(read, w));
    CHECK(!clashes(read, far));     // sem sobreposição
    VramAccess read2 = read;
    CHECK(!clashes(read, read2));   // duas leituras nunca colidem
}

namespace {

// Página (índice absoluto, 0..511) do pixel (x, y) pelo endereçamento real da Vram.
std::uint32_t pageOf(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, unsigned x, unsigned y) {
    std::uint32_t addr = 0;
    switch (psm) {
        case PSMT8: addr = Vram::byteAddress8(bp, bw, x, y); break;
        case PSMT4: addr = Vram::nibbleAddress4(bp, bw, x, y) / 2; break;
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: addr = Vram::byteAddress16(bp, bw, x, y, psm); break;
        case PSMZ32: case PSMZ24: addr = Vram::byteAddress32(bp, bw, x, y, true); break;
        default: addr = Vram::byteAddress32(bp, bw, x, y, false); break;  // 32/24 bits e os T*H
    }
    return addr / 8192;
}

}  // namespace

TEST_CASE(gs_bands, page_set_contains_every_touched_page) {
    // Varredura exaustiva: a página de cada pixel do retângulo, no endereçamento
    // real, tem de estar no conjunto. Todos os formatos, larguras (inclusive 0 e
    // ímpares), bases fora de página e retângulos que não começam na coluna 0.
    const std::uint32_t psms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8,  PSMT4,   PSMT8H,
                                  PSMT4HL, PSMT4HH, PSMZ32,  PSMZ24,   PSMZ16, PSMZ16S};
    for (std::uint32_t psm : psms) {
        for (std::uint32_t bw : {0u, 1u, 2u, 3u, 5u}) {
            for (std::uint32_t bp : {0u, 7u, 32u, 1000u, 16384u - 40u, 16384u - 3u}) {
                const unsigned rowLo = 3, rowHi = 140, colLo = 37, colHi = 230;
                const PageSet s = pageSet(psm, bp, bw, rowLo, rowHi, colLo, colHi);
                for (unsigned y = rowLo; y <= rowHi; ++y) {
                    for (unsigned x = colLo; x <= colHi; ++x) CHECK(s.test(pageOf(psm, bp, bw, x, y)));
                }
            }
        }
    }
}

TEST_CASE(gs_bands, page_set_is_exactly_the_touched_pages) {
    // Com a base alinhada a uma página, o conjunto é exatamente o das páginas dos
    // pixels (nada a mais): retângulos estreitos, largos e fora da coluna 0.
    const std::uint32_t psms[] = {PSMCT32, PSMCT16, PSMT8, PSMT4, PSMZ32, PSMZ16};
    struct Rect { unsigned r0, r1, c0, c1; };
    const Rect rects[] = {{0, 255, 0, 31}, {10, 200, 70, 90}, {0, 63, 0, 639}, {33, 34, 5, 600}, {0, 0, 0, 0},
                          {17, 300, 130, 300}};
    for (std::uint32_t psm : psms) {
        for (std::uint32_t bw : {2u, 4u, 10u}) {
            for (std::uint32_t bp : {0u, 40u * 32u}) {
                for (const Rect& rc : rects) {
                    const PageSet s = pageSet(psm, bp, bw, rc.r0, rc.r1, rc.c0, rc.c1);
                    PageSet exact;
                    for (unsigned y = rc.r0; y <= rc.r1; ++y) {
                        for (unsigned x = rc.c0; x <= rc.c1; ++x) {
                            const std::uint32_t pg = pageOf(psm, bp, bw, x, y);
                            exact.setRange(pg, pg);
                        }
                    }
                    CHECK(countPages(s) == countPages(exact));
                    for (std::uint32_t i = 0; i < PageSet::kPages; ++i) CHECK_EQ(s.test(i), exact.test(i));
                }
            }
        }
    }
}

TEST_CASE(gs_bands, page_set_is_exact_per_format) {
    // Uma textura PSMT4 de 128x64 perto do fim da VRAM (como as do GT4): só as
    // páginas dela, não a VRAM inteira.
    const PageSet t4 = pageSet(PSMT4, 0x3E60, 2, 0, 63, 0, 127);
    CHECK_EQ(countPages(t4), 1u);
    CHECK(t4.test(0x3E60u / 32));
    // PSMT8 de 128x64 alinhado: uma página só.
    const PageSet t8 = pageSet(PSMT8, 0x3F00, 2, 0, 63, 0, 127);
    CHECK_EQ(countPages(t8), 1u);
    CHECK(t8.test(0x3F00u / 32));
    // O FRAME de 640x224 em FBP 0x160 (FBW 10, PSMCT32): 10 páginas por linha de
    // páginas, 7 linhas de páginas.
    const PageSet fr = pageSet(PSMCT32, 0x160 * 32, 10, 0, 223, 0, 639);
    CHECK_EQ(countPages(fr), 70u);
    CHECK(fr.test(0x160u) && fr.test(0x160u + 69) && !fr.test(0x160u + 70) && !fr.test(0x160u - 1));
    // 16 bits: páginas de 64x64.
    const PageSet c16 = pageSet(PSMCT16, 0, 2, 0, 63, 0, 127);
    CHECK_EQ(countPages(c16), 2u);
    CHECK(c16.test(0) && c16.test(1));
    const PageSet c16b = pageSet(PSMCT16, 0, 2, 64, 64, 0, 0);
    CHECK_EQ(countPages(c16b), 1u);
    CHECK(c16b.test(2));
    // Coluna inicial: um retângulo que começa na terceira página da linha.
    const PageSet col = pageSet(PSMCT32, 0, 4, 0, 31, 130, 140);
    CHECK_EQ(countPages(col), 1u);
    CHECK(col.test(2));
}

TEST_CASE(gs_bands, page_set_of_a_narrow_strip_skips_the_other_columns) {
    // Sprite de 32x256 num buffer de 640 px (FBW 10): uma página por linha de
    // páginas (0, 10, 20, ... 70), não as 71 páginas do primeiro ao último.
    const PageSet strip = pageSet(PSMCT32, 0x1A0 * 32, 10, 0, 255, 0, 31);
    CHECK_EQ(countPages(strip), 8u);
    for (std::uint32_t k = 0; k < 8; ++k) CHECK(strip.test(0x1A0u + 10 * k));
    CHECK(!strip.test(0x1A0u + 5));
    CHECK(!strip.test(0x1A0u + 69));
}

TEST_CASE(gs_bands, page_set_wraps_the_end_of_vram) {
    // Base na página 510, três páginas em linha: 510, 511 e a volta para a 0.
    const PageSet s = pageSet(PSMCT32, 510 * 32, 3, 0, 31, 0, 191);
    CHECK_EQ(countPages(s), 3u);
    CHECK(s.test(510) && s.test(511) && s.test(0));
    CHECK(!s.test(1));
    for (unsigned x = 0; x < 192; ++x) CHECK(s.test(pageOf(PSMCT32, 510 * 32, 3, x, 17)));
    // Um buffer que cobre 512 páginas ou mais vira a VRAM toda.
    const PageSet all = pageSet(PSMCT32, 0, 8, 0, 2047, 0, 511);
    CHECK(all.full());
}

TEST_CASE(gs_bands, clashes_respects_wrapped_pages_and_near_end_textures) {
    // O acesso que dá a volta colide com o que está no começo da VRAM, e não com o
    // do meio.
    VramAccess w{};
    w.write = true;
    w.pages = pageSet(PSMCT32, 510 * 32, 3, 0, 31, 0, 191);
    VramAccess low{};
    low.pages = pagesRange(0, 0);
    low.surface = 2;
    VramAccess mid = low;
    mid.pages = pagesRange(100, 120);
    CHECK(clashes(w, low));
    CHECK(!clashes(w, mid));
    // Caso do GT4: textura PSMT4 em TBP 0x3E60 e FRAME em FBP 0x160 não colidem.
    VramAccess tex{};
    tex.pages = pageSet(PSMT4, 0x3E60, 2, 0, 127, 0, 127);
    tex.surface = 2;
    VramAccess frame{};
    frame.pages = pageSet(PSMCT32, 0x160 * 32, 10, 0, 223, 0, 639);
    frame.write = true;
    CHECK(!clashes(frame, tex));
    // A fila de pendentes também respeita a volta.
    PendingAccess p;
    p.add(w);
    CHECK(p.clashesWith(low));
    CHECK(!p.clashesWith(mid));
}

TEST_CASE(gs_bands, narrow_strip_does_not_clash_with_a_clut_in_another_column) {
    // O render-to-texture coluna por coluna do GT4: faixa de 32x256 na coluna 0 e
    // uma CLUT 16x16 (CT32) na página 5 da mesma faixa de linhas de páginas.
    VramAccess strip{};
    strip.write = true;
    strip.surface = 1;
    strip.base = 0x1A0;
    strip.bw = 10;
    strip.psm = PSMZ24;
    strip.pages = pageSet(PSMZ24, 0x1A0 * 32, 10, 0, 255, 0, 31);
    VramAccess clut{};
    clut.pages = pageSet(PSMCT32, (0x1A0 + 5) * 32, 1, 0, 15, 0, 15);
    clut.surface = 3;
    CHECK(!clashes(strip, clut));
    CHECK(!accessesOverlap(strip, clut));
    PendingAccess pend;
    pend.add(strip);
    CHECK(!pend.conflictsWith(clut));
    // Uma CLUT na coluna do sprite colide.
    VramAccess inside = clut;
    inside.pages = pageSet(PSMCT32, (0x1A0 + 20) * 32, 1, 0, 15, 0, 15);
    CHECK(pend.conflictsWith(inside));
    // Leituras acumuladas viram uma entrada só: a fila não enche com texturas.
    PendingAccess reads(4);
    for (std::uint32_t i = 0; i < 20; ++i) {
        VramAccess r{};
        r.pages = pagesRange(i, i);
        reads.add(r);
    }
    CHECK_EQ(reads.size(), 1u);
    VramAccess probe = strip;
    probe.pages = pagesRange(19, 19);
    CHECK(reads.clashesWith(probe));
    probe.pages = pagesRange(30, 30);
    CHECK(!reads.clashesWith(probe));
}

TEST_CASE(gs_bands, draw_rows_match_the_shading_box) {
    // Sprite [10, 30) x [20, 40) com janela y 25..35: linhas 25..35.
    const Vertex a = Vertex{};
    Vertex p0 = a, p1 = a;
    p0.x = 10 * 16;
    p0.y = 20 * 16;
    p1.x = 30 * 16;
    p1.y = 40 * 16;
    const RowRange r = drawRows(DrawWindow{0, 63, 25, 35, 0}, 6, p0, p1, p0);
    CHECK_EQ(r.lo, 25);
    CHECK_EQ(r.hi, 35);
    const RowRange none = drawRows(DrawWindow{0, 63, 50, 63, 0}, 6, p0, p1, p0);
    CHECK(none.empty());
}

// ---------------------------------------------------------------------------
// Imagem idêntica entre o caminho síncrono e as faixas
// ---------------------------------------------------------------------------

TEST_CASE(gs_bands, scene_is_identical_for_any_lane_count) {
    const Render ref = renderScene(false, 1);
    CHECK_EQ(ref.covered, ref.shaded);
    bool any = false;
    for (std::uint8_t b : ref.vram) any = any || b != 0;
    CHECK(any);
    for (unsigned lanes : {2u, 3u, 4u}) {
        const Render got = renderScene(true, lanes);
        CHECK_EQ(got.covered, got.shaded);
        CHECK_EQ(got.shaded, ref.shaded);
        CHECK(got.vram == ref.vram);
    }
}

TEST_CASE(gs_bands, random_draws_are_identical_for_any_lane_count) {
    const Render ref = renderRandom(false, 1, 0xBA5E, 400);
    for (unsigned lanes : {2u, 3u, 4u}) {
        const Render got = renderRandom(true, lanes, 0xBA5E, 400);
        CHECK_EQ(got.shaded, got.covered);
        CHECK_EQ(got.covered, ref.covered);
        CHECK(got.vram == ref.vram);
    }
}

// FRAME mais estreito que o SCISSOR: com FBW = 1 o pixel (64 + x, y) cai no
// mesmo endereço que (x, y + 32), que é de outra faixa. A ordem das escritas só
// é a do programa se o desenho não for dividido entre as faixas.
Render renderAliased(bool threaded, unsigned lanes) {
    setEnv("ANYPS2_GS_THREAD", threaded ? "1" : "0");
    setEnv("ANYPS2_GS_THREADS", std::to_string(lanes).c_str());
    Gs g(nullptr);
    g.writeRegister(FRAME_1, frameReg(0, 1, PSMCT32), 0);
    g.writeRegister(ZBUF_1, 1ull << 32, 0);  // ZMSK
    g.writeRegister(SCISSOR_1, scissorReg(0, 127, 0, 63), 0);
    g.writeRegister(TEST_1, 0, 0);
    g.writeRegister(XYOFFSET_1, 0, 0);
    g.writeRegister(PRMODECONT, 1, 0);
    for (unsigned i = 0; i < 6; ++i) {
        // Gouraud: cada pixel tem uma cor diferente, então a ordem aparece.
        g.writeRegister(PRIM, prim(3, true), 0);
        vertex(g, 0, i, rgbaq(40 * i, 0, 200, 0));
        vertex(g, 127, 2 + i, rgbaq(0, 250 - 40 * i, 0, 0));
        vertex(g, 30 + 10 * i, 63, rgbaq(9 * i, 99, 199, 0));
        g.writeRegister(PRIM, prim(6), 0);
        vertex(g, 70 + i, 1 + i, rgbaq(255, 7 * i, 0, 0));
        vertex(g, 120, 30 - i, rgbaq(255, 7 * i, 0, 0));
        g.writeRegister(PRIM, prim(6), 0);
        vertex(g, 5 + i, 33 + i, rgbaq(0, 255, 7 * i, 0));
        vertex(g, 60, 62 - i, rgbaq(0, 255, 7 * i, 0));
    }
    return capture(g);
}

TEST_CASE(gs_bands, frame_narrower_than_scissor_is_identical) {
    const Render sync = renderAliased(false, 1);
    // Corrida depende do tempo das threads: repete para ela ter chance de aparecer.
    for (unsigned rep = 0; rep < 20; ++rep) {
        for (unsigned lanes = 2; lanes <= 4; ++lanes) {
            const Render r = renderAliased(true, lanes);
            CHECK(r.vram == sync.vram);
        }
    }
}

TEST_CASE(gs_bands, shaded_matches_covered_with_lanes) {
    // Sprites e triângulos em todas as faixas: a conta por faixa soma a conta analítica.
    for (unsigned lanes : {2u, 4u}) {
        const Render r = renderRandom(true, lanes, 0x5EED + lanes, 150);
        CHECK_EQ(r.shaded, r.covered);
    }
}

// ---------------------------------------------------------------------------
// CLUT por versões: várias cargas entre desenhos de faixas diferentes
// ---------------------------------------------------------------------------

namespace {

struct ClutRender {
    Render img;
    BandStats stats;
};

// Texturas PSMT8 e PSMT4 de 64x64 com cargas de CLUT (CSM1 em CT32 e CT16, CSM2,
// CSA, CLD 1/4) entre desenhos que cruzam as faixas; áreas de CLUT reescritas por
// HOST→LOCAL e por desenho, para que algumas cargas conflitem com acessos
// pendentes e outras não.
ClutRender renderClut(bool threaded, unsigned lanes) {
    setEnv("ANYPS2_GS_THREAD", threaded ? "1" : "0");
    setEnv("ANYPS2_GS_THREADS", std::to_string(lanes).c_str());
    Gs g(nullptr);
    ClutRender out;
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
    g.writeRegister(ZBUF_1, 1ull << 32, 0);
    g.writeRegister(SCISSOR_1, scissorReg(0, 127, 0, 127), 0);
    g.writeRegister(TEST_1, 0, 0);
    g.writeRegister(XYOFFSET_1, 0, 0);
    g.writeRegister(PRMODECONT, 1, 0);
    Lcg r{0xC1074};
    auto words = [&r](unsigned n) {
        std::vector<std::uint64_t> v;
        for (unsigned i = 0; i < n; ++i) v.push_back(r.word());
        return v;
    };
    for (unsigned k = 0; k < 6; ++k) hostToLocal(g, (48 + k) * 32, 1, PSMCT32, 0, 0, 16, 16, words(128));
    hostToLocal(g, 60 * 32, 4, PSMCT16, 0, 0, 256, 4, words(256));  // CLUT 16 bits para CSM2
    hostToLocal(g, 1280, 1, PSMT8, 0, 0, 64, 64, words(512));
    hostToLocal(g, 1344, 1, PSMT4, 0, 0, 64, 64, words(256));
    for (unsigned k = 0; k < 18; ++k) {
        const unsigned c = k % 6;
        const bool four = k % 3 == 2;
        const std::uint64_t cld = (k % 4 == 3) ? 4 : 1;
        std::uint64_t t0 = four ? tex0(1344, 1, PSMT4, 6, 6, (48 + c) * 32, PSMCT32, cld, k % 16)
                                : tex0(1280, 1, PSMT8, 6, 6, (48 + c) * 32, PSMCT32, cld, 0);
        if (k % 5 == 4) {  // CLUT de 16 bits, CSM1: lê a área como PSMCT16
            t0 = four ? tex0(1344, 1, PSMT4, 6, 6, (48 + c) * 32, PSMCT16, 1, k % 16)
                      : tex0(1280, 1, PSMT8, 6, 6, (48 + c) * 32, PSMCT16, 1, 0);
        }
        if (k % 7 == 6) {  // CSM2: entradas em linha de 256 pixels de 16 bits
            t0 = tex0(1344, 1, PSMT4, 6, 6, 60 * 32, PSMCT16, 1, k % 16) | (1ull << 55);
            g.writeRegister(TEXCLUT, 4 | (std::uint64_t{k % 3} << 6) | (std::uint64_t{k % 4} << 12), 0);
        }
        g.writeRegister(TEX0_1, t0, 0);
        g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
        const unsigned y0 = (k * 7) % 40;
        vertex(g, 3 + k, y0, rgbaq(255, 255, 255, 0x80), 0, uv(0, 0));
        vertex(g, 100 + k, y0 + 70, rgbaq(255, 255, 255, 0x80), 0, uv(63, 63));
        if (k == 5) {  // um desenho escreve numa área de CLUT: a carga seguinte conflita
            g.writeRegister(FRAME_1, frameReg(51, 1, PSMCT32), 0);
            g.writeRegister(PRIM, prim(3, true), 0);
            vertex(g, 0, 0, rgbaq(255, 0, 0, 0));
            vertex(g, 63, 0, rgbaq(0, 255, 0, 0));
            vertex(g, 0, 31, rgbaq(0, 0, 255, 0));
            g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
        }
        if (k == 9) hostToLocal(g, 49 * 32, 1, PSMCT32, 0, 0, 16, 16, words(128));  // reescreve a CLUT 1
    }
    out.stats = g.bandStats();
    out.img = capture(g);
    return out;
}

}  // namespace

TEST_CASE(gs_bands, clut_versions_are_identical_for_any_lane_count) {
    const ClutRender ref = renderClut(false, 1);
    bool any = false;
    for (std::uint8_t b : ref.img.vram) any = any || b != 0;
    CHECK(any);
    CHECK(ref.stats.clutDirect > 10);  // sem thread tudo é direto
    CHECK_EQ(ref.stats.submit[BandStats::SubmitClut], 0u);
    for (unsigned rep = 0; rep < 5; ++rep) {
        for (unsigned lanes : {2u, 3u, 4u}) {
            const ClutRender got = renderClut(true, lanes);
            CHECK(got.img.vram == ref.img.vram);
            CHECK_EQ(got.img.shaded, got.img.covered);
            // Cada carga é direta ou uma barreira (quantas de cada, depende do tempo).
            CHECK_EQ(got.stats.clutDirect + got.stats.submit[BandStats::SubmitClut], ref.stats.clutDirect);
        }
    }
}

// Render-to-texture em colunas estreitas (como o do GT4) sobre a mesma faixa de
// linhas de páginas onde ficam as CLUTs: as cargas não podem virar barreira, e a
// imagem continua idêntica à do caminho síncrono.
namespace {

ClutRender renderStripsAndCluts(bool threaded, unsigned lanes) {
    setEnv("ANYPS2_GS_THREAD", threaded ? "1" : "0");
    setEnv("ANYPS2_GS_THREADS", std::to_string(lanes).c_str());
    Gs g(nullptr);
    ClutRender out;
    Lcg r{0x57A1};
    const std::uint32_t base = 0x1A0;  // 10 páginas por linha de páginas (FBW 10)
    std::vector<std::uint64_t> clut;
    for (unsigned i = 0; i < 128; ++i) clut.push_back(r.word());
    hostToLocal(g, (base + 5) * 32, 1, PSMCT32, 0, 0, 16, 16, clut);  // CLUT na coluna 5
    std::vector<std::uint64_t> idx;
    for (unsigned i = 0; i < 512; ++i) idx.push_back(r.word());
    hostToLocal(g, 1280, 1, PSMT8, 0, 0, 64, 64, idx);
    g.writeRegister(ZBUF_1, 1ull << 32, 0);
    g.writeRegister(TEST_1, 0, 0);
    g.writeRegister(XYOFFSET_1, 0, 0);
    g.writeRegister(PRMODECONT, 1, 0);
    g.writeRegister(SCISSOR_1, scissorReg(0, 639, 0, 255), 0);
    for (unsigned k = 0; k < 20; ++k) {
        // Coluna k%2 (0 ou 1) do buffer: 32 px de largura por 256 de altura.
        g.writeRegister(FRAME_1, frameReg(base, 10, PSMCT32), 0);
        g.writeRegister(TEX0_1, tex0(1280, 1, PSMT8, 6, 6, (base + 5) * 32, PSMCT32, 1, 0), 0);
        g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
        const unsigned x0 = (k % 2) * 64;
        vertex(g, x0, 0, rgbaq(255, 255, 255, 0x80), 0, uv(0, 0));
        vertex(g, x0 + 32, 255, rgbaq(255, 255, 255, 0x80), 0, uv(63, 63));
    }
    out.stats = g.bandStats();
    out.img = capture(g);
    return out;
}

}  // namespace

TEST_CASE(gs_bands, strips_over_the_clut_area_keep_clut_loads_without_barrier) {
    const ClutRender ref = renderStripsAndCluts(false, 1);
    bool any = false;
    for (std::uint8_t b : ref.img.vram) any = any || b != 0;
    CHECK(any);
    CHECK_EQ(ref.stats.clutDirect, 20u);
    for (unsigned rep = 0; rep < 5; ++rep) {
        for (unsigned lanes : {2u, 3u, 4u}) {
            const ClutRender got = renderStripsAndCluts(true, lanes);
            CHECK(got.img.vram == ref.img.vram);
            CHECK_EQ(got.img.shaded, got.img.covered);
            // As colunas 0 e 1 não tocam a página 5 da CLUT: nenhuma carga é barreira.
            CHECK_EQ(got.stats.clutDirect, 20u);
            CHECK_EQ(got.stats.submit[BandStats::SubmitClut], 0u);
        }
    }
}

namespace {

// Muitas cargas (mais que o limite da cadeia de versões) com CSA variados, CT32 e
// CT16, intercaladas com desenhos que cruzam as faixas; a cada 17 cargas um desenho
// escreve numa área de CLUT que a carga seguinte lê (conflito real, carga por barreira).
ClutRender renderClutMany(bool threaded, unsigned lanes) {
    setEnv("ANYPS2_GS_THREAD", threaded ? "1" : "0");
    setEnv("ANYPS2_GS_THREADS", std::to_string(lanes).c_str());
    Gs g(nullptr);
    ClutRender out;
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
    g.writeRegister(ZBUF_1, 1ull << 32, 0);
    g.writeRegister(SCISSOR_1, scissorReg(0, 127, 0, 127), 0);
    g.writeRegister(TEST_1, 0, 0);
    g.writeRegister(XYOFFSET_1, 0, 0);
    g.writeRegister(PRMODECONT, 1, 0);
    Lcg r{0xC1075};
    auto words = [&r](unsigned n) {
        std::vector<std::uint64_t> v;
        for (unsigned i = 0; i < n; ++i) v.push_back(r.word());
        return v;
    };
    for (unsigned k = 0; k < 6; ++k) hostToLocal(g, (48 + k) * 32, 1, PSMCT32, 0, 0, 16, 16, words(128));
    hostToLocal(g, 1280, 1, PSMT8, 0, 0, 64, 64, words(512));
    hostToLocal(g, 1344, 1, PSMT4, 0, 0, 64, 64, words(256));
    for (unsigned k = 0; k < 100; ++k) {
        const unsigned c = k % 6;
        const bool four = k % 3 != 0;
        const std::uint32_t cpsm = (k % 5 == 4) ? PSMCT16 : PSMCT32;
        const std::uint64_t t0 = four ? tex0(1344, 1, PSMT4, 6, 6, (48 + c) * 32, cpsm, 1, (k * 5) % 32)
                                      : tex0(1280, 1, PSMT8, 6, 6, (48 + c) * 32, cpsm, 1, (k * 3) % 2 * 16);
        g.writeRegister(TEX0_1, t0, 0);
        g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
        const unsigned y0 = (k * 7) % 40;
        vertex(g, 3 + k % 20, y0, rgbaq(255, 255, 255, 0x80), 0, uv(0, 0));
        vertex(g, 90 + k % 20, y0 + 60, rgbaq(255, 255, 255, 0x80), 0, uv(63, 63));
        if (k % 17 == 16) {  // desenho sobre a CLUT 3: a próxima carga a lê com conflito
            g.writeRegister(FRAME_1, frameReg(51, 1, PSMCT32), 0);
            g.writeRegister(PRIM, prim(3, true), 0);
            vertex(g, 0, 0, rgbaq(255, 0, 0, 0));
            vertex(g, 63, 0, rgbaq(0, 255, 0, 0));
            vertex(g, 0, 31, rgbaq(0, 0, 255, 0));
            g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
            g.writeRegister(TEX0_1, tex0(1344, 1, PSMT4, 6, 6, 51 * 32, PSMCT32, 1, k % 16), 0);
            g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
            vertex(g, 5, 5, rgbaq(255, 255, 255, 0x80), 0, uv(0, 0));
            vertex(g, 100, 100, rgbaq(255, 255, 255, 0x80), 0, uv(63, 63));
        }
    }
    out.stats = g.bandStats();
    out.img = capture(g);
    return out;
}

}  // namespace

TEST_CASE(gs_bands, clut_chain_many_loads_with_real_conflicts_match_sync) {
    const ClutRender ref = renderClutMany(false, 1);
    bool any = false;
    for (std::uint8_t b : ref.img.vram) any = any || b != 0;
    CHECK(any);
    CHECK_EQ(ref.stats.submit[BandStats::SubmitClut], 0u);
    for (unsigned rep = 0; rep < 5; ++rep) {
        for (unsigned lanes : {2u, 3u, 4u}) {
            const ClutRender got = renderClutMany(true, lanes);
            CHECK(got.img.vram == ref.img.vram);
            CHECK_EQ(got.stats.clutDirect + got.stats.submit[BandStats::SubmitClut], ref.stats.clutDirect);
        }
    }
}

// ---------------------------------------------------------------------------
// HOST→LOCAL sem barreira: uploads entre desenhos que usam e que não usam a textura
// ---------------------------------------------------------------------------

namespace {

struct UploadRender {
    Render img;
    BandStats stats;
};

// Duas texturas PSMCT32 de 64x64 (páginas 40 e 44) e uma PSMT8 de 64x64 (página 48).
// A cada volta: desenhos que lêem A, upload em B (sem colisão), desenho com B,
// upload em A com desenhos pendentes que a lêem (colisão: lotes por barreira),
// e um upload partido em dois pacotes com um desenho que lê a textura no meio.
UploadRender renderUploads(bool threaded, unsigned lanes) {
    setEnv("ANYPS2_GS_THREAD", threaded ? "1" : "0");
    setEnv("ANYPS2_GS_THREADS", std::to_string(lanes).c_str());
    Gs g(nullptr);
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
    g.writeRegister(ZBUF_1, 1ull << 32, 0);
    g.writeRegister(SCISSOR_1, scissorReg(0, 127, 0, 127), 0);
    g.writeRegister(TEST_1, 0, 0);
    g.writeRegister(XYOFFSET_1, 0, 0);
    g.writeRegister(PRMODECONT, 1, 0);
    Lcg r{0x0B10AD};
    auto words = [&r](unsigned n) {
        std::vector<std::uint64_t> v;
        for (unsigned i = 0; i < n; ++i) v.push_back(r.word());
        return v;
    };
    auto drawFrom = [&g](std::uint64_t tbp, unsigned k) {
        g.writeRegister(TEX0_1, tex0(tbp, 1, PSMCT32, 6, 6), 0);
        g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
        vertex(g, 2 + k, 5 + k, rgbaq(255, 255, 255, 0x80), 0, uv(0, 0));
        vertex(g, 70 + k, 90 + k, rgbaq(255, 255, 255, 0x80), 0, uv(63, 63));
    };
    const std::uint64_t A = 40 * 32, B = 44 * 32;
    hostToLocal(g, A, 1, PSMCT32, 0, 0, 64, 64, words(2048));
    for (unsigned k = 0; k < 12; ++k) {
        drawFrom(A, k);
        hostToLocal(g, B, 1, PSMCT32, 0, 0, 64, 64, words(2048));  // B ninguém usa
        drawFrom(B, k);
        drawFrom(A, k + 1);
        hostToLocal(g, A, 1, PSMCT32, 0, 0, 64, 64, words(2048));  // A tem leitores pendentes
        drawFrom(A, k + 2);
        // Upload partido: metade, um desenho que lê a textura, o resto.
        g.writeRegister(BITBLTBUF, (std::uint64_t{B} << 32) | (1ull << 48) | (std::uint64_t{PSMCT32} << 56), 0);
        g.writeRegister(TRXPOS, 0, 0);
        g.writeRegister(TRXREG, 64 | (std::uint64_t{64} << 32), 0);
        g.writeRegister(TRXDIR, 0, 0);
        for (std::uint64_t d : words(1024)) g.writeRegister(HWREG, d, 0);
        drawFrom(B, k + 3);
        for (std::uint64_t d : words(1024)) g.writeRegister(HWREG, d, 0);
        drawFrom(B, k + 4);
        // PSMCT24 e PSMT8 (palavras que atravessam pixels), perto do fim da VRAM.
        hostToLocal(g, 500 * 32, 1, PSMCT24, 3, 5, 20, 7, words(53));
        hostToLocal(g, 508 * 32, 2, PSMT8, 0, 0, 128, 64, words(1024));
    }
    UploadRender out;
    out.stats = g.bandStats();
    out.img = capture(g);
    return out;
}

}  // namespace

TEST_CASE(gs_bands, host_uploads_are_identical_for_any_lane_count) {
    const UploadRender ref = renderUploads(false, 1);
    bool any = false;
    for (std::uint8_t b : ref.img.vram) any = any || b != 0;
    CHECK(any);
    // Todo upload é direto (o produtor escreve na VRAM), com ou sem espera das faixas.
    CHECK_EQ(ref.stats.hostDirect, 12u * 5 + 1);
    CHECK_EQ(ref.stats.hostBarrier + ref.stats.hostOrder, 0u);  // sem thread não há o que esperar
    for (unsigned rep = 0; rep < 5; ++rep) {
        for (unsigned lanes : {2u, 3u, 4u}) {
            const UploadRender got = renderUploads(true, lanes);
            CHECK(got.img.vram == ref.img.vram);
            CHECK_EQ(got.stats.hostDirect + got.stats.hostBarrier, ref.stats.hostDirect);
            CHECK_EQ(got.img.shaded, got.img.covered);
        }
    }
}

// ---------------------------------------------------------------------------
// Cache de texturas: transferência grande sobre uma área que desenhos pendentes lêem
// ---------------------------------------------------------------------------

namespace {

// Textura PSMT8 na página 40 e CLUTs nas páginas 44 e 45, todas dentro de uma área de
// 64x256 PSMCT32 (páginas 40..47) que é reescrita a cada volta por um upload grande,
// partido ao meio por um desenho que lê a textura (e vê só a primeira metade). Antes e
// depois do upload há desenhos pendentes que lêem a área, e cargas de CLUT dela.
UploadRender renderTextureCache(bool threaded, unsigned lanes) {
    setEnv("ANYPS2_GS_THREAD", threaded ? "1" : "0");
    setEnv("ANYPS2_GS_THREADS", std::to_string(lanes).c_str());
    Gs g(nullptr);
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
    g.writeRegister(ZBUF_1, 1ull << 32, 0);
    g.writeRegister(SCISSOR_1, scissorReg(0, 127, 0, 127), 0);
    g.writeRegister(TEST_1, 0, 0);
    g.writeRegister(XYOFFSET_1, 0, 0);
    g.writeRegister(PRMODECONT, 1, 0);
    Lcg r{0x7E27CAC};
    auto words = [&r](unsigned n) {
        std::vector<std::uint64_t> v;
        for (unsigned i = 0; i < n; ++i) v.push_back(r.word());
        return v;
    };
    auto drawIndexed = [&g](unsigned cbp, unsigned k) {
        g.writeRegister(TEX0_1, tex0(40 * 32, 1, PSMT8, 6, 6, cbp * 32, PSMCT32, 1), 0);  // CLD = 1: carrega
        g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
        vertex(g, 1 + k, 4 + k, rgbaq(255, 255, 255, 0x80), 0, uv(0, 0));
        vertex(g, 90 + k, 100 + k, rgbaq(255, 255, 255, 0x80), 0, uv(63, 63));
    };
    auto split = [&](unsigned k) {
        g.writeRegister(BITBLTBUF, (std::uint64_t{40 * 32} << 32) | (1ull << 48) | (std::uint64_t{PSMCT32} << 56), 0);
        g.writeRegister(TRXPOS, 0, 0);
        g.writeRegister(TRXREG, 64 | (std::uint64_t{256} << 32), 0);
        g.writeRegister(TRXDIR, 0, 0);
        for (std::uint64_t d : words(4096)) g.writeRegister(HWREG, d, 0);
        drawIndexed(44, k + 3);  // lê a textura e a CLUT com só metade do upload feita
        for (std::uint64_t d : words(4096)) g.writeRegister(HWREG, d, 0);
    };
    hostToLocal(g, 40 * 32, 1, PSMCT32, 0, 0, 64, 256, words(8192));
    for (unsigned k = 0; k < 10; ++k) {
        drawIndexed(44, k);      // leitores pendentes da área
        drawIndexed(45, k + 1);
        split(k);                // a transferência colide com eles
        for (unsigned c = 0; c < 4; ++c) drawIndexed(44 + (c & 1), k + c);  // CLUT e textura da área
    }
    UploadRender out;
    out.stats = g.bandStats();
    out.img = capture(g);
    return out;
}

}  // namespace

TEST_CASE(gs_bands, texture_cache_upload_keeps_clut_loads_barrier_free) {
    const UploadRender ref = renderTextureCache(false, 1);
    bool any = false;
    for (std::uint8_t b : ref.img.vram) any = any || b != 0;
    CHECK(any);
    CHECK_EQ(ref.stats.hostDirect, 11u);
    CHECK_EQ(ref.stats.clutDirect, 10u * 7);
    for (unsigned rep = 0; rep < 5; ++rep) {
        for (unsigned lanes : {2u, 3u, 4u}) {
            const UploadRender got = renderTextureCache(true, lanes);
            CHECK(got.img.vram == ref.img.vram);
            CHECK_EQ(got.img.shaded, got.img.covered);
            // Direta, com ou sem espera pelas faixas no início (depende do tempo).
            CHECK_EQ(got.stats.hostDirect + got.stats.hostBarrier, 11u);
            // O upload não fica pendente nas faixas: nenhuma carga de CLUT precisa de barreira.
            CHECK_EQ(got.stats.submit[BandStats::SubmitClut], 0u);
            CHECK_EQ(got.stats.clutDirect, ref.stats.clutDirect);
        }
    }
}

// ---------------------------------------------------------------------------
// Contadores de serialização (Gs::bandStats): cada motivo de espera, de desenho
// solitário e de barreira cai no contador certo.
// ---------------------------------------------------------------------------

namespace {

// GS com 4 faixas e o ambiente padrão restaurado depois (o Gs lê o ambiente só
// na construção).
struct StatsGs {
    std::unique_ptr<Gs> gs;
    StatsGs() {
        setEnv("ANYPS2_GS_THREAD", "1");
        setEnv("ANYPS2_GS_THREADS", "4");
        gs = std::make_unique<Gs>(nullptr);
        setEnv("ANYPS2_GS_THREAD", "");
        setEnv("ANYPS2_GS_THREADS", "");
        gs->writeRegister(SCISSOR_1, scissorReg(0, 127, 0, 127), 0);
        gs->writeRegister(XYOFFSET_1, 0, 0);
        gs->writeRegister(PRMODECONT, 1, 0);
        gs->writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
        gs->writeRegister(ZBUF_1, 1ull << 32, 0);  // ZMSK: sem escrita de Z
        gs->writeRegister(TEST_1, 0, 0);
    }
};

void sprite(Gs& g, unsigned x0, unsigned y0, unsigned x1, unsigned y1, bool tme = false) {
    g.writeRegister(PRIM, prim(6, false, tme, false, tme), 0);
    vertex(g, x0, y0, rgbaq(200, 100, 50, 0x80), 0, uv(0, 0));
    vertex(g, x1, y1, rgbaq(200, 100, 50, 0x80), 0, uv(x1 - x0, y1 - y0));
}

}  // namespace

TEST_CASE(gs_bands, stats_count_draws_and_lanes) {
    StatsGs t;
    Gs& g = *t.gs;
    sprite(g, 0, 0, 100, 63);  // 4 blocos de 16 linhas: as 4 faixas
    sprite(g, 0, 0, 20, 10);   // um bloco só: 1 faixa
    const BandStats& s = g.bandStats();
    CHECK_EQ(s.bandDraws, 2u);
    CHECK_EQ(s.lanesTouched, 5u);
    CHECK_EQ(s.self[BandStats::SelfWrap] + s.self[BandStats::SelfTexture] + s.self[BandStats::SelfFrameZ], 0u);
    CHECK_EQ(s.conflict[BandStats::ConflictFull] + s.conflict[BandStats::ConflictWrite] +
                 s.conflict[BandStats::ConflictTexture],
             0u);
}

TEST_CASE(gs_bands, stats_self_wrap) {
    StatsGs t;
    Gs& g = *t.gs;
    g.writeRegister(FRAME_1, frameReg(0, 0, PSMCT32), 0);  // FBW = 0
    sprite(g, 0, 0, 20, 20);
    g.writeRegister(FRAME_1, frameReg(0, 1, PSMCT32), 0);  // largura 64 < 100
    sprite(g, 0, 0, 100, 20);
    const BandStats& s = g.bandStats();
    CHECK_EQ(s.self[BandStats::SelfWrap], 2u);
    CHECK_EQ(s.submit[BandStats::SubmitDraw], 2u);
    CHECK_EQ(s.bandDraws, 0u);
}

TEST_CASE(gs_bands, stats_self_texture_and_pairs) {
    StatsGs t;
    Gs& g = *t.gs;
    g.writeRegister(FRAME_1, frameReg(3, 1, PSMCT32), 0);
    g.writeRegister(TEX0_1, tex0(3 * 32 + 4, 1, PSMCT32, 5, 5), 0);  // dentro do próprio FRAME
    for (unsigned i = 0; i < 3; ++i) sprite(g, 0, 0, 30, 30, true);
    g.writeRegister(TEX0_1, tex0(3 * 32, 1, PSMCT32, 5, 5), 0);
    sprite(g, 0, 0, 30, 30, true);
    const BandStats& s = g.bandStats();
    CHECK_EQ(s.self[BandStats::SelfTexture], 4u);
    CHECK_EQ(s.submit[BandStats::SubmitDraw], 4u);
    CHECK_EQ(s.selfPairs.count(3, 3 * 32 + 4), 3u);
    CHECK_EQ(s.selfPairs.count(3, 3 * 32), 1u);
    CHECK_EQ(s.selfPairs.used, 2u);
}

TEST_CASE(gs_bands, stats_self_frame_z) {
    StatsGs t;
    Gs& g = *t.gs;
    g.writeRegister(ZBUF_1, 0, 0);                    // ZBP = FBP, com escrita de Z
    g.writeRegister(TEST_1, (1ull << 16) | (1ull << 17), 0);  // ZTE, ZTST = ALWAYS
    sprite(g, 0, 0, 20, 20);
    CHECK_EQ(g.bandStats().self[BandStats::SelfFrameZ], 1u);
    CHECK_EQ(g.bandStats().bandDraws, 0u);
}

TEST_CASE(gs_bands, stats_conflict_write) {
    StatsGs t;
    Gs& g = *t.gs;
    sprite(g, 0, 0, 20, 20);
    g.writeRegister(FRAME_1, frameReg(0, 1, PSMCT32), 0);  // mesmas páginas, outro mapeamento
    sprite(g, 0, 0, 20, 20);
    const BandStats& s = g.bandStats();
    CHECK_EQ(s.conflict[BandStats::ConflictWrite], 1u);
    CHECK_EQ(s.conflict[BandStats::ConflictTexture], 0u);
    CHECK_EQ(s.bandDraws, 2u);
}

TEST_CASE(gs_bands, stats_conflict_texture_and_pairs) {
    StatsGs t;
    Gs& g = *t.gs;
    g.writeRegister(FRAME_1, frameReg(32, 1, PSMCT32), 0);  // renderiza na textura
    sprite(g, 0, 0, 63, 63);
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
    g.writeRegister(TEX0_1, tex0(32 * 32, 1, PSMCT32, 6, 6), 0);  // e a lê em seguida
    sprite(g, 0, 0, 63, 63, true);
    const BandStats& s = g.bandStats();
    CHECK_EQ(s.conflict[BandStats::ConflictTexture], 1u);
    CHECK_EQ(s.conflict[BandStats::ConflictWrite], 0u);
    CHECK_EQ(s.conflictPairs.count(0, 32 * 32), 1u);
    CHECK_EQ(s.self[BandStats::SelfTexture], 0u);
}

TEST_CASE(gs_bands, stats_conflict_full) {
    StatsGs t;
    Gs& g = *t.gs;
    // Cada desenho escreve numa página diferente (de 3 em 3): nada colide, mas a lista de
    // acessos pendentes enche (64) e o desenho seguinte pede a barreira.
    for (unsigned i = 0; i < 70; ++i) {
        g.writeRegister(FRAME_1, frameReg(3 * i, 1, PSMCT32), 0);
        sprite(g, 0, 0, 8, 8);
    }
    const BandStats& s = g.bandStats();
    CHECK_EQ(s.conflict[BandStats::ConflictFull], 1u);
    CHECK_EQ(s.conflict[BandStats::ConflictWrite], 0u);
    CHECK_EQ(s.bandDraws, 70u);
}

TEST_CASE(gs_bands, stats_submits_and_host_direct) {
    StatsGs t;
    Gs& g = *t.gs;
    hostToLocal(g, 0, 1, PSMCT32, 0, 0, 8, 8, std::vector<std::uint64_t>(32, 0x1234));
    g.writeRegister(TRXDIR, 3, 0);  // cancela a transferência
    // LOCAL→LOCAL
    g.writeRegister(BITBLTBUF, (std::uint64_t{PSMCT32} << 24) | (1ull << 16) | (std::uint64_t{64} << 32) |
                                   (1ull << 48) | (std::uint64_t{PSMCT32} << 56),
                    0);
    g.writeRegister(TRXPOS, 0, 0);
    g.writeRegister(TRXREG, 8 | (8ull << 32), 0);
    g.writeRegister(TRXDIR, 2, 0);
    // CLUT (PSMT8 com CLD = 1)
    g.writeRegister(TEX0_1, tex0(1280, 2, 0x13, 6, 6, 1536, PSMCT32, 1), 0);
    // Reset do GS
    g.writePrivileged(0x12001000u, 1ull << 9, 0);
    const BandStats& s = g.bandStats();
    // Nada pendente toca o destino: o produtor escreveu direto, sem esperar as faixas.
    CHECK_EQ(s.hostDirect, 1u);
    CHECK_EQ(s.hostBarrier, 0u);
    CHECK_EQ(s.submit[BandStats::SubmitLocal], 1u);
    // A área da CLUT (página 48) não é tocada por nada pendente: carga no produtor.
    CHECK_EQ(s.submit[BandStats::SubmitClut], 0u);
    CHECK_EQ(s.clutDirect, 1u);
    CHECK_EQ(s.submit[BandStats::SubmitReset], 1u);
    CHECK_EQ(s.submit[BandStats::SubmitDraw], 0u);
}

TEST_CASE(gs_bands, stats_waits_by_reason) {
    StatsGs t;
    Gs& g = *t.gs;
    g.setRealTimeClock(true);
    sprite(g, 0, 0, 20, 20);
    const BandStats& s = g.bandStats();
    for (unsigned i = 0; i < BandStats::WaitCount; ++i) CHECK_EQ(s.waits[i], 0u);

    (void)g.display();
    CHECK_EQ(s.waits[BandStats::WaitDisplay], 1u);
    (void)g.vram();
    const Gs& cg = g;
    (void)cg.vram();
    CHECK_EQ(s.waits[BandStats::WaitVram], 2u);
    g.writeRegister(SIGNAL, 0xFFFFFFFF00000001ull, 0);
    CHECK_EQ(s.waits[BandStats::WaitSignal], 1u);
    g.writeRegister(FINISH, 0, 0);
    CHECK_EQ(s.waits[BandStats::WaitFinish], 1u);
    g.writeRegister(BITBLTBUF, (std::uint64_t{PSMCT32} << 24) | (1ull << 16), 0);
    g.writeRegister(TRXPOS, 0, 0);
    g.writeRegister(TRXREG, 8 | (8ull << 32), 0);
    g.writeRegister(TRXDIR, 1, 0);  // LOCAL→HOST
    CHECK_EQ(s.waits[BandStats::WaitDownload], 1u);
    g.waitIdle();
    CHECK_EQ(s.waits[BandStats::WaitOther], 1u);
}

TEST_CASE(gs_bands, clut_load_away_from_pending_pages_has_no_barrier) {
    StatsGs t;
    Gs& g = *t.gs;
    // Desenhos grandes pendentes nas páginas 0..: a CLUT na página 48 não é tocada.
    sprite(g, 0, 0, 100, 63);
    g.writeRegister(TEX0_1, tex0(1280, 2, 0x13, 6, 6, 1536, PSMCT32, 1), 0);
    sprite(g, 0, 0, 100, 63);
    g.writeRegister(TEX0_1, tex0(1280, 2, 0x13, 6, 6, 1568, PSMCT32, 1), 0);
    sprite(g, 0, 0, 100, 63);
    const BandStats& s = g.bandStats();
    CHECK_EQ(s.clutDirect, 2u);
    CHECK_EQ(s.submit[BandStats::SubmitClut], 0u);
    CHECK_EQ(s.conflict[BandStats::ConflictWrite], 0u);
}

TEST_CASE(gs_bands, host_upload_away_from_pending_pages_has_no_barrier) {
    StatsGs t;
    Gs& g = *t.gs;
    sprite(g, 0, 0, 100, 63);  // pendente no FRAME (páginas 0..1)
    hostToLocal(g, 40 * 32, 1, PSMCT32, 0, 0, 64, 32, std::vector<std::uint64_t>(1024, 0x1234));
    hostToLocal(g, 41 * 32, 1, PSMCT32, 0, 0, 64, 32, std::vector<std::uint64_t>(1024, 0x1234));
    const BandStats& s = g.bandStats();
    CHECK_EQ(s.hostDirect, 2u);
    CHECK_EQ(s.hostBarrier, 0u);
    CHECK_EQ(g.vram().read32(Vram::byteAddress32(40 * 32, 1, 0, 0)), 0x1234u);
}

TEST_CASE(gs_bands, upload_over_pending_readers_waits_once_then_writes_direct) {
    StatsGs t;
    Gs& g = *t.gs;
    // Leitores pendentes da área (textura da página 40), todos grandes para seguirem nas faixas.
    g.writeRegister(TEX0_1, tex0(40 * 32, 1, PSMCT32, 6, 6), 0);
    for (unsigned i = 0; i < 20; ++i) sprite(g, 0, 0, 100, 63, true);
    hostToLocal(g, 40 * 32, 1, PSMCT32, 0, 0, 64, 32, std::vector<std::uint64_t>(1024, 0x1234));
    const BandStats& s = g.bandStats();
    // Uma espera no início (ou nenhuma, se as faixas já tinham terminado), nunca lotes.
    CHECK_EQ(s.hostDirect + s.hostBarrier, 1u);
    CHECK(s.hostOrder == 0u);
    CHECK_EQ(g.vram().read32(Vram::byteAddress32(40 * 32, 1, 0, 0)), 0x1234u);
}

// Uma carga que conflita de verdade vira barreira; as seguintes, sem conflito, não:
// antes cada uma esperava a versão anterior ficar pronta e virava barreira também.
TEST_CASE(gs_bands, clut_barrier_load_does_not_cascade_into_next_loads) {
    for (unsigned rep = 0; rep < 5; ++rep) {
        StatsGs t;
        Gs& g = *t.gs;
        Lcg r{0xCA5CADE};
        for (unsigned k = 0; k < 4; ++k) {
            std::vector<std::uint64_t> w;
            for (unsigned i = 0; i < 128; ++i) w.push_back(r.word());
            hostToLocal(g, (48 + k) * 32, 1, PSMCT32, 0, 0, 16, 16, w);
        }
        std::vector<std::uint64_t> px;
        for (unsigned i = 0; i < 256; ++i) px.push_back(r.word());
        hostToLocal(g, 1344, 1, PSMT4, 0, 0, 64, 64, px);
        // Desenho grande sobre a página 51 (a CLUT 3) ainda pendente.
        g.writeRegister(FRAME_1, frameReg(51, 2, PSMCT32), 0);
        sprite(g, 0, 0, 127, 127);
        g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
        const std::uint64_t before = g.bandStats().submit[BandStats::SubmitClut];
        g.writeRegister(TEX0_1, tex0(1344, 1, PSMT4, 6, 6, 51 * 32, PSMCT32, 1, 0), 0);  // conflito real
        for (unsigned k = 0; k < 3; ++k) {
            g.writeRegister(TEX0_1, tex0(1344, 1, PSMT4, 6, 6, (48 + k) * 32, PSMCT32, 1, k), 0);
            sprite(g, 0, 0, 63, 63, true);
        }
        const BandStats& s = g.bandStats();
        CHECK(s.submit[BandStats::SubmitClut] - before <= 1u);
        CHECK_EQ(s.clutDirect + s.submit[BandStats::SubmitClut], 4u);
    }
}
