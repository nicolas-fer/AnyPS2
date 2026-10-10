// Testes das faixas do GS (gs_bands.cpp e gs_worker.cpp com várias threads).
// Com faixas, a imagem tem de ser idêntica à do caminho síncrono
// (ANYPS2_GS_THREAD=0), byte a byte na VRAM inteira, para qualquer número de
// faixas; e a contagem analítica tem de continuar batendo com o rasterizador.

#include <cstdint>
#include <cstdlib>
#include <cstring>
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

TEST_CASE(gs_bands, clashes_rules) {
    VramAccess w{};
    w.write = true;
    w.surface = 0;
    w.base = 0;
    w.bw = 2;
    w.psm = PSMCT32;
    w.span = VramSpan{0, 9};
    VramAccess same = w;
    same.span = VramSpan{5, 12};
    VramAccess other = w;
    other.surface = 1;  // ZBUF sobre o mesmo intervalo: outro mapeamento
    VramAccess read{};
    read.span = VramSpan{3, 4};
    read.surface = 2;
    VramAccess far = read;
    far.span = VramSpan{40, 50};

    CHECK(!clashes(w, same));       // mesmo mapeamento: a faixa de cada pixel é a mesma
    CHECK(clashes(w, other));       // mapeamentos diferentes sobrepostos
    CHECK(clashes(w, read));        // escrita e leitura sobrepostas
    CHECK(clashes(read, w));
    CHECK(!clashes(read, far));     // sem sobreposição
    VramAccess read2 = read;
    CHECK(!clashes(read, read2));   // duas leituras nunca colidem
}

TEST_CASE(gs_bands, page_span_contains_every_touched_page) {
    // Varredura: as páginas de cada pixel do retângulo têm de estar no intervalo.
    const std::uint32_t psms[] = {PSMCT32, PSMCT16, PSMT8, PSMT4, PSMZ32};
    for (std::uint32_t psm : psms) {
        for (std::uint32_t bw : {0u, 1u, 2u, 3u}) {
            for (std::uint32_t base : {0u, 1u, 32u}) {
                const std::uint32_t bp = base * 32 + 7;  // começa no meio de uma página
                const unsigned rowLo = 3, rowHi = 70, cols = 150;
                const VramSpan s = pageSpan(psm, base, bw, rowLo, rowHi, cols);
                for (unsigned y = rowLo; y <= rowHi; ++y) {
                    for (unsigned x = 0; x < cols; x += 3) {
                        std::uint32_t addr = 0;
                        if (psm == PSMT8) addr = Vram::byteAddress8(bp, bw, x, y);
                        else if (psm == PSMT4) addr = Vram::nibbleAddress4(bp, bw, x, y) / 2;
                        else if (psm == PSMCT16) addr = Vram::byteAddress16(bp, bw, x, y, psm);
                        else addr = Vram::byteAddress32(bp, bw, x, y, psm == PSMZ32);
                        const std::uint32_t page = addr / 8192;
                        CHECK(page >= s.first && page <= s.last);
                    }
                }
            }
        }
    }
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

TEST_CASE(gs_bands, shaded_matches_covered_with_lanes) {
    // Sprites e triângulos em todas as faixas: a conta por faixa soma a conta analítica.
    for (unsigned lanes : {2u, 4u}) {
        const Render r = renderRandom(true, lanes, 0x5EED + lanes, 150);
        CHECK_EQ(r.shaded, r.covered);
    }
}
