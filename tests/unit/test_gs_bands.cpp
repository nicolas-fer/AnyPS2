// Testes das faixas do GS (gs_bands.cpp e gs_worker.cpp com várias threads).
// Com faixas, a imagem tem de ser idêntica à do caminho síncrono
// (ANYPS2_GS_THREAD=0), byte a byte na VRAM inteira, para qualquer número de
// faixas; e a contagem analítica tem de continuar batendo com o rasterizador.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <thread>
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

TEST_CASE(gs_bands, page_seq_tracker_is_exact_and_never_loses_track) {
    // Muito mais acessos que a lista outstanding_ comporta: cada página guarda só a
    // barreira que conclui o último acesso, então nada se perde e nada sobra.
    PageSeqTracker t;
    for (std::uint32_t i = 0; i < 1000; ++i) {
        VramAccess r{};
        r.pages = pagesRange(i % 400, i % 400);
        t.add(r, i + 1);  // leitura da página i % 400, concluída pela barreira i + 1
    }
    VramAccess w{};
    w.write = true;
    w.pages = pagesRange(450, 460);
    CHECK(!t.conflicts(w, 0));  // páginas nunca acessadas
    VramAccess rd{};
    rd.pages = pagesRange(0, 399);
    CHECK(!t.conflicts(rd, 0));  // leitura contra leituras não conflita
    w.pages = pagesRange(10, 10);
    CHECK(t.conflicts(w, 800));   // último acesso à página 10: barreira 811
    CHECK(!t.conflicts(w, 811));  // já concluída
    VramAccess wr{};
    wr.write = true;
    wr.pages = pagesRange(500, 501);
    t.add(wr, 2000);
    rd.pages = pagesRange(501, 501);
    CHECK(t.conflicts(rd, 1999));
    CHECK(!t.conflicts(rd, 2000));
    t.clear();
    CHECK(!t.conflicts(rd, 0));
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
            CHECK_EQ(got.stats.clutDirect + got.stats.clutLazy + got.stats.submit[BandStats::SubmitClut], ref.stats.clutDirect);
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
            CHECK_EQ(got.stats.clutDirect + got.stats.clutLazy + got.stats.submit[BandStats::SubmitClut], ref.stats.clutDirect);
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
    // Todo upload é direto (o produtor escreve na VRAM) ou, com acesso pendente no destino, em fila.
    CHECK_EQ(ref.stats.hostDirect, 12u * 5 + 1);
    CHECK_EQ(ref.stats.hostBarrier + ref.stats.hostOrder, 0u);  // sem thread não há o que esperar
    for (unsigned rep = 0; rep < 5; ++rep) {
        for (unsigned lanes : {2u, 3u, 4u}) {
            const UploadRender got = renderUploads(true, lanes);
            CHECK(got.img.vram == ref.img.vram);
            CHECK_EQ(got.stats.hostDirect + got.stats.hostBarrier + got.stats.hostQueued, ref.stats.hostDirect);
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
            // Direta ou em fila, nunca com espera no início (depende do tempo).
            CHECK_EQ(got.stats.hostDirect + got.stats.hostQueued, 11u);
            CHECK_EQ(got.stats.hostBarrier, 0u);
            // Direta, o upload não fica pendente nas faixas e nenhuma carga de CLUT precisa de
            // barreira; em fila, a escrita pendente da área faz algumas virarem barreira.
            CHECK_EQ(got.stats.clutDirect + got.stats.clutLazy + got.stats.submit[BandStats::SubmitClut], ref.stats.clutDirect);
            if (got.stats.hostQueued == 0) CHECK_EQ(got.stats.submit[BandStats::SubmitClut], 0u);
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
    // clutDep = falso: ANYPS2_GS_CLUT_DEP=0 (as cargas de CLUT que conflitam viram sempre
    // barreira); os testes de classificação da fatia 1 medem esse caminho.
    explicit StatsGs(bool clutDep = true) {
        setEnv("ANYPS2_GS_THREAD", "1");
        setEnv("ANYPS2_GS_THREADS", "4");
        setEnv("ANYPS2_GS_CLUT_DEP", clutDep ? "" : "0");
        gs = std::make_unique<Gs>(nullptr);
        setEnv("ANYPS2_GS_THREAD", "");
        setEnv("ANYPS2_GS_THREADS", "");
        setEnv("ANYPS2_GS_CLUT_DEP", "");
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

TEST_CASE(gs_bands, upload_over_pending_readers_is_queued_without_waiting) {
    StatsGs t;
    Gs& g = *t.gs;
    // Leitores pendentes da área (textura da página 40), todos grandes para seguirem nas faixas.
    g.writeRegister(TEX0_1, tex0(40 * 32, 1, PSMCT32, 6, 6), 0);
    for (unsigned i = 0; i < 20; ++i) sprite(g, 0, 0, 100, 63, true);
    hostToLocal(g, 40 * 32, 1, PSMCT32, 0, 0, 64, 32, std::vector<std::uint64_t>(1024, 0x1234));
    const BandStats& s = g.bandStats();
    // Em fila (ou direta, se as faixas já tinham terminado): o EE não espera no início.
    CHECK_EQ(s.hostDirect + s.hostQueued, 1u);
    CHECK_EQ(s.hostBarrier, 0u);
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

// ---------------------------------------------------------------------------
// HOST→LOCAL em modo fila: o destino tem acesso não concluído nas faixas, então a
// transferência não faz o EE esperar; as palavras viram uma barreira ordenada. O
// resultado (VRAM inteira e o que o LOCAL→HOST devolve) tem de ser o do modo síncrono.
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kQT = 40 * 32;  // textura/destino PSMCT32 de 64x64 (páginas 40 e 41)

struct QueueRun {
    Render img;
    BandStats stats;
    std::vector<std::uint8_t> download;
    std::uint64_t mismatches = 0;
};

void queueSetup(Gs& g) {
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
    g.writeRegister(ZBUF_1, 1ull << 32, 0);
    g.writeRegister(SCISSOR_1, scissorReg(0, 127, 0, 127), 0);
    g.writeRegister(TEST_1, 0, 0);
    g.writeRegister(XYOFFSET_1, 0, 0);
    g.writeRegister(PRMODECONT, 1, 0);
    g.writeRegister(COLCLAMP, 1, 0);
    g.writeRegister(ALPHA_1, 0 | (2ull << 2) | (1ull << 6) | (0x80ull << 32), 0);  // Cs·As + Cd
}

// Sprites com alfa (a ordem importa) sobre um retângulo w x h do buffer (fbp em páginas).
void queueBusy(Gs& g, unsigned fbp, unsigned fbw, unsigned w, unsigned h, unsigned n, unsigned salt) {
    g.writeRegister(FRAME_1, frameReg(fbp, fbw, PSMCT32), 0);
    g.writeRegister(PRIM, prim(6, false, false, true), 0);
    for (unsigned i = 0; i < n; ++i) {
        const std::uint64_t c =
            rgbaq(40 + (i * 29 + salt * 7) % 200, (i * 13 + salt) % 256, 90 + salt % 50, 0x28 + i % 16);
        vertex(g, 0, 0, c);
        vertex(g, w, h, c);
    }
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
}

// Sprite opaco que lê a textura de 64x64 em `tbp` e escreve no FRAME 0.
void queueTexSprite(Gs& g, std::uint32_t tbp, unsigned k) {
    g.writeRegister(TEX0_1, tex0(tbp, 1, PSMCT32, 6, 6), 0);
    g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
    vertex(g, 2 + k % 7, 5 + k % 5, rgbaq(255, 255, 255, 0x80), 0, uv(0, 0));
    vertex(g, 70 + k % 7, 90 + k % 9, rgbaq(255, 255, 255, 0x80), 0, uv(63, 63));
}

// Sprite que lê a textura PSMT8 em kQT com a CLUT de `cbp` (CLD = 1: carrega a CLUT).
void queueClutSprite(Gs& g, std::uint32_t cbp, unsigned k) {
    g.writeRegister(TEX0_1, tex0(kQT, 1, PSMT8, 6, 6, cbp, PSMCT32, 1), 0);
    g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
    vertex(g, 1 + k % 5, 4 + k % 3, rgbaq(255, 255, 255, 0x80), 0, uv(0, 0));
    vertex(g, 90 + k % 5, 100 + k % 3, rgbaq(255, 255, 255, 0x80), 0, uv(63, 63));
}

void queueBegin(Gs& g, std::uint32_t dbp, std::uint32_t dbw, std::uint32_t psm, std::uint32_t x, std::uint32_t y,
                std::uint32_t w, std::uint32_t h) {
    g.writeRegister(BITBLTBUF, (std::uint64_t{dbp} << 32) | (std::uint64_t{dbw} << 48) | (std::uint64_t{psm} << 56), 0);
    g.writeRegister(TRXPOS, (std::uint64_t{x} << 32) | (std::uint64_t{y} << 48), 0);
    g.writeRegister(TRXREG, w | (std::uint64_t{h} << 32), 0);
    g.writeRegister(TRXDIR, 0, 0);
}

void queueSend(Gs& g, const std::vector<std::uint64_t>& v, std::size_t from, std::size_t to) {
    for (std::size_t i = from; i < to; ++i) g.writeRegister(HWREG, v[i], 0);
}

// LOCAL→HOST do retângulo; os bytes entregues vão para `out`.
void queueDownload(Gs& g, std::vector<std::uint8_t>& out, std::uint32_t sbp, std::uint32_t sbw, std::uint32_t psm,
                   std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
    g.writeRegister(BITBLTBUF, std::uint64_t{sbp} | (std::uint64_t{sbw} << 16) | (std::uint64_t{psm} << 24), 0);
    g.writeRegister(TRXPOS, (std::uint64_t{x}) | (std::uint64_t{y} << 16), 0);
    g.writeRegister(TRXREG, w | (std::uint64_t{h} << 32), 0);
    g.writeRegister(TRXDIR, 1, 0);
    const std::size_t q = g.downloadRemaining();
    const std::size_t at = out.size();
    out.resize(at + q * 16);
    g.readDownload(out.data() + at, q);
}

std::vector<std::uint64_t> queueWords(Lcg& r, unsigned n) {
    std::vector<std::uint64_t> v;
    for (unsigned i = 0; i < n; ++i) v.push_back(r.word());
    return v;
}

enum QueueScenario : unsigned {
    QsReaderSeesOld,      // desenhos pendentes que lêem o destino veem a VRAM antiga
    QsWriterThenHost,     // desenhos pendentes que escrevem o destino: o HOST fica por cima
    QsDrawMidTransfer,    // desenhos no meio da transferência: leitor e escritor do destino
    QsClutAfterHost,      // carga de CLUT do destino logo depois (e no meio) do HOST em fila
    QsDownloadAfterHost,  // LOCAL→HOST logo depois (e no meio) do HOST em fila
    QsCopyAfterHost,      // LOCAL→LOCAL do destino no meio de um HOST em fila
    QsHostOverHost,       // HOST em fila sobre HOST em fila (mesma área)
    QsCount
};

QueueRun renderQueue(QueueScenario sc, bool threaded, unsigned lanes, bool texCache) {
    setEnv("ANYPS2_GS_THREAD", threaded ? "1" : "0");
    setEnv("ANYPS2_GS_THREADS", std::to_string(lanes).c_str());
    setEnv("ANYPS2_GS_TEXCACHE", texCache ? "1" : "");
    setEnv("ANYPS2_GS_TEXCACHE_VERIFY", texCache ? "1" : "");
    Gs g(nullptr);
    setEnv("ANYPS2_GS_TEXCACHE", "");
    setEnv("ANYPS2_GS_TEXCACHE_VERIFY", "");
    g.setTexVerifyAbort(false);
    CHECK_EQ(g.threaded(), threaded);
    queueSetup(g);
    Lcg r{0x5EED0000u + sc};
    QueueRun out;
    constexpr unsigned kRounds = 6;
    constexpr unsigned kPending = 14;
    if (sc == QsClutAfterHost) {
        queueBegin(g, kQT, 1, PSMT8, 0, 0, 64, 64);  // índices da textura PSMT8
        const auto idx = queueWords(r, 512);
        queueSend(g, idx, 0, idx.size());
    } else {
        queueBegin(g, kQT, 1, PSMCT32, 0, 0, 64, 64);  // textura inicial: nada pendente ainda
        const auto w = queueWords(r, 2048);
        queueSend(g, w, 0, w.size());
    }
    for (unsigned k = 0; k < kRounds; ++k) {
        switch (sc) {
            case QsReaderSeesOld: {
                for (unsigned i = 0; i < kPending; ++i) queueTexSprite(g, kQT, k + i);
                const auto w = queueWords(r, 2048);
                queueBegin(g, kQT, 1, PSMCT32, 0, 0, 64, 64);
                queueSend(g, w, 0, w.size());
                queueTexSprite(g, kQT, k);  // vê a textura nova
                break;
            }
            case QsWriterThenHost: {
                queueBusy(g, 40, 1, 64, 64, kPending, k);  // escreve o destino (pendente)
                const auto w = queueWords(r, 512);
                queueBegin(g, kQT, 1, PSMCT32, 8 + k, 8, 32, 32);  // retângulo no meio
                queueSend(g, w, 0, w.size());
                queueTexSprite(g, kQT, k);
                break;
            }
            case QsDrawMidTransfer: {
                for (unsigned i = 0; i < kPending; ++i) queueTexSprite(g, kQT, k + i);
                const auto w = queueWords(r, 2048);
                queueBegin(g, kQT, 1, PSMCT32, 0, 0, 64, 64);
                queueSend(g, w, 0, 700);
                queueTexSprite(g, kQT, k + 1);      // lê: só as primeiras 700 palavras são novas
                queueSend(g, w, 700, 1400);
                queueBusy(g, 40, 1, 64, 40, 3, k);  // escreve o destino no meio da transferência
                queueSend(g, w, 1400, w.size());    // o resto vai por cima do desenho
                queueTexSprite(g, kQT, k + 2);
                break;
            }
            case QsClutAfterHost: {
                queueBusy(g, 48, 1, 63, 31, kPending, k);  // escreve a página da CLUT
                const auto c1 = queueWords(r, 128), c2 = queueWords(r, 128);
                queueBegin(g, 48 * 32, 1, PSMCT32, 0, 0, 16, 16);
                queueSend(g, c1, 0, c1.size());
                queueClutSprite(g, 48 * 32, k);  // carga logo depois do HOST em fila
                queueBusy(g, 48, 1, 63, 31, kPending, k + 9);
                queueBegin(g, 48 * 32, 1, PSMCT32, 0, 0, 16, 16);
                queueSend(g, c2, 0, 64);  // metade
                queueClutSprite(g, 48 * 32, k + 1);
                queueSend(g, c2, 64, c2.size());  // o resto
                queueClutSprite(g, 48 * 32, k + 2);
                break;
            }
            case QsDownloadAfterHost: {
                queueBusy(g, 40, 1, 64, 64, kPending, k);
                const auto w = queueWords(r, 2048);
                queueBegin(g, kQT, 1, PSMCT32, 0, 0, 64, 64);
                queueSend(g, w, 0, 1000);
                queueDownload(g, out.download, kQT, 1, PSMCT32, 0, 0, 64, 40);  // só as 1000 primeiras são novas
                queueSend(g, w, 1000, w.size());
                queueDownload(g, out.download, kQT, 1, PSMCT32, 0, 0, 64, 64);
                break;
            }
            case QsCopyAfterHost: {
                queueBusy(g, 40, 1, 64, 64, kPending, k);
                const auto w = queueWords(r, 2048);
                queueBegin(g, kQT, 1, PSMCT32, 0, 0, 64, 64);
                queueSend(g, w, 0, 900);
                // LOCAL→LOCAL do destino (PSMCT32 64 px) para o FRAME 0, no meio da transferência.
                g.writeRegister(BITBLTBUF, std::uint64_t{kQT} | (1ull << 16) | (std::uint64_t{PSMCT32} << 24) |
                                               (0ull << 32) | (2ull << 48) | (std::uint64_t{PSMCT32} << 56),
                                0);
                g.writeRegister(TRXPOS, std::uint64_t{8} | (std::uint64_t{4} << 16) | (std::uint64_t{20 + k} << 32) |
                                            (std::uint64_t{30} << 48),
                                0);
                g.writeRegister(TRXREG, 40 | (std::uint64_t{50} << 32), 0);
                g.writeRegister(TRXDIR, 2, 0);
                queueSend(g, w, 900, w.size());
                queueTexSprite(g, kQT, k);
                break;
            }
            case QsHostOverHost: {
                queueBusy(g, 40, 1, 64, 64, kPending, k);
                const auto a = queueWords(r, 2048), b = queueWords(r, 512);
                queueBegin(g, kQT, 1, PSMCT32, 0, 0, 64, 64);
                queueSend(g, a, 0, a.size());
                queueBegin(g, kQT, 1, PSMCT32, 16, 16, 32, 32);  // por cima da anterior, ainda na fila
                queueSend(g, b, 0, b.size());
                queueTexSprite(g, kQT, k);
                break;
            }
            default:
                break;
        }
    }
    out.stats = g.bandStats();
    out.mismatches = g.texVerifyMismatches();
    out.img = capture(g);
    return out;
}

void checkQueueScenario(QueueScenario sc, bool texCache) {
    const QueueRun ref = renderQueue(sc, false, 1, false);
    bool any = false;
    for (std::uint8_t b : ref.img.vram) any = any || b != 0;
    CHECK(any);
    CHECK_EQ(ref.stats.hostQueued, 0u);  // sem thread não há acesso pendente
    std::uint64_t queued = 0;
    for (unsigned rep = 0; rep < 5; ++rep) {
        for (unsigned lanes : {1u, 2u, 4u}) {
            const QueueRun got = renderQueue(sc, true, lanes, texCache);
            CHECK(got.img.vram == ref.img.vram);
            CHECK(got.download == ref.download);
            CHECK_EQ(got.img.shaded, got.img.covered);
            CHECK_EQ(got.mismatches, 0u);
            // Nenhuma espera no início: ou direta, ou em fila.
            CHECK_EQ(got.stats.hostBarrier, 0u);
            CHECK_EQ(got.stats.hostDirect + got.stats.hostQueued, ref.stats.hostDirect);
            queued += got.stats.hostQueued;
        }
    }
    CHECK(queued > 0);  // o modo fila foi exercitado
}

}  // namespace

TEST_CASE(gs_bands, host_queue_pending_reader_sees_old_vram) { checkQueueScenario(QsReaderSeesOld, false); }
TEST_CASE(gs_bands, host_queue_over_pending_writer_ends_on_top) { checkQueueScenario(QsWriterThenHost, false); }
TEST_CASE(gs_bands, host_queue_draws_in_the_middle_see_only_sent_words) { checkQueueScenario(QsDrawMidTransfer, false); }
TEST_CASE(gs_bands, host_queue_clut_load_right_after) { checkQueueScenario(QsClutAfterHost, false); }
TEST_CASE(gs_bands, host_queue_local_to_host_right_after) { checkQueueScenario(QsDownloadAfterHost, false); }
TEST_CASE(gs_bands, host_queue_local_to_local_in_the_middle) { checkQueueScenario(QsCopyAfterHost, false); }
TEST_CASE(gs_bands, host_queue_over_host_queue) { checkQueueScenario(QsHostOverHost, false); }

TEST_CASE(gs_bands, host_queue_with_texture_cache_verify) {
    for (unsigned sc = 0; sc < QsCount; ++sc) checkQueueScenario(static_cast<QueueScenario>(sc), true);
}

TEST_CASE(gs_bands, host_queue_counts_transfers_and_submits) {
    // Leitores pendentes do destino fazem a transferência abrir em fila; partida em duas por
    // um desenho que a lê, vira duas submissões.
    bool seen = false;
    for (unsigned rep = 0; rep < 20 && !seen; ++rep) {
        StatsGs t;
        Gs& g = *t.gs;
        Lcg r{0xC0FFEE};
        queueBegin(g, kQT, 1, PSMCT32, 0, 0, 64, 64);
        const auto w0 = queueWords(r, 2048);
        queueSend(g, w0, 0, w0.size());
        g.waitIdle();
        for (unsigned i = 0; i < 40; ++i) queueTexSprite(g, kQT, i);
        const auto w = queueWords(r, 2048);
        queueBegin(g, kQT, 1, PSMCT32, 0, 0, 64, 64);
        queueSend(g, w, 0, 1024);
        queueTexSprite(g, kQT, 1);
        queueSend(g, w, 1024, w.size());
        const BandStats& s = g.bandStats();
        if (s.hostQueued == 0) continue;
        seen = true;
        CHECK_EQ(s.hostQueued, 1u);
        CHECK_EQ(s.hostQueuedSubmits, 2u);
        CHECK_EQ(s.hostQueuedWords, 2048u);
        CHECK_EQ(s.submit[BandStats::SubmitHost], 2u);
        CHECK_EQ(s.hostBarrier, 0u);
        CHECK_EQ(g.vram().read32(Vram::byteAddress32(kQT, 1, 0, 0)), static_cast<std::uint32_t>(w[0]));
    }
    CHECK(seen);
}

// ---------------------------------------------------------------------------
// Saída de vídeo assíncrona (requestDisplay/latestDisplay): o quadro composto por uma
// barreira na fila tem de ser idêntico ao display() síncrono do mesmo ponto do programa,
// para qualquer número de faixas, mesmo com HOST→LOCAL em fila antes do pedido e com o
// vídeo reconfigurado logo depois dele.
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kPmode = 0x12000000u, kDispfb1 = 0x12000070u, kDisplay1 = 0x12000080u,
                        kBgcolor = 0x120000E0u;

std::uint64_t dispfbReg(std::uint64_t fbp, std::uint64_t fbw, std::uint64_t psm) {
    return fbp | (fbw << 9) | (psm << 15);
}
// DISPLAY1 sem ampliação: w x h pixels.
std::uint64_t displayReg(std::uint64_t w, std::uint64_t h) {
    return ((w - 1) << 32) | ((h - 1) << 44);
}

void videoOn(Gs& g) {
    g.writePrivileged(kPmode, 1, 0);
    g.writePrivileged(kDispfb1, dispfbReg(0, 2, PSMCT32), 0);
    g.writePrivileged(kDisplay1, displayReg(128, 128), 0);
    g.writePrivileged(kBgcolor, 0x203040, 0);
}

// Reconfigura o vídeo de um jeito que muda o quadro (outro buffer, vídeo desligado, outro
// tamanho e fundo): se o quadro pedido antes lesse isto, sairia diferente.
void videoDisturb(Gs& g, unsigned k) {
    switch (k % 3) {
        case 0: g.writePrivileged(kDispfb1, dispfbReg(40, 1, PSMCT32), 0); break;
        case 1: g.writePrivileged(kPmode, 0, 0); break;
        default:
            g.writePrivileged(kDisplay1, displayReg(64, 32), 0);
            g.writePrivileged(kBgcolor, 0xFF00FF, 0);
            break;
    }
}

constexpr unsigned kDispSteps = 8;

// O mesmo programa para a referência e para o teste: `checkpoint(k)` é o VBlank do passo k
// (display() síncrono ou requestDisplay); o vídeo é reconfigurado logo depois dele e
// restaurado no passo seguinte. Nos passos pares há uma HOST→LOCAL no framebuffer que
// continua depois do VBlank.
template <class F>
void displayProgram(Gs& g, F&& checkpoint) {
    queueSetup(g);
    Lcg r{0xD15B1A7};
    for (unsigned k = 0; k < kDispSteps; ++k) {
        videoOn(g);
        queueBusy(g, 0, 2, 100, 100, 24, k);  // escreve o framebuffer (pendente nas faixas)
        if (k % 2 == 0) {
            // Destino com escrita pendente: a transferência abre em fila. O VBlank cai no meio.
            const auto w = queueWords(r, 256);
            queueBegin(g, 0, 2, PSMCT32, 4 + k, 4, 32, 16);
            queueSend(g, w, 0, 100);
            checkpoint(k);
            videoDisturb(g, k);
            queueSend(g, w, 100, w.size());
        } else {
            checkpoint(k);
            videoDisturb(g, k);
        }
    }
}

std::unique_ptr<Gs> makeDisplayGs(bool threaded, unsigned lanes) {
    setEnv("ANYPS2_GS_THREAD", threaded ? "1" : "0");
    setEnv("ANYPS2_GS_THREADS", std::to_string(lanes).c_str());
    auto g = std::make_unique<Gs>(nullptr);
    setEnv("ANYPS2_GS_THREAD", "");
    setEnv("ANYPS2_GS_THREADS", "");
    return g;
}

bool sameFrame(const Frame& a, const Frame& b) {
    return a.width == b.width && a.height == b.height && a.pixels == b.pixels;
}

std::vector<Frame> referenceFrames() {
    auto g = makeDisplayGs(false, 1);
    std::vector<Frame> out;
    displayProgram(*g, [&](unsigned) { out.push_back(g->display()); });
    return out;
}

}  // namespace

TEST_CASE(gs_bands, async_display_frames_match_sync_display) {
    const std::vector<Frame> ref = referenceFrames();
    CHECK_EQ(ref.size(), std::size_t{kDispSteps});
    // O programa muda o quadro de passo a passo (senão comparar não provaria nada).
    for (std::size_t i = 1; i < ref.size(); ++i) CHECK(!sameFrame(ref[i], ref[i - 1]));
    for (const unsigned lanes : {1u, 2u, 4u}) {
        for (unsigned rep = 0; rep < 6; ++rep) {
            auto g = makeDisplayGs(true, lanes);
            CHECK(g->asyncDisplay());
            std::vector<Frame> got;
            Frame f;
            displayProgram(*g, [&](unsigned) {
                g->requestDisplay();
                // Espera a entrega do quadro pedido (as escritas de vídeo do programa vêm
                // logo depois do pedido e ainda podem correr contra a composição).
                for (unsigned spin = 0; !g->latestDisplay(f); ++spin) {
                    CHECK(spin < 50000000u);
                    if (spin > 1000) std::this_thread::yield();
                }
                got.push_back(f);
            });
            CHECK_EQ(got.size(), ref.size());
            for (std::size_t i = 0; i < got.size() && i < ref.size(); ++i) CHECK(sameFrame(got[i], ref[i]));
            CHECK_EQ(g->bandStats().displayQueued, std::uint64_t{kDispSteps});
            CHECK_EQ(g->bandStats().submit[BandStats::SubmitDisplay], std::uint64_t{kDispSteps});
            // A VRAM final é a do caminho síncrono.
            auto s = makeDisplayGs(false, 1);
            displayProgram(*s, [&](unsigned) {});
            CHECK(std::memcmp(g->vram().data(), s->vram().data(), Vram::kSize) == 0);
        }
    }
}

TEST_CASE(gs_bands, async_display_pipelined_never_more_than_one_frame_behind) {
    const std::vector<Frame> ref = referenceFrames();
    for (const unsigned lanes : {1u, 2u, 4u}) {
        for (unsigned rep = 0; rep < 6; ++rep) {
            auto g = makeDisplayGs(true, lanes);
            Frame f;
            unsigned last = 0;  // pedido (1-based) do último quadro entregue: nunca anda para trás
            displayProgram(*g, [&](unsigned k) {
                g->requestDisplay();
                // Sem esperar: entrega o que houver. A partir do segundo pedido o anterior já
                // ficou pronto (limite de atraso), então sempre há quadro novo: o k-1 ou o k.
                if (g->latestDisplay(f)) {
                    const unsigned idx = k + 1;
                    const bool isCur = sameFrame(f, ref[k]);
                    const bool isPrev = k > 0 && sameFrame(f, ref[k - 1]);
                    CHECK(isCur || isPrev);
                    const unsigned got = isCur ? idx : idx - 1;
                    CHECK(got > last);
                    last = got;
                }
                // O quadro do pedido anterior (k na contagem 1-based) já tem de ter sido entregue.
                CHECK(last >= k);
            });
            g->waitIdle();
            // Depois de tudo pronto, o mais recente é o último pedido (se ainda não entregue).
            if (g->latestDisplay(f)) CHECK(sameFrame(f, ref[kDispSteps - 1]));
            CHECK(!g->latestDisplay(f));
        }
    }
}

TEST_CASE(gs_bands, async_display_without_threads_matches_sync) {
    const std::vector<Frame> ref = referenceFrames();
    auto g = makeDisplayGs(false, 1);
    CHECK(!g->asyncDisplay());  // o Runtime só usa o caminho assíncrono com worker em thread
    std::vector<Frame> got;
    Frame f;
    displayProgram(*g, [&](unsigned) {
        g->requestDisplay();
        CHECK(g->latestDisplay(f));
        CHECK(!g->latestDisplay(f));  // nada novo: não entrega o mesmo quadro duas vezes
        got.push_back(f);
    });
    CHECK_EQ(got.size(), ref.size());
    for (std::size_t i = 0; i < got.size() && i < ref.size(); ++i) CHECK(sameFrame(got[i], ref[i]));
}

TEST_CASE(gs_bands, async_display_env_switch_and_stats) {
    setEnv("ANYPS2_GS_ASYNC_DISPLAY", "0");
    auto off = makeDisplayGs(true, 2);
    setEnv("ANYPS2_GS_ASYNC_DISPLAY", "");
    CHECK(!off->asyncDisplay());
    auto on = makeDisplayGs(true, 2);
    CHECK(on->asyncDisplay());
    // Pedidos seguidos sem entregar: cada um espera o anterior (no máximo um quadro de atraso).
    queueSetup(*on);
    videoOn(*on);
    for (unsigned i = 0; i < 20; ++i) {
        queueBusy(*on, 0, 2, 120, 120, 40, i);
        on->requestDisplay();
    }
    on->waitIdle();
    const BandStats& s = on->bandStats();
    CHECK_EQ(s.displayQueued, 20u);
    CHECK(s.displayLagWaits <= 19u);
    CHECK_EQ(s.waits[BandStats::WaitDisplay], s.displayLagWaits);
    Frame f;
    CHECK(on->latestDisplay(f));
    CHECK_EQ(f.width, 128u);
    CHECK_EQ(f.height, 128u);
    CHECK(sameFrame(f, on->display()));
}

// ---------------------------------------------------------------------------
// Contadores de CLUT e espera por tipo de barreira (fatia 1 do plano de dependência de
// CLUT): só medem; aqui conferem a classificação em cenas sintéticas. A carga só vira
// barreira se as faixas ainda estão ocupadas, então cada cena é tentada algumas vezes
// (desenhos pesados antes dela) até as pré-condições valerem.
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint32_t kClutPage = 51 * 32;  // CBP da CLUT nos testes (página 51)

// Trabalho pesado em outro lugar (FRAME 0), para manter as faixas ocupadas.
void clutBusy(Gs& g) {
    queueBusy(g, 0, 2, 127, 127, 150, 0);
}

// Carga de CLUT PSMT4 (8x2 pixels PSMCT32 no bloco 0 da página 51).
void clutLoad(Gs& g) {
    g.writeRegister(TEX0_1, tex0(1344, 1, PSMT4, 6, 6, kClutPage, PSMCT32, 1, 0), 0);
}

// Desenho de sprite no FRAME da página 51 (a mesma da CLUT).
void clutPageSprite(Gs& g, unsigned x0, unsigned y0, unsigned x1, unsigned y1) {
    g.writeRegister(FRAME_1, frameReg(51, 2, PSMCT32), 0);
    sprite(g, x0, y0, x1, y1);
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
}

template <class F>
void clutScene(F&& scene) {
    bool ok = false;
    for (unsigned attempt = 0; attempt < 30 && !ok; ++attempt) {
        StatsGs t(false);
        Gs& g = *t.gs;
        g.forceClutMeasure(true);
        queueSetup(g);
        ok = scene(g);
    }
    CHECK(ok);
}

std::uint64_t sumRecent(const BandStats& s) {
    std::uint64_t n = 0;
    for (const std::uint64_t r : s.clutRecent) n += r;
    return n;
}

}  // namespace

TEST_CASE(gs_bands, clut_stats_covered_and_recent_four_lanes) {
    clutScene([](Gs& g) {
        clutBusy(g);
        clutPageSprite(g, 0, 0, 127, 127);  // escreve a página da CLUT nas 4 faixas
        clutLoad(g);                        // recente: escrita ainda sem barreira
        clutLoad(g);                        // coberta: a barreira da anterior já foi emitida
        const BandStats& s = g.bandStats();
        if (s.clutBarriers != 2) return false;  // as faixas já tinham terminado: tenta de novo
        CHECK_EQ(s.clutRecent[4], 1u);
        CHECK_EQ(sumRecent(s), 1u);
        CHECK_EQ(s.clutCovered, 1u);
        CHECK_EQ(s.clutRecentTrueBlock, 1u);
        CHECK_EQ(s.clutHost, 0u);
        CHECK_EQ(s.clutLost, 0u);
        CHECK_EQ(s.submit[BandStats::SubmitClut], 2u);
        CHECK_EQ(s.barrierKind[BandStats::BkClut], 2u);
        std::uint64_t lag = 0;
        for (const std::uint64_t n : s.clutLag) lag += n;
        CHECK_EQ(lag, 2u);
        return true;
    });
}

TEST_CASE(gs_bands, clut_stats_recent_one_lane_true_block_and_false_sharing) {
    // Escrita só nas linhas 0..15 (faixa 0): sobrepõe o bloco 0 da CLUT (x < 8)...
    clutScene([](Gs& g) {
        clutBusy(g);
        clutPageSprite(g, 0, 0, 127, 15);
        clutLoad(g);
        const BandStats& s = g.bandStats();
        if (s.clutBarriers != 1) return false;
        CHECK_EQ(s.clutRecent[1], 1u);
        CHECK_EQ(sumRecent(s), 1u);
        CHECK_EQ(s.clutRecentTrueBlock, 1u);
        CHECK_EQ(s.clutCovered, 0u);
        return true;
    });
    // ...ou só as colunas 16..: mesma página, outros blocos (conflito falso de página).
    clutScene([](Gs& g) {
        clutBusy(g);
        clutPageSprite(g, 16, 0, 127, 15);
        clutLoad(g);
        const BandStats& s = g.bandStats();
        if (s.clutBarriers != 1) return false;
        CHECK_EQ(s.clutRecent[1], 1u);
        CHECK_EQ(s.clutRecentTrueBlock, 0u);
        return true;
    });
}

TEST_CASE(gs_bands, clut_stats_host_queue_and_war_after_covered_load) {
    clutScene([](Gs& g) {
        clutBusy(g);
        clutPageSprite(g, 0, 0, 127, 127);
        // HOST→LOCAL sobre a página da CLUT com escrita pendente: abre em modo fila.
        hostToLocal(g, kClutPage, 1, PSMCT32, 0, 0, 16, 16, std::vector<std::uint64_t>(128, 0x1122334455667788ull));
        clutLoad(g);  // o conflito vem do HOST em fila (coberto pela barreira dele)
        // Escrita na página da CLUT antes da próxima barreira: a leitura virou WAR.
        clutPageSprite(g, 0, 0, 127, 15);
        const BandStats& s = g.bandStats();
        if (s.hostQueued != 1 || s.clutBarriers != 1) return false;
        CHECK_EQ(s.clutHost, 1u);
        CHECK_EQ(s.clutCovered, 1u);
        CHECK_EQ(s.clutWarAfter, 1u);
        CHECK_EQ(s.clutWarAfterCovered, 1u);
        return true;
    });
}

TEST_CASE(gs_bands, clut_stats_war_not_counted_after_a_barrier) {
    clutScene([](Gs& g) {
        clutBusy(g);
        clutPageSprite(g, 0, 0, 127, 127);
        clutLoad(g);
        clutLoad(g);  // barreira: a leitura da primeira deixa de ser acompanhada
        if (g.bandStats().clutBarriers != 2) return false;
        const std::uint64_t before = g.bandStats().clutWarAfter;
        CHECK_EQ(before, 0u);
        clutPageSprite(g, 0, 0, 127, 15);  // colide só com a segunda
        CHECK_EQ(g.bandStats().clutWarAfter, 1u);
        clutPageSprite(g, 0, 0, 127, 15);  // já contada: não repete
        CHECK_EQ(g.bandStats().clutWarAfter, 1u);
        return true;
    });
}

TEST_CASE(gs_bands, barrier_kinds_and_waits_are_counted_per_lane) {
    StatsGs t;
    Gs& g = *t.gs;
    g.forceClutMeasure(true);
    queueSetup(g);
    clutBusy(g);
    clutPageSprite(g, 0, 0, 127, 127);
    clutLoad(g);                         // CLUT
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
    sprite(g, 0, 0, 100, 100);           // escreve de novo a área do desenho anterior: conflito
    g.waitIdle();
    const BandStats& s = g.bandStats();
    std::uint64_t kinds = 0;
    for (const std::uint64_t n : s.barrierKind) kinds += n;
    std::uint64_t submits = 0;
    for (const std::uint64_t n : s.submit) submits += n;
    CHECK_EQ(kinds, submits + s.conflict[BandStats::ConflictFull] + s.conflict[BandStats::ConflictWrite] +
                        s.conflict[BandStats::ConflictTexture]);
    CHECK_EQ(s.barrierKind[BandStats::BkConflict], s.conflict[BandStats::ConflictFull] +
                                                       s.conflict[BandStats::ConflictWrite] +
                                                       s.conflict[BandStats::ConflictTexture]);
    CHECK_EQ(s.lanes, 4u);
    std::int64_t byKind = 0, byLane = 0;
    for (const std::int64_t n : s.barrierWaitNs) {
        CHECK(n >= 0);
        byKind += n;
    }
    for (const std::int64_t n : s.laneWaitNs) byLane += n;
    CHECK_EQ(byKind, byLane);
}

TEST_CASE(gs_bands, block_overlap_distinguishes_blocks_of_the_same_page) {
    // Escrita PSMCT32 em colunas 16..63 × linhas 0..15 da página 0 (largura 2) e uma CLUT
    // de 8x2 pixels no canto (0, 0): mesma página, blocos diferentes.
    VramAccess w{};
    w.write = true;
    w.psm = PSMCT32;
    w.bw = 2;
    w.bp = 0;
    w.rx0 = 16; w.ry0 = 0; w.rx1 = 63; w.ry1 = 15;
    w.rectValid = true;
    w.pages.setRange(0, 0);
    VramAccess c{};
    c.psm = PSMCT32;
    c.bw = 1;
    c.bp = 0;
    c.rx0 = 0; c.ry0 = 0; c.rx1 = 7; c.ry1 = 1;
    c.rectValid = true;
    c.pages.setRange(0, 0);
    CHECK(accessesOverlap(w, c));
    CHECK(!accessesOverlapBlocks(w, c));
    w.rx0 = 0;
    CHECK(accessesOverlapBlocks(w, c));
    w.rectValid = false;  // sem caixa: aproxima por cima
    w.rx0 = 16;
    CHECK(accessesOverlapBlocks(w, c));
}

// ---------------------------------------------------------------------------
// ClutBuffer com leitura diferida (fatia 2 do plano de dependência de CLUT): a versão guarda
// como ler a VRAM depois, e resolve() executa a leitura uma vez só, de baixo para cima.
// ---------------------------------------------------------------------------

namespace {

// Cadeia de `n` versões: as de índice par são diretas (células na hora), as ímpares são
// diferidas (células na primeira leitura). `reads[i]` conta quantas vezes a leitura da versão
// i rodou. `expect` é a tabela esperada depois de resolver a última.
struct LazyChain {
    std::vector<std::shared_ptr<ClutBuffer>> v;
    std::unique_ptr<std::atomic<unsigned>[]> reads;
    std::array<std::uint16_t, 512> expect{};

    explicit LazyChain(unsigned n, unsigned lazyEvery = 2) : reads(new std::atomic<unsigned>[n]) {
        std::shared_ptr<ClutBuffer> prev;
        for (unsigned i = 0; i < n; ++i) {
            reads[i].store(0);
            auto b = std::make_shared<ClutBuffer>();
            b->base = prev;
            // Cada versão troca 3 posições (algumas repetem as de versões anteriores).
            const auto cells = [i](ClutBuffer& dst) {
                dst.set((i * 7u) % 512, static_cast<std::uint16_t>(0x1000 + i));
                dst.set((i * 13u + 5u) % 512, static_cast<std::uint16_t>(0x2000 + i));
                dst.set((i % 16u) * 3u, static_cast<std::uint16_t>(0x3000 + i));
            };
            if (lazyEvery != 0 && i % lazyEvery == lazyEvery - 1) {
                std::atomic<unsigned>* counter = &reads[i];
                b->makeLazy([cells, counter](ClutBuffer& dst) {
                    counter->fetch_add(1);
                    cells(dst);
                });
            } else {
                cells(*b);
            }
            // Referência independente da implementação: aplica as células na ordem.
            expect[(i * 7u) % 512] = static_cast<std::uint16_t>(0x1000 + i);
            expect[(i * 13u + 5u) % 512] = static_cast<std::uint16_t>(0x2000 + i);
            expect[(i % 16u) * 3u] = static_cast<std::uint16_t>(0x3000 + i);
            v.push_back(b);
            prev = b;
        }
    }
};

}  // namespace

TEST_CASE(gs_bands, lazy_versions_interleaved_with_direct_resolve_in_order) {
    LazyChain c(9);
    for (unsigned i = 0; i < 9; ++i) {
        CHECK_EQ(c.v[i]->lazy(), i % 2 == 1);
        CHECK_EQ(c.v[i]->ready.load(), i % 2 == 0);
    }
    c.v.back()->resolve();
    CHECK(c.v.back()->v == c.expect);
    for (unsigned i = 0; i < 9; ++i) {
        CHECK(c.v[i]->done.load());
        CHECK(c.v[i]->ready.load());
        CHECK_EQ(c.reads[i].load(), i % 2 == 1 ? 1u : 0u);
    }
    c.v.back()->resolve();  // de novo: nada muda
    for (unsigned i = 0; i < 9; ++i) CHECK_EQ(c.reads[i].load(), i % 2 == 1 ? 1u : 0u);
}

TEST_CASE(gs_bands, lazy_fill_runs_once_and_resolve_uses_the_filled_version) {
    LazyChain c(4);
    // Uma tarefa de faixa lê antes (fillLazy), depois um desenho resolve: a leitura não repete.
    c.v[1]->fillLazy();
    c.v[1]->fillLazy();
    CHECK(c.v[1]->ready.load());
    CHECK(!c.v[1]->done.load());
    CHECK(!c.v[3]->ready.load());
    c.v[3]->resolve();
    CHECK_EQ(c.reads[1].load(), 1u);
    CHECK_EQ(c.reads[3].load(), 1u);
    CHECK(c.v[3]->v == c.expect);
    // Versão do meio resolvida antes da de cima: as duas ficam certas.
    LazyChain d(6);
    d.v[2]->resolve();
    std::array<std::uint16_t, 512> mid{};
    for (unsigned i = 0; i <= 2; ++i) {
        mid[(i * 7u) % 512] = static_cast<std::uint16_t>(0x1000 + i);
        mid[(i * 13u + 5u) % 512] = static_cast<std::uint16_t>(0x2000 + i);
        mid[(i % 16u) * 3u] = static_cast<std::uint16_t>(0x3000 + i);
    }
    CHECK(d.v[2]->v == mid);
    d.v[5]->resolve();
    CHECK(d.v[5]->v == d.expect);
    for (unsigned i = 0; i < 6; ++i) CHECK_EQ(d.reads[i].load(), i % 2 == 1 ? 1u : 0u);
}

TEST_CASE(gs_bands, lazy_resolve_from_many_threads_reads_each_version_once) {
    for (unsigned rep = 0; rep < 40; ++rep) {
        LazyChain c(20);
        std::atomic<bool> go{false};
        std::vector<std::thread> th;
        for (unsigned t = 0; t < 8; ++t) {
            th.emplace_back([&, t] {
                while (!go.load()) std::this_thread::yield();
                // Alguns resolvem a última, outros uma do meio ou só leem (fillLazy), como as faixas.
                if (t % 3 == 0) c.v.back()->resolve();
                if (t % 3 == 1) c.v[11]->resolve();
                if (t % 3 == 2) {
                    for (auto& b : c.v) b->fillLazy();
                }
            });
        }
        go.store(true);
        for (auto& x : th) x.join();
        c.v.back()->resolve();
        CHECK(c.v.back()->v == c.expect);
        for (unsigned i = 0; i < 20; ++i) CHECK_EQ(c.reads[i].load(), i % 2 == 1 ? 1u : 0u);
    }
}

TEST_CASE(gs_bands, long_lazy_chain_resolves_and_destroys_without_recursion) {
    {
        LazyChain c(20000, 3);
        c.v.back()->resolve();
        CHECK(c.v.back()->v == c.expect);
        std::uint64_t reads = 0;
        for (unsigned i = 0; i < 20000; ++i) reads += c.reads[i].load();
        CHECK_EQ(reads, 20000u / 3);
    }
    {
        // Cadeia longa nunca resolvida: o destrutor desencadeia em laço (sem estourar a pilha) e
        // nenhuma leitura diferida roda.
        auto last = [] {
            std::shared_ptr<ClutBuffer> prev;
            for (unsigned i = 0; i < 200000; ++i) {
                auto b = std::make_shared<ClutBuffer>();
                b->base = prev;
                if (i % 2) b->makeLazy([](ClutBuffer& d) { d.set(0, 1); });
                prev = b;
            }
            return prev;
        }();
        CHECK(!last->done.load());
        last.reset();
    }
}

// ---------------------------------------------------------------------------
// Carga de CLUT "coberta" sem barreira (fatia 3, ANYPS2_GS_CLUT_DEP): a versão é lida depois,
// por tarefa numa faixa ou por quem resolve. A VRAM e o quadro têm de sair idênticos aos do
// caminho síncrono, byte a byte, para 1, 2 e 4 faixas, e as escritas que vêm depois da carga
// (desenho, HOST direto, LOCAL→LOCAL) não podem mudar o que a versão lê.
// ---------------------------------------------------------------------------

namespace {

enum ClutDepScenario : unsigned {
    CdHostQueued,    // (a) HOST em fila nas páginas da CLUT, depois cargas e desenhos
    CdLocalCopy,     // (b) carga depois de LOCAL→LOCAL sobre a CLUT
    CdWar,           // (c) carga coberta, desenho que escreve a página (WAR), desenho que usa a CLUT
    CdHostAfter,     // (d) carga coberta seguida de HOST direto no mesmo lugar (abre em fila)
    CdHostOpen,      // (e) carga com HOST direto aberto sobre a área (hostDrain_)
    CdCsm2,          // (f) CSM2 com TEXCLUT
    CdFormats,       // (g) PSMT4 e PSMT8, CT32 e CT16, cadeia maior que o limite
    CdNoDraw,        // (h) carga sem desenho antes da próxima barreira e do display
    CdRenderToClut,  // (i) render-to-CLUT recente: continua barreira
    CdFullList,      // (j) lista outstanding_ cheia (o caso do GT4)
    CdCount
};

constexpr std::uint32_t kCdT8 = 1280, kCdT4 = 1344;
constexpr std::uint64_t kNoLoad = ~(7ull << 61);  // zera o CLD do TEX0: usa a CLUT que já vale

std::uint64_t cdT4(unsigned page, std::uint32_t cpsm, unsigned csa) {
    return tex0(kCdT4, 1, PSMT4, 6, 6, page * 32, cpsm, 1, csa);
}
std::uint64_t cdT8(unsigned page, std::uint32_t cpsm) {
    return tex0(kCdT8, 1, PSMT8, 6, 6, page * 32, cpsm, 1, 0);
}

// CLUTs CT32 de 16x16 nas páginas 48..55 (CBP = página·32), uma CLUT CT16 de 256x4 na página
// 60 (CSM2) e as texturas indexadas de 64x64.
void cdPrelude(Gs& g, Lcg& r) {
    queueSetup(g);
    for (unsigned k = 0; k < 8; ++k) hostToLocal(g, (48 + k) * 32, 1, PSMCT32, 0, 0, 16, 16, queueWords(r, 128));
    hostToLocal(g, 60 * 32, 4, PSMCT16, 0, 0, 256, 4, queueWords(r, 256));
    hostToLocal(g, kCdT8, 1, PSMT8, 0, 0, 64, 64, queueWords(r, 512));
    hostToLocal(g, kCdT4, 1, PSMT4, 0, 0, 64, 64, queueWords(r, 256));
}

// Sprite texturizado que usa a CLUT de `t0` (a escrita do TEX0 carrega a CLUT se CLD pedir),
// nas faixas.
void cdUse(Gs& g, unsigned k, std::uint64_t t0) {
    g.writeRegister(TEX0_1, t0, 0);
    g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
    vertex(g, 2 + k % 7, 3 + k % 5, rgbaq(255, 255, 255, 0x80), 0, uv(0, 0));
    vertex(g, 90 + k % 9, 100 + k % 11, rgbaq(255, 255, 255, 0x80), 0, uv(63, 63));
}

// Desenho que escreve a página inteira da CLUT (render-to-CLUT / WAR), com alfa: o resultado
// depende do conteúdo anterior.
void cdPageWrite(Gs& g, unsigned page, unsigned k) {
    g.writeRegister(FRAME_1, frameReg(page, 1, PSMCT32), 0);
    g.writeRegister(PRIM, prim(6, false, false, true), 0);
    const std::uint64_t c = rgbaq(30 + (k * 37) % 200, (k * 91) % 256, 200 - (k * 13) % 150, 0x50 + k % 40);
    vertex(g, 0, 0, c);
    vertex(g, 63, 31, c);
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
}

// Desenho que escreve a linha de CLUTs CT16 da página 60 (CSM2).
void cdRowWrite(Gs& g, unsigned k) {
    g.writeRegister(FRAME_1, frameReg(60, 4, PSMCT16), 0);
    g.writeRegister(PRIM, prim(6, false, false, true), 0);
    const std::uint64_t c = rgbaq(20 + (k * 53) % 200, (k * 17) % 256, (k * 7) % 256, 0x60);
    vertex(g, 0, 0, c);
    vertex(g, 255, 3, c);
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
}

// Desenho no FRAME 0 com outro formato (mesmo endereço): colide com o anterior e abre uma
// barreira de conflito, esvaziando o pending_ sem esperar as faixas.
void cdClash(Gs& g, unsigned k) {
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
    g.writeRegister(PRIM, prim(6), 0);
    vertex(g, 0, 0, rgbaq(10 + k % 90, 20, 30, 0x80));
    vertex(g, 20, 20, rgbaq(10 + k % 90, 20, 30, 0x80));
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT16), 0);
    vertex(g, 0, 0, rgbaq(90, 20 + k % 90, 30, 0x80));
    vertex(g, 20, 20, rgbaq(90, 20 + k % 90, 30, 0x80));
    g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32), 0);
}

