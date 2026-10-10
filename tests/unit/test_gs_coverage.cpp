// Testes do worker do GS (gs_worker.cpp) e da contagem analítica de pixels
// (gs_coverage.cpp). A contagem que o tempo do GS usa tem de dar exatamente o
// número de pixels que o rasterizador escreve depois do SCISSOR e do SCANMSK;
// e a saída com o worker tem de ser idêntica à do caminho síncrono.

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/gs/gs_coverage.h"
#include "minitest.h"

#ifdef _WIN32
#include <stdlib.h>
#endif

using namespace anyps2::rt;
using namespace anyps2::rt::gs;

namespace {

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

constexpr std::uint64_t kTestZAlways = (1ull << 16) | (1ull << 17);  // ZTE=1, ZTST=ALWAYS

// Vértice já em 12.4 (x16, y16), com o Q padrão.
Vertex vtx(int x16, int y16) {
    Vertex v;
    v.x = x16;
    v.y = y16;
    return v;
}

// Framebuffer PSMCT32 64x64 em FBP=0, sem Z (ZMSK), scissor cobrindo tudo.
void setupFb(Gs& g) {
    g.writeRegister(FRAME_1, frameReg(0, 1, PSMCT32), 0);
    g.writeRegister(ZBUF_1, (std::uint64_t{1} << 32) | 8, 0);
    g.writeRegister(SCISSOR_1, scissorReg(0, 63, 0, 63), 0);
    g.writeRegister(TEST_1, kTestZAlways, 0);
    g.writeRegister(XYOFFSET_1, 0, 0);
    g.writeRegister(PRMODECONT, 1, 0);
}

std::uint32_t px(const Gs& g, unsigned x, unsigned y) {
    return g.vram().readPixel(PSMCT32, 0, 1, x, y);
}

void sprite(Gs& g, unsigned x0, unsigned y0, unsigned x1, unsigned y1, std::uint64_t color, bool abe = false) {
    g.writeRegister(PRIM, prim(6, false, false, abe), 0);
    g.writeRegister(RGBAQ, color, 0);
    g.writeRegister(XYZ2, xyzPx(x0, y0), 0);
    g.writeRegister(XYZ2, xyzPx(x1, y1), 0);
}

void hostToLocal(Gs& g, std::uint32_t dbp, std::uint32_t dbw, std::uint32_t psm, std::uint32_t x, std::uint32_t y,
                 std::uint32_t w, std::uint32_t h, const std::vector<std::uint64_t>& data) {
    g.writeRegister(BITBLTBUF, (std::uint64_t{dbp} << 32) | (std::uint64_t{dbw} << 48) | (std::uint64_t{psm} << 56), 0);
    g.writeRegister(TRXPOS, (std::uint64_t{x} << 32) | (std::uint64_t{y} << 48), 0);
    g.writeRegister(TRXREG, w | (std::uint64_t{h} << 32), 0);
    g.writeRegister(TRXDIR, 0, 0);
    for (std::uint64_t d : data) g.writeRegister(HWREG, d, 0);
}

// Gerador determinístico (sem depender da biblioteca padrão para repetir).
struct Lcg {
    std::uint64_t s;
    std::uint32_t next(std::uint32_t n) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>((s >> 33) % n);
    }
};

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// Uma cena com triângulos sombreados, texturas de cor, sprites com alfa, linhas,
// pontos, uma transferência HOST→LOCAL e uma cópia LOCAL→LOCAL; devolve o framebuffer.
std::vector<std::uint32_t> renderScene(bool threaded) {
    setEnv("ANYPS2_GS_THREAD", threaded ? "1" : "0");
    Gs g(nullptr);
    CHECK_EQ(g.threaded(), threaded);
    setupFb(g);
    g.writeRegister(ALPHA_1, 0 | (2u << 2) | (2u << 4) | (1u << 6) | (0x80ull << 32), 0);
    g.writeRegister(PRIM, prim(4, true, false, false), 0);
    g.writeRegister(RGBAQ, rgbaq(200, 0, 0, 0), 0);
    g.writeRegister(XYZ2, xyzPx(2, 2), 0);
    g.writeRegister(RGBAQ, rgbaq(0, 200, 0, 0), 0);
    g.writeRegister(XYZ2, xyzPx(40, 3), 0);
    g.writeRegister(RGBAQ, rgbaq(0, 0, 200, 0), 0);
    g.writeRegister(XYZ2, xyzPx(20, 50), 0);
    g.writeRegister(SCISSOR_1, scissorReg(4, 60, 1, 58), 0);
    g.writeRegister(SCANMSK, 2, 0);
    sprite(g, 10, 10, 50, 30, rgbaq(255, 128, 0, 0x60), true);
    g.writeRegister(SCANMSK, 0, 0);
    g.writeRegister(PRIM, prim(1, true, false, false), 0);
    g.writeRegister(RGBAQ, rgbaq(255, 255, 255, 0), 0);
    g.writeRegister(XYZ2, xyzPx(0, 63), 0);
    g.writeRegister(RGBAQ, rgbaq(9, 9, 9, 0), 0);
    g.writeRegister(XYZ2, xyzPx(63, 0), 0);
    g.writeRegister(PRIM, prim(0), 0);
    g.writeRegister(RGBAQ, rgbaq(1, 2, 3, 0), 0);
    g.writeRegister(XYZ2, xyzPx(33, 33), 0);
    g.writeRegister(SCISSOR_1, scissorReg(0, 63, 0, 63), 0);
    hostToLocal(g, 32, 1, PSMCT32, 0, 0, 4, 4, {0x2222222211111111ull, 0x4444444433333333ull, 0x6666666655555555ull,
                                               0x8888888877777777ull});
    g.writeRegister(BITBLTBUF, (32ull << 32) | (1ull << 48) | (std::uint64_t{PSMCT32} << 56) |
                                   (32ull) | (1ull << 16) | (std::uint64_t{PSMCT32} << 24),
                    0);
    g.writeRegister(TRXPOS, (10ull << 32) | (20ull << 48) | (0ull), 0);
    g.writeRegister(TRXREG, 4 | (4ull << 32), 0);
    g.writeRegister(TRXDIR, 2, 0);
    std::vector<std::uint32_t> out;
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 64; ++x) out.push_back(px(g, x, y));
    setEnv("ANYPS2_GS_THREAD", "");
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Contagem analítica (função pura)
// ---------------------------------------------------------------------------