// LOCAL→LOCAL de 16x16 pixels de uma página (CT32, FBW 1) para outra.
void cdLocalCopy(Gs& g, unsigned srcPage, unsigned dstPage) {
    g.writeRegister(BITBLTBUF, (std::uint64_t{srcPage} * 32) | (1ull << 16) | (std::uint64_t{PSMCT32} << 24) |
                                   ((std::uint64_t{dstPage} * 32) << 32) | (1ull << 48) |
                                   (std::uint64_t{PSMCT32} << 56),
                    0);
    g.writeRegister(TRXPOS, 0, 0);
    g.writeRegister(TRXREG, 16 | (16ull << 32), 0);
    g.writeRegister(TRXDIR, 2, 0);
}

void cdScene(Gs& g, unsigned sc, unsigned rep) {
    Lcg r{0xC1D0000u + sc * 977u + rep};
    cdPrelude(g, r);
    const unsigned salt = rep;
    switch (sc) {
        case CdHostQueued: {
            queueBusy(g, 0, 2, 100, 100, 30, salt);
            cdPageWrite(g, 48, salt);
            cdPageWrite(g, 49, salt + 1);
            const auto w = queueWords(r, 128);
            queueBegin(g, 48 * 32, 1, PSMCT32, 0, 0, 16, 16);
            queueSend(g, w, 0, 64);
            cdUse(g, 0, cdT4(48, PSMCT32, 0));  // a fila é submetida antes da carga
            queueSend(g, w, 64, 128);           // o resto vem depois: a primeira versão não o vê
            for (unsigned k = 1; k < 8; ++k) cdUse(g, k, k % 2 ? cdT4(48 + k % 2, PSMCT32, k) : cdT8(48, PSMCT32));
            const auto w2 = queueWords(r, 128);
            queueBegin(g, 49 * 32, 1, PSMCT32, 0, 0, 16, 16);
            queueSend(g, w2, 0, 128);
            for (unsigned k = 8; k < 12; ++k) cdUse(g, k, cdT4(49, PSMCT32, k));
            break;
        }
        case CdLocalCopy: {
            queueBusy(g, 0, 2, 100, 100, 30, salt);
            cdPageWrite(g, 50, salt);
            cdLocalCopy(g, 53, 50);
            for (unsigned k = 0; k < 5; ++k) cdUse(g, k, cdT4(50, PSMCT32, k));
            cdPageWrite(g, 51, salt);
            cdLocalCopy(g, 52, 51);
            for (unsigned k = 5; k < 9; ++k) cdUse(g, k, k % 2 ? cdT8(51, PSMCT32) : cdT4(51, PSMCT32, k));
            break;
        }
        case CdWar: {
            queueBusy(g, 0, 2, 100, 100, 30, salt);
            for (unsigned round = 0; round < 4; ++round) {
                const unsigned p = 48 + round;
                cdPageWrite(g, p, salt + round);
                cdUse(g, round, cdT4(p, PSMCT32, round));                // recente: barreira
                g.writeRegister(TEX0_1, cdT4(p, PSMCT32, round + 3), 0);  // coberta: diferida
                cdPageWrite(g, p, salt + round + 9);                     // WAR: barreira antes do desenho
                cdUse(g, round + 1, cdT4(p, PSMCT32, round + 3) & kNoLoad);  // usa a versão lida antes
                g.writeRegister(TEX0_1, cdT4(p, PSMCT32, round + 5), 0);
                cdUse(g, round + 2, cdT4(p, PSMCT32, round + 5) & kNoLoad);
            }
            break;
        }
        case CdHostAfter: {
            queueBusy(g, 0, 2, 100, 100, 30, salt);
            cdPageWrite(g, 53, salt);
            cdUse(g, 0, cdT4(53, PSMCT32, 0));
            g.writeRegister(TEX0_1, cdT4(53, PSMCT32, 4), 0);  // coberta
            const auto w = queueWords(r, 128);
            hostToLocal(g, 53 * 32, 1, PSMCT32, 0, 0, 16, 16, w);  // HOST sobre a área lida: em fila
            cdUse(g, 1, cdT4(53, PSMCT32, 4) & kNoLoad);           // a versão é a de antes do HOST
            g.writeRegister(TEX0_1, cdT4(53, PSMCT32, 6), 0);
            cdUse(g, 2, cdT4(53, PSMCT32, 6) & kNoLoad);
            break;
        }
        case CdHostOpen: {
            g.waitIdle();
            const auto w = queueWords(r, 128);
            queueBegin(g, 53 * 32, 1, PSMCT32, 0, 0, 16, 16);  // sem acesso pendente: modo direto
            queueSend(g, w, 0, 40);
            queueBusy(g, 0, 2, 100, 100, 30, salt);
            cdPageWrite(g, 53, salt);  // desenho sobre o destino aberto: a próxima palavra espera
            cdClash(g, salt);          // barreira de conflito: o pending_ esvazia
            cdUse(g, 0, cdT4(53, PSMCT32, 0));  // carga com a transferência ainda aberta
            queueSend(g, w, 40, 128);
            cdUse(g, 1, cdT4(53, PSMCT32, 3) & kNoLoad);
            cdUse(g, 2, cdT8(53, PSMCT32));
            break;
        }
        case CdCsm2: {
            queueBusy(g, 0, 2, 100, 100, 30, salt);
            cdRowWrite(g, salt);
            for (unsigned k = 0; k < 8; ++k) {
                g.writeRegister(TEXCLUT, 4 | (std::uint64_t{k % 3} << 6) | (std::uint64_t{k % 4} << 12), 0);
                cdUse(g, k, cdT4(60, PSMCT16, k % 16) | (1ull << 55));
                if (k == 3) cdRowWrite(g, salt + 5);
                if (k == 5) hostToLocal(g, 60 * 32, 4, PSMCT16, 0, 0, 256, 4, queueWords(r, 256));
            }
            break;
        }
        case CdFormats: {
            queueBusy(g, 0, 2, 100, 100, 30, salt);
            for (unsigned k = 0; k < 150; ++k) {
                const unsigned p = 48 + k % 6;
                if (k % 11 == 5) cdPageWrite(g, p, salt + k);
                if (k % 17 == 9) hostToLocal(g, p * 32, 1, PSMCT32, 0, 0, 16, 16, queueWords(r, 128));
                const std::uint32_t cpsm = k % 4 == 3 ? PSMCT16 : PSMCT32;
                cdUse(g, k, k % 3 == 0 ? cdT8(p, cpsm) : cdT4(p, cpsm, (k * 5) % 16));
            }
            break;
        }
        case CdNoDraw: {
            queueBusy(g, 0, 2, 100, 100, 30, salt);
            cdPageWrite(g, 52, salt);
            for (unsigned k = 0; k < 4; ++k) g.writeRegister(TEX0_1, cdT4(52, PSMCT32, k), 0);  // sem desenho
            cdLocalCopy(g, 53, 52);  // a próxima barreira escreve a área
            cdUse(g, 0, cdT4(52, PSMCT32, 3) & kNoLoad);  // a versão ainda é a de antes
            g.writeRegister(TEX0_1, cdT4(52, PSMCT32, 7), 0);
            videoOn(g);
            (void)g.display();  // o vídeo espera as faixas: a leitura já tem de ter acontecido
            cdUse(g, 1, cdT4(52, PSMCT32, 7) & kNoLoad);
            break;
        }
        case CdRenderToClut: {
            for (unsigned k = 0; k < 10; ++k) {
                const unsigned p = 48 + k % 3;
                queueBusy(g, 0, 2, 100, 100, 6, salt + k);
                cdPageWrite(g, p, salt + k);
                cdUse(g, k, cdT4(p, PSMCT32, k));
                cdUse(g, k + 1, cdT8(p, PSMCT32));
            }
            break;
        }
        case CdFullList: {
            // Centenas de barreiras de conflito com as faixas atrasadas: a lista outstanding_
            // (256) enche e perde o rastro; as cargas seguintes não podem confiar nela.
            for (unsigned i = 0; i < 300; ++i) {
                queueBusy(g, 0, 2, 100, 100, 1, i);
                cdClash(g, i);
            }
            cdPageWrite(g, 54, salt);
            cdClash(g, 1);
            for (unsigned k = 0; k < 6; ++k) {
                cdUse(g, k, k % 2 ? cdT8(54, PSMCT32) : cdT4(54, PSMCT32, k));
                if (k == 2) cdPageWrite(g, 54, salt + 7);
            }
            break;
        }
        default: break;
    }
}

struct ClutDepRun {
    Render img;
    Frame frame;
    BandStats stats;
    std::uint64_t mismatches = 0;
};

ClutDepRun runClutDep(unsigned sc, unsigned rep, bool threaded, unsigned lanes, bool dep, bool texCache) {
    setEnv("ANYPS2_GS_THREAD", threaded ? "1" : "0");
    setEnv("ANYPS2_GS_THREADS", std::to_string(lanes).c_str());
    setEnv("ANYPS2_GS_CLUT_DEP", dep ? "" : "0");
    setEnv("ANYPS2_GS_TEXCACHE", texCache ? "1" : "");
    setEnv("ANYPS2_GS_TEXCACHE_VERIFY", texCache ? "1" : "");
    Gs g(nullptr);
    setEnv("ANYPS2_GS_CLUT_DEP", "");
    setEnv("ANYPS2_GS_TEXCACHE", "");
    setEnv("ANYPS2_GS_TEXCACHE_VERIFY", "");
    g.setTexVerifyAbort(false);
    if (sc == CdFullList) g.forceClutMeasure(true);
    cdScene(g, sc, rep);
    ClutDepRun out;
    videoOn(g);
    out.frame = g.display();
    out.stats = g.bandStats();
    out.mismatches = g.texVerifyMismatches();
    out.img = capture(g);
    return out;
}