TEST_CASE(gs_coverage, sprite_count_with_scissor_and_scanmsk) {
    // Sprite [10, 30) x [20, 40); janela x 15..25 (inclusivo), y livre.
    const Vertex a = vtx(10 * 16, 20 * 16);
    const Vertex b = vtx(30 * 16, 40 * 16);
    CHECK_EQ(coveredPixels(DrawWindow{15, 25, 0, 63, 0}, 6, a, b, a), 220u);  // 20 linhas × 11 colunas
    // SCANMSK=2 descarta as linhas ímpares: 10 das 20 linhas.
    CHECK_EQ(coveredPixels(DrawWindow{15, 25, 0, 63, 2}, 6, a, b, a), 110u);
    // SCANMSK=3 mantém só as ímpares (21..39): 10 linhas.
    CHECK_EQ(coveredPixels(DrawWindow{15, 25, 0, 63, 3}, 6, a, b, a), 110u);
    // Janela que não toca o sprite.
    CHECK_EQ(coveredPixels(DrawWindow{40, 63, 0, 63, 0}, 6, a, b, a), 0u);
}

TEST_CASE(gs_coverage, point_and_line_count) {
    const Vertex p = vtx(5 * 16, 6 * 16);
    CHECK_EQ(coveredPixels(DrawWindow{0, 63, 0, 63, 0}, 0, p, p, p), 1u);
    CHECK_EQ(coveredPixels(DrawWindow{0, 63, 0, 63, 2}, 0, p, p, p), 1u);  // y = 6 é par
    const Vertex q = vtx(5 * 16, 7 * 16);
    CHECK_EQ(coveredPixels(DrawWindow{0, 63, 0, 63, 2}, 0, q, q, q), 0u);  // y = 7 é ímpar
    // Linha horizontal de 4 pixels (passos = 4 → 4 amostras).
    const Vertex l0 = vtx(0, 0);
    const Vertex l1 = vtx(4 * 16, 0);
    CHECK_EQ(coveredPixels(DrawWindow{0, 63, 0, 63, 0}, 1, l0, l1, l1), 4u);
    CHECK_EQ(coveredPixels(DrawWindow{2, 63, 0, 63, 0}, 1, l0, l1, l1), 2u);
}