constexpr const char* kCdNames[CdCount] = {"host_queued", "local_copy", "war",            "host_after", "host_open",
                                           "csm2",        "formats",    "no_draw_display", "render_to_clut", "full_list"};

struct ClutDepTotals {
    std::uint64_t lazy = 0, clutSubmits = 0, lost = 0;
};

// Compara `reps` repetições do cenário contra o caminho síncrono para `lanes` faixas.
ClutDepTotals checkClutDep(unsigned sc, unsigned reps, unsigned lanes, bool texCache) {
    ClutDepTotals t;
    for (unsigned rep = 0; rep < reps; ++rep) {
        const ClutDepRun ref = runClutDep(sc, rep, false, 1, true, false);
        if (rep == 0) {
            bool any = false;
            for (const std::uint8_t b : ref.img.vram) any = any || b != 0;
            CHECK(any);
        }
        const ClutDepRun got = runClutDep(sc, rep, true, lanes, true, texCache);
        const bool sameVram = got.img.vram == ref.img.vram;
        if (!sameVram) {
            std::fprintf(stderr, "  cenário %s, repetição %u, %u faixas: VRAM diferente\n", kCdNames[sc], rep, lanes);
        }
        CHECK(sameVram);
        CHECK(sameFrame(got.frame, ref.frame));
        CHECK_EQ(got.mismatches, 0u);
        CHECK_EQ(got.img.shaded, got.img.covered);
        t.lazy += got.stats.clutLazy;
        t.clutSubmits += got.stats.submit[BandStats::SubmitClut];
        t.lost += got.stats.clutLost;
    }
    return t;
}

}  // namespace

#define CLUT_DEP_TEST(NAME, SC, MINLAZY)                                \
    TEST_CASE(gs_bands, clut_dep_##NAME) {                              \
        for (const unsigned lanes : {1u, 2u, 4u}) {                     \
            const ClutDepTotals t = checkClutDep(SC, 50, lanes, false); \
            if (MINLAZY) CHECK(t.lazy > 0);                             \
        }                                                               \
    }

CLUT_DEP_TEST(host_queued, CdHostQueued, true)
CLUT_DEP_TEST(local_copy, CdLocalCopy, true)
CLUT_DEP_TEST(war_becomes_barrier, CdWar, true)
CLUT_DEP_TEST(host_direct_after_opens_queued, CdHostAfter, true)
CLUT_DEP_TEST(host_open_over_source, CdHostOpen, true)
CLUT_DEP_TEST(csm2_texclut, CdCsm2, true)
CLUT_DEP_TEST(formats_and_long_chain, CdFormats, true)
CLUT_DEP_TEST(no_draw_before_barrier_and_display, CdNoDraw, true)
CLUT_DEP_TEST(render_to_clut_stays_barrier, CdRenderToClut, false)

TEST_CASE(gs_bands, clut_dep_full_outstanding_list) {
    for (const unsigned lanes : {1u, 2u, 4u}) {
        const ClutDepTotals t = checkClutDep(CdFullList, 50, lanes, false);
        CHECK(t.lazy > 0);
        CHECK(t.lost > 0);  // a lista enche de fato (o caso do GT4)
    }
}