TEST_CASE(gs_coverage, triangle_count_top_left_rule) {
    // (0,0), (8,0), (0,8): a aresta de cima e a da esquerda entram; a hipotenusa
    // não. Pixels com x >= 0, y >= 0 e x + y <= 7: 8·9/2 = 36.
    const Vertex a = vtx(0, 0);
    const Vertex b = vtx(8 * 16, 0);
    const Vertex c = vtx(0, 8 * 16);
    CHECK_EQ(coveredPixels(DrawWindow{0, 63, 0, 63, 0}, 3, a, b, c), 36u);
    // Ordem invertida (área negativa) cobre o mesmo conjunto.
    CHECK_EQ(coveredPixels(DrawWindow{0, 63, 0, 63, 0}, 3, a, c, b), 36u);
    // Triângulo degenerado não escreve nada.
    CHECK_EQ(coveredPixels(DrawWindow{0, 63, 0, 63, 0}, 3, a, b, b), 0u);
}

// ---------------------------------------------------------------------------
// Contagem analítica contra o rasterizador (shadePixel)
// ---------------------------------------------------------------------------

TEST_CASE(gs_coverage, sprite_counts_match_shading) {
    Gs g(nullptr);
    setupFb(g);
    g.writeRegister(SCISSOR_1, scissorReg(15, 25, 0, 63), 0);
    g.writeRegister(SCANMSK, 2, 0);
    sprite(g, 10, 20, 30, 40, rgbaq(1, 2, 3, 0x80));
    CHECK_EQ(g.pixelsCovered(), 110u);
    CHECK_EQ(g.pixelsShaded(), 110u);
    g.writeRegister(SCANMSK, 0, 0);
    sprite(g, 10, 20, 30, 40, rgbaq(1, 2, 3, 0x80));
    CHECK_EQ(g.pixelsCovered(), 330u);
    CHECK_EQ(g.pixelsShaded(), 330u);
}

TEST_CASE(gs_coverage, random_draws_match_shading) {
    // Desenhos de todos os tipos com janela e SCANMSK sorteados (inclusive
    // nas bordas): a conta analítica tem de andar junto com o que é escrito.
    Gs g(nullptr);
    setupFb(g);
    Lcg r{0x5EED};
    static constexpr unsigned kVerts[7] = {1, 2, 2, 3, 3, 3, 2};
    for (int n = 0; n < 500; ++n) {
        const auto x0 = r.next(64), y0 = r.next(64);
        const auto x1 = x0 + r.next(64 - x0), y1 = y0 + r.next(64 - y0);
        g.writeRegister(SCISSOR_1, scissorReg(x0, x1, y0, y1), 0);
        g.writeRegister(SCANMSK, r.next(4), 0);
        const unsigned type = r.next(7);
        g.writeRegister(PRIM, prim(type, r.next(2) == 1), 0);
        g.writeRegister(RGBAQ, rgbaq(r.next(256), r.next(256), r.next(256), r.next(256)), 0);
        for (unsigned k = 0; k < kVerts[type]; ++k) g.writeRegister(XYZ2, xyzPx(r.next(80), r.next(80)), 0);
        CHECK_EQ(g.pixelsShaded(), g.pixelsCovered());
    }
}

// ---------------------------------------------------------------------------
// Drenagem obrigatória do EE
// ---------------------------------------------------------------------------

TEST_CASE(gs_coverage, local_to_host_right_after_draws) {
    Gs g(nullptr);
    setupFb(g);
    sprite(g, 0, 0, 4, 1, rgbaq(255, 0, 0, 0x80));
    // LOCAL→HOST de 4×1 logo depois do desenho: tem de ver o sprite.
    g.writeRegister(BITBLTBUF, 0 | (1ull << 16) | (std::uint64_t{PSMCT32} << 24), 0);
    g.writeRegister(TRXPOS, 0, 0);
    g.writeRegister(TRXREG, 4 | (1ull << 32), 0);
    g.writeRegister(TRXDIR, 1, 0);
    CHECK_EQ(g.downloadRemaining(), 1u);
    std::uint32_t got[4] = {};
    g.readDownload(reinterpret_cast<std::uint8_t*>(got), 1);
    CHECK_EQ(got[0], 0x800000FFu);
    CHECK_EQ(got[3], 0x800000FFu);
}