TEST_CASE(gs_bands, clut_dep_render_to_clut_keeps_barriers) {
    std::uint64_t submits = 0;
    for (unsigned rep = 0; rep < 10; ++rep) {
        submits += runClutDep(CdRenderToClut, rep, true, 4, true, false).stats.submit[BandStats::SubmitClut];
    }
    CHECK(submits > 0);
}

TEST_CASE(gs_bands, clut_dep_off_never_defers) {
    for (unsigned sc = 0; sc < CdCount; ++sc) {
        const ClutDepRun ref = runClutDep(sc, 0, false, 1, true, false);
        for (unsigned rep = 0; rep < 3; ++rep) {
            const ClutDepRun got = runClutDep(sc, 0, true, 4, false, false);
            CHECK_EQ(got.stats.clutLazy, 0u);
            CHECK(got.img.vram == ref.img.vram);
            CHECK(sameFrame(got.frame, ref.frame));
        }
    }
}

TEST_CASE(gs_bands, clut_dep_with_texture_cache_verify) {
    for (unsigned sc = 0; sc < CdCount; ++sc) {
        for (const unsigned lanes : {2u, 4u}) checkClutDep(sc, 12, lanes, true);
    }
}

TEST_CASE(gs_bands, clut_dep_does_not_hang) {
    // Cenários pesados com 1 e 8 faixas, com tempo limite: uma espera trocada por outra (ou um
    // mutex preso durante uma espera) travaria; o teste falha em vez de ficar parado.
    for (const unsigned lanes : {1u, 8u}) {
        auto done = std::make_shared<std::promise<bool>>();
        std::future<bool> f = done->get_future();
        std::thread th([done, lanes] {
            for (unsigned rep = 0; rep < 20; ++rep) {
                for (const unsigned sc : {static_cast<unsigned>(CdFormats), static_cast<unsigned>(CdFullList),
                                          static_cast<unsigned>(CdHostOpen), static_cast<unsigned>(CdWar)}) {
                    (void)runClutDep(sc, rep, true, lanes, true, false);
                }
            }
            done->set_value(true);
        });
        const bool ok = f.wait_for(std::chrono::seconds(120)) == std::future_status::ready;
        CHECK(ok);
        if (!ok) {
            th.detach();  // travado: o teste já falhou
            return;
        }
        th.join();
    }
}


// ---------------------------------------------------------------------------
// Alcance exato da leitura de textura (texReach0 em gs_draw.cpp): o desenho que lê uma
// parte do próprio FRAME/ZBUF e escreve em outra não vai mais sozinho; o que se cruza,
// vai. A imagem tem de ser a do caminho síncrono em qualquer caso.
// ---------------------------------------------------------------------------

namespace {

std::uint32_t floatBits(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    return u;
}

// Vértice STQ (FST=0): S/T no registrador ST, Q no RGBAQ.
void stVertex(Gs& g, unsigned x, unsigned y, float s, float t, float q) {
    g.writeRegister(RGBAQ, 0xFFull | (0xFFull << 8) | (0xFFull << 16) | (0x80ull << 24) | (std::uint64_t{floatBits(q)} << 32), 0);
    g.writeRegister(ST, std::uint64_t{floatBits(s)} | (std::uint64_t{floatBits(t)} << 32), 0);
    g.writeRegister(XYZ2, xyzPx(x, y, 0), 0);
}

// UV cru (1/16 de texel).
std::uint64_t uvRaw(unsigned u16, unsigned v16) {
    return std::uint64_t{u16} | (std::uint64_t{v16} << 16);
}

// Sprite com UV em texels (FST=1): dst [x0,x1)×[y0,y1), textura de (u0,v0) a (u1,v1).
void uvSprite(Gs& g, unsigned x0, unsigned y0, unsigned x1, unsigned y1, unsigned u0, unsigned v0, unsigned u1,
              unsigned v1, std::uint64_t z = 0) {
    g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
    vertex(g, x0, y0, rgbaq(255, 255, 255, 0x80), z, uv(u0, v0));
    vertex(g, x1, y1, rgbaq(255, 255, 255, 0x80), z, uv(u1, v1));
}

void stSprite(Gs& g, unsigned x0, unsigned y0, unsigned x1, unsigned y1, float s0, float t0, float s1, float t1) {
    g.writeRegister(PRIM, prim(6, false, true, false, false), 0);
    stVertex(g, x0, y0, s0, t0, 1.0f);
    stVertex(g, x1, y1, s1, t1, 1.0f);
}

enum ReachScenario : unsigned {
    RsSamePage,     // lê a metade de uma página e escreve a outra (nos dois sentidos)
    RsCross,        // lê e escreve áreas que se cruzam
    RsBilinOk,      // bilinear que fica a 1 texel da área escrita
    RsBilinEdge,    // bilinear que alcança o primeiro texel escrito
    RsRepeatWrap,   // REPEAT que dá a volta numa textura de 32x32 (alcance: a textura inteira)
    RsRepeatHit,    // REPEAT que dá a volta e cai na área escrita
    RsRegionClamp,  // REGION_CLAMP limita a leitura a 32 colunas
    RsRegionHit,    // REGION_CLAMP com a região dentro da área escrita
    RsStq,          // STQ com Q constante: sprite e triângulo
    RsStqVarQ,      // STQ com Q variável: textura inteira
    RsMip,          // mipmap (níveis seguintes fora do FRAME)
    RsMipOverlap,   // mipmap com um nível dentro do FRAME
    RsZbuf,         // o ZBUF é a textura; lê uma parte e escreve a outra
    RsZbufHit,      // o ZBUF é a textura e se cruzam
    RsCt16,         // CT16 no mesmo buffer
    RsTriFst,       // triângulos com UV
    RsMaskedFrame,  // FBMSK cobre tudo: o FRAME não é escrito; lê a própria área e escreve só o Z
    RsMaskedDate,   // o mesmo com DATE (o alfa de destino ainda é lido)
    RsCount
};

enum ReachExpect { ExNoSelfSaved, ExSelf, ExNoSelf };

ReachExpect expectOf(unsigned sc) {
    switch (sc) {
        case RsMaskedFrame: case RsMaskedDate:
            return ExNoSelf;
        case RsCross: case RsBilinEdge: case RsRepeatHit: case RsRegionHit: case RsStqVarQ: case RsMipOverlap:
        case RsZbufHit:
            return ExSelf;
        default:
            return ExNoSelfSaved;
    }
}

constexpr std::uint32_t kReachZ = 64 * 32;  // ZBP 64 (blocos 2048)

std::unique_ptr<Gs> makeReachGs(bool threaded, unsigned lanes, bool texCache) {
    setEnv("ANYPS2_GS_THREAD", threaded ? "1" : "0");
    setEnv("ANYPS2_GS_THREADS", std::to_string(lanes).c_str());
    setEnv("ANYPS2_GS_TEXCACHE", texCache ? "1" : "");
    setEnv("ANYPS2_GS_TEXCACHE_VERIFY", texCache ? "1" : "");
    auto g = std::make_unique<Gs>(nullptr);
    setEnv("ANYPS2_GS_TEXCACHE", "");
    setEnv("ANYPS2_GS_TEXCACHE_VERIFY", "");
    g->setTexVerifyAbort(false);
    g->writeRegister(SCISSOR_1, scissorReg(0, 127, 0, 127), 0);
    g->writeRegister(XYOFFSET_1, 0, 0);
    g->writeRegister(PRMODECONT, 1, 0);
    g->writeRegister(COLCLAMP, 1, 0);
    g->writeRegister(TEST_1, 0, 0);
    g->writeRegister(ZBUF_1, 1ull << 32, 0);
    return g;
}

struct ReachRun {
    Render img;
    BandStats stats;
    std::uint64_t mismatches = 0;
};

ReachRun finishReach(Gs& g) {
    ReachRun out;
    out.stats = g.bandStats();
    out.img = capture(g);
    out.mismatches = g.texVerifyMismatches();
    return out;
}

void reachPrefill(Gs& g, std::uint64_t seed) {
    Lcg r{seed};
    auto words = [&r](unsigned n) {
        std::vector<std::uint64_t> v;
        for (unsigned i = 0; i < n; ++i) v.push_back(r.word());
        return v;
    };
    hostToLocal(g, 0, 2, PSMCT32, 0, 0, 128, 128, words(128 * 128 / 2));
    hostToLocal(g, 0, 2, PSMCT16, 0, 0, 128, 128, words(128 * 128 / 4));
    hostToLocal(g, 0, 2, PSMCT32, 0, 0, 128, 128, words(128 * 128 / 2));  // de novo: CT32 por último
    hostToLocal(g, kReachZ, 2, PSMCT32, 0, 0, 128, 128, words(128 * 128 / 2));
    hostToLocal(g, 1024, 1, PSMCT32, 0, 0, 64, 64, words(64 * 64 / 2));
    hostToLocal(g, 1100, 1, PSMCT32, 0, 0, 32, 32, words(32 * 32 / 2));
}

constexpr unsigned kReachIters = 12;

// Os desenhos de cada cenário. Frame: FBP 0, 128x128. Metades de 32 px na mesma página
// (as páginas de 32 bits têm 64x32 px; as de 16 bits, 64x64).
void reachDraws(Gs& g, unsigned sc) {
    g.writeRegister(FRAME_1, frameReg(0, 2, sc == RsCt16 ? PSMCT16 : PSMCT32), 0);
    const std::uint64_t clamp = 1 | (1ull << 2);  // CLAMP/CLAMP
    g.writeRegister(CLAMP_1, clamp, 0);
    g.writeRegister(TEX1_1, 0, 0);
    switch (sc) {
        case RsSamePage: case RsCt16:
            g.writeRegister(TEX0_1, tex0(0, 2, sc == RsCt16 ? PSMCT16 : PSMCT32, 7, 7), 0);
            for (unsigned k = 0; k < kReachIters; ++k) {
                if (k & 1) uvSprite(g, 0, 0, 32, 64, 32, 0, 64, 64);
                else uvSprite(g, 32, 0, 64, 64, 0, 0, 32, 64);
            }
            break;
        case RsCross:
            g.writeRegister(TEX0_1, tex0(0, 2, PSMCT32, 7, 7), 0);
            for (unsigned k = 0; k < kReachIters; ++k) uvSprite(g, 0, 0, 64, 64, 16 + k, 0, 80 + k, 64);
            break;
        case RsBilinOk: case RsBilinEdge: {
            g.writeRegister(TEX0_1, tex0(0, 2, PSMCT32, 7, 7), 0);
            g.writeRegister(TEX1_1, 1ull << 5, 0);  // MMAG linear
            const unsigned len = sc == RsBilinOk ? 30 : 33;
            for (unsigned k = 0; k < kReachIters; ++k) {
                if (k & 1) uvSprite(g, 0, 0, 32, 64, 34, 0, 34 + len, 64);
                else uvSprite(g, 32, 0, 64, 64, 0, 0, len, 64);
            }
            break;
        }
        case RsRepeatWrap: case RsRepeatHit:
            g.writeRegister(TEX0_1, tex0(0, 2, PSMCT32, 5, 5), 0);
            g.writeRegister(CLAMP_1, 0, 0);  // REPEAT
            for (unsigned k = 0; k < kReachIters; ++k) {
                if (sc == RsRepeatWrap) uvSprite(g, 32, 0, 64, 32, 20, 4, 52, 36);
                else uvSprite(g, 0, 0, 40, 32, 20, 4, 52, 36);
            }
            break;
        case RsRegionClamp: case RsRegionHit: {
            g.writeRegister(TEX0_1, tex0(0, 2, PSMCT32, 7, 7), 0);
            const std::uint64_t maxu = 31;
            g.writeRegister(CLAMP_1, 2 | (1ull << 2) | (0ull << 4) | (maxu << 14), 0);  // REGION_CLAMP u, CLAMP v
            for (unsigned k = 0; k < kReachIters; ++k) {
                if (sc == RsRegionClamp) uvSprite(g, 32, 0, 128, 128, 0, 0, 200, 128);
                else uvSprite(g, 20, 0, 80, 128, 0, 0, 200, 128);
            }
            break;
        }
        case RsStq:
            g.writeRegister(TEX0_1, tex0(0, 2, PSMCT32, 7, 7), 0);
            for (unsigned k = 0; k < kReachIters; ++k) {
                stSprite(g, 32, 0, 64, 64, 0.0f, 0.0f, 0.25f, 0.5f);
                g.writeRegister(PRIM, prim(3, false, true, false, false), 0);
                stVertex(g, 40, 70, 0.0f, 0.0f, 1.0f);
                stVertex(g, 120, 72, 0.2f, 0.0f, 1.0f);
                stVertex(g, 80, 125, 0.1f, 0.4f, 1.0f);
            }
            break;
        case RsStqVarQ:
            g.writeRegister(TEX0_1, tex0(0, 2, PSMCT32, 7, 7), 0);
            for (unsigned k = 0; k < kReachIters; ++k) {
                g.writeRegister(PRIM, prim(3, false, true, false, false), 0);
                stVertex(g, 40, 70, 0.0f, 0.0f, 1.0f);
                stVertex(g, 120, 72, 0.2f, 0.0f, 2.0f);
                stVertex(g, 80, 125, 0.1f, 0.4f, 1.0f);
            }
            break;
        case RsMip: case RsMipOverlap: {
            g.writeRegister(TEX0_1, tex0(0, 2, PSMCT32, 7, 7), 0);
            // MXL = 2, LCM = 1, MMAG linear, MMIN linear-mipmap-linear, K = 0,75.
            g.writeRegister(TEX1_1, 1ull | (2ull << 2) | (1ull << 5) | (5ull << 6) | (12ull << 32), 0);
            const std::uint64_t l1 = sc == RsMip ? 1024 : 64;  // 64: páginas 2 e 3, as que o desenho escreve
            g.writeRegister(MIPTBP1_1, l1 | (1ull << 14) | (std::uint64_t{1100} << 20) | (1ull << 34), 0);
            for (unsigned k = 0; k < kReachIters; ++k) {
                if (k & 1) uvSprite(g, 0, 0, 32, 64, 34, 0, 64, 64);
                else uvSprite(g, 32, 0, 64, 64, 0, 0, 30, 64);
            }
            break;
        }
        case RsZbuf: case RsZbufHit:
            g.writeRegister(ZBUF_1, 64, 0);  // ZBP 64, Z32, com escrita
            g.writeRegister(TEST_1, (1ull << 16) | (1ull << 17), 0);  // ZTE, ZTST = ALWAYS
            g.writeRegister(TEX0_1, tex0(kReachZ, 2, PSMZ32, 7, 7), 0);
            for (unsigned k = 0; k < kReachIters; ++k) {
                if (sc == RsZbuf) uvSprite(g, 32, 0, 64, 64, 0, 0, 32, 64, 100 + k);
                else uvSprite(g, 0, 0, 64, 64, 16, 0, 80, 64, 100 + k);
            }
            break;
        case RsMaskedFrame: case RsMaskedDate:
            // Como o efeito do GT4: FRAME 0xF0 com FBMSK = 0xFFFFFFFF, textura no próprio FRAME e Z escrito.
            g.writeRegister(FRAME_1, frameReg(0, 2, PSMCT32) | (0xFFFFFFFFull << 32), 0);
            g.writeRegister(ZBUF_1, 64, 0);  // ZBP 64, Z32, com escrita
            g.writeRegister(TEST_1, (1ull << 16) | (1ull << 17) | (sc == RsMaskedDate ? (1ull << 14) : 0), 0);
            g.writeRegister(TEX0_1, tex0(0, 2, PSMCT32, 7, 7), 0);
            for (unsigned k = 0; k < kReachIters; ++k) uvSprite(g, 0, 0, 64, 64, 8 + k, 0, 72 + k, 64, 100 + k);
            break;
        case RsTriFst:
            g.writeRegister(TEX0_1, tex0(0, 2, PSMCT32, 7, 7), 0);
            for (unsigned k = 0; k < kReachIters; ++k) {
                g.writeRegister(PRIM, prim(3, true, true, false, true), 0);
                vertex(g, 40, 10 + k, rgbaq(255, 255, 255, 0x80), 0, uv(0, 0));
                vertex(g, 120, 12, rgbaq(255, 255, 255, 0x80), 0, uv(20, 0));
                vertex(g, 80, 100, rgbaq(255, 255, 255, 0x80), 0, uv(10, 20));
            }
            break;
        default:
            break;
    }
}

ReachRun runReach(unsigned sc, bool threaded, unsigned lanes, bool texCache) {
    auto g = makeReachGs(threaded, lanes, texCache);
    reachPrefill(*g, 0x5EED + sc);
    reachDraws(*g, sc);
    return finishReach(*g);
}

}  // namespace