TEST_CASE(gs_coverage, display_after_draws) {
    Gs g(nullptr);
    setupFb(g);
    sprite(g, 0, 0, 64, 32, rgbaq(10, 20, 30, 0x80));
    g.writePrivileged(0x12000000, 1, 0);                              // PMODE: EN1
    g.writePrivileged(0x12000070, 0 | (1ull << 9) | (0ull << 15), 0);  // DISPFB1
    g.writePrivileged(0x12000080, (63ull << 32) | (31ull << 44), 0);   // DISPLAY1: 64x32
    const Frame f = g.display();
    CHECK_EQ(f.width, 64u);
    CHECK_EQ(f.height, 32u);
    CHECK_EQ(f.pixels[0], 0xFF1E140Au);
}

TEST_CASE(gs_coverage, reset_with_queued_work) {
    Gs g(nullptr);
    setupFb(g);
    sprite(g, 0, 0, 2, 1, rgbaq(255, 0, 0, 0x80));
    // Reset com desenho na fila: o desenho termina (como no caminho síncrono).
    g.writePrivileged(0x12001000, 0x200, 0);
    CHECK_EQ(px(g, 0, 0), 0x800000FFu);
    // Transferência HOST→LOCAL 2×2 em curso: a primeira linha entra, o reset
    // cancela o resto. O primeiro pixel de cada palavra fica nos bits baixos.
    g.writeRegister(BITBLTBUF, (32ull << 32) | (1ull << 48) | (std::uint64_t{PSMCT32} << 56), 0);
    g.writeRegister(TRXPOS, 0, 0);
    g.writeRegister(TRXREG, 2 | (2ull << 32), 0);
    g.writeRegister(TRXDIR, 0, 0);
    g.writeRegister(HWREG, 0x1111111122222222ull, 0);
    g.writePrivileged(0x12001000, 0x200, 0);
    g.writeRegister(HWREG, 0x3333333344444444ull, 0);
    CHECK_EQ(g.vram().readPixel(PSMCT32, 32, 1, 0, 0), 0x22222222u);
    CHECK_EQ(g.vram().readPixel(PSMCT32, 32, 1, 1, 0), 0x11111111u);
    CHECK_EQ(g.vram().readPixel(PSMCT32, 32, 1, 0, 1), 0u);
}

TEST_CASE(gs_coverage, unsupported_throws_at_the_same_write) {
    Gs g(nullptr);
    setupFb(g);
    g.writeRegister(FRAME_1, frameReg(0, 1, PSMT8), 0);  // formato que o desenho não aceita
    g.writeRegister(PRIM, prim(6), 0);
    g.writeRegister(RGBAQ, rgbaq(1, 0, 0, 0), 0);
    g.writeRegister(XYZ2, xyzPx(0, 0), 0);
    CHECK_THROWS_WITH(g.writeRegister(XYZ2, xyzPx(4, 4), 0), "desenho em FRAME");
    // Nada foi enfileirado para o worker com esse desenho.
    CHECK_EQ(g.pixelsCovered(), 0u);
}

TEST_CASE(gs_coverage, clean_shutdown_with_pending_work) {
    {
        Gs g(nullptr);
        setupFb(g);
        for (unsigned i = 0; i < 200; ++i) sprite(g, 0, 0, 64, 64, rgbaq(i, 0, 0, 0x80), true);
        // Sai do escopo sem drenar: o destrutor espera o worker.
    }
    Gs g(nullptr);
    CHECK_EQ(g.pixelsShaded(), 0u);
}

// ---------------------------------------------------------------------------
// Modo síncrono (ANYPS2_GS_THREAD=0)
// ---------------------------------------------------------------------------

TEST_CASE(gs_coverage, threaded_and_synchronous_images_are_identical) {
    const std::vector<std::uint32_t> threaded = renderScene(true);
    const std::vector<std::uint32_t> sync = renderScene(false);
    CHECK_EQ(threaded.size(), sync.size());
    CHECK(threaded == sync);
    // A cena tem de ter desenhado algo, senão a comparação não prova nada.
    bool any = false;
    for (std::uint32_t p : sync) any = any || p != 0;
    CHECK(any);
}