TEST_CASE(gs_bands, reach_scenarios_match_sync_and_classify_self) {
    for (unsigned sc = 0; sc < RsCount; ++sc) {
        const ReachRun ref = runReach(sc, false, 1, false);
        bool any = false;
        for (std::uint8_t b : ref.img.vram) any = any || b != 0;
        CHECK(any);
        for (unsigned rep = 0; rep < 3; ++rep) {
            for (const unsigned lanes : {1u, 2u, 4u}) {
                const ReachRun got = runReach(sc, true, lanes, false);
                CHECK(got.img.vram == ref.img.vram);
                CHECK_EQ(got.img.shaded, got.img.covered);
                if (lanes < 2) continue;  // com uma faixa só todo desenho vai "nas faixas"
                if (expectOf(sc) == ExSelf) {
                    CHECK(got.stats.self[BandStats::SelfTexture] > 0u);
                } else if (expectOf(sc) == ExNoSelf) {
                    CHECK_EQ(got.stats.self[BandStats::SelfTexture], 0u);
                    CHECK_EQ(got.stats.self[BandStats::SelfFrameZ], 0u);
                } else {
                    CHECK_EQ(got.stats.self[BandStats::SelfTexture], 0u);
                    CHECK_EQ(got.stats.self[BandStats::SelfFrameZ], 0u);
                    CHECK(got.stats.reachSaved > 0u);
                }
            }
        }
    }
}

TEST_CASE(gs_bands, reach_scenarios_with_texture_cache_verify) {
    for (unsigned sc = 0; sc < RsCount; ++sc) {
        const ReachRun ref = runReach(sc, false, 1, false);
        for (unsigned rep = 0; rep < 2; ++rep) {
            for (const unsigned lanes : {1u, 2u, 4u}) {
                const ReachRun got = runReach(sc, true, lanes, true);
                CHECK(got.img.vram == ref.img.vram);
                CHECK_EQ(got.mismatches, 0u);
            }
        }
    }
}

namespace {

// Desenhos aleatórios que lêem o próprio buffer (mesma base, mesma largura e formato quase
// sempre), com todos os modos de wrap, filtros, FST/STQ e todos os primitivos: o alcance
// exato nunca pode declarar menos do que o desenho lê (o VERIFY confere cada texel).
ReachRun renderReachRandom(bool threaded, unsigned lanes, bool texCache, std::uint64_t seed, int draws) {
    auto gp = makeReachGs(threaded, lanes, texCache);
    Gs& g = *gp;
    reachPrefill(g, seed);
    Lcg r{seed};
    static constexpr unsigned kVerts[7] = {1, 2, 2, 3, 3, 3, 2};
    for (int n = 0; n < draws; ++n) {
        const bool ct16 = r.next(4) == 0;
        const std::uint32_t psm = ct16 ? PSMCT16 : PSMCT32;
        g.writeRegister(FRAME_1, frameReg(0, 2, psm), 0);
        const unsigned x0 = r.next(128), y0 = r.next(128);
        g.writeRegister(SCISSOR_1, scissorReg(x0, x0 + r.next(128 - x0), y0, y0 + r.next(128 - y0)), 0);
        const unsigned type = r.next(7);
        const bool fst = r.next(10) < 7;
        const bool abe = r.next(2) == 1;
        g.writeRegister(PRIM, prim(type, r.next(2) == 1, true, abe, fst), 0);
        // Textura no próprio buffer quase sempre; às vezes noutra base/largura/formato.
        const bool same = r.next(8) != 0;
        const std::uint64_t tbp = same ? 0 : 8 * r.next(5);
        const std::uint64_t tbw = same ? 2 : 1 + r.next(3);
        const std::uint32_t tpsm = same ? psm : (r.next(2) ? PSMCT32 : PSMCT16);
        const unsigned tw = 5 + r.next(3), th = 5 + r.next(3);
        g.writeRegister(TEX0_1, tex0(tbp, tbw, tpsm, tw, th), 0);
        const unsigned wms = r.next(4), wmt = r.next(4);
        const std::uint64_t minu = r.next(1u << 7), maxu = r.next(1u << 7), minv = r.next(1u << 7), maxv = r.next(1u << 7);
        g.writeRegister(CLAMP_1, wms | (std::uint64_t{wmt} << 2) | (minu << 4) | (maxu << 14) | (minv << 24) | (maxv << 34), 0);
        const std::uint64_t mmag = r.next(2), mmin = r.next(2);  // nearest ou linear, sem mipmap
        g.writeRegister(TEX1_1, (mmag << 5) | (mmin << 6), 0);
        const bool constQ = r.next(10) != 0;
        const float q = 0.5f + static_cast<float>(r.next(8)) / 4.0f;
        for (unsigned k = 0; k < kVerts[type]; ++k) {
            const unsigned vx = r.next(128), vy = r.next(128);
            if (fst) {
                vertex(g, vx, vy, rgbaq(r.next(256), r.next(256), r.next(256), 0x80 + r.next(128)), 0,
                       uvRaw(r.next(140 * 16), r.next(140 * 16)));
            } else {
                const float s = static_cast<float>(r.next(1000)) / 800.0f, t = static_cast<float>(r.next(1000)) / 800.0f;
                stVertex(g, vx, vy, s * q, t * q, constQ ? q : q * (1.0f + static_cast<float>(k) / 4.0f));
            }
        }
    }
    return finishReach(g);
}

}  // namespace

TEST_CASE(gs_bands, reach_random_draws_match_sync) {
    std::uint64_t saved = 0;
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        const ReachRun ref = renderReachRandom(false, 1, false, seed, 150);
        for (unsigned rep = 0; rep < 2; ++rep) {
            for (const unsigned lanes : {2u, 4u}) {
                const ReachRun got = renderReachRandom(true, lanes, false, seed, 150);
                CHECK(got.img.vram == ref.img.vram);
                CHECK_EQ(got.img.shaded, got.img.covered);
                saved += got.stats.reachSaved;
            }
        }
    }
    CHECK(saved > 0u);
}

TEST_CASE(gs_bands, reach_random_draws_with_texture_cache_verify) {
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        const ReachRun ref = renderReachRandom(false, 1, false, seed, 150);
        for (const unsigned lanes : {1u, 2u, 4u}) {
            const ReachRun got = renderReachRandom(true, lanes, true, seed, 150);
            CHECK(got.img.vram == ref.img.vram);
            CHECK_EQ(got.mismatches, 0u);  // inclui a conferência do alcance declarado
        }
    }
}
