// Testes do GS em software, do GIF, do VIF e do DMAC. Os valores esperados
// são calculados à mão a partir do manual do GS (regras de cobertura,
// fórmulas de blending, layout da memória local), não copiados da saída do
// próprio rasterizador.

#include <cstring>
#include <set>
#include <vector>

#include "anyps2/runtime/dmac.h"
#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/timing.h"
#include "anyps2/runtime/vif.h"
#include "minitest.h"

using namespace anyps2::rt;
using namespace anyps2::rt::gs;

namespace {

std::uint64_t frameReg(std::uint64_t fbp, std::uint64_t fbw, std::uint64_t psm, std::uint64_t mask = 0) {
    return fbp | (fbw << 16) | (psm << 24) | (mask << 32);
}
std::uint64_t scissorReg(std::uint64_t x0, std::uint64_t x1, std::uint64_t y0, std::uint64_t y1) {
    return x0 | (x1 << 16) | (y0 << 32) | (y1 << 48);
}
// Coordenadas em pixels (inteiros ou com fração em 1/16).
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

// Framebuffer PSMCT32 64x64 em FBP=0, sem Z (ZMSK), scissor cobrindo tudo.
void setupFb(Gs& g) {
    g.writeRegister(FRAME_1, frameReg(0, 1, PSMCT32), 0);
    g.writeRegister(ZBUF_1, (std::uint64_t{1} << 32) | 8, 0);  // ZMSK, ZBP=8 (fora do FB)
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

}  // namespace

// ---------------------------------------------------------------------------
// Memória local
// ---------------------------------------------------------------------------

TEST_CASE(gs, vram_layout_psmct32) {
    // Bloco 8x8, colunas de 8x2; blocos na ordem 0,1,4,5,16,... na página.
    CHECK_EQ(Vram::byteAddress32(0, 1, 0, 0), 0u);
    CHECK_EQ(Vram::byteAddress32(0, 1, 1, 0), 4u);    // palavra 1
    CHECK_EQ(Vram::byteAddress32(0, 1, 0, 1), 8u);    // palavra 2
    CHECK_EQ(Vram::byteAddress32(0, 1, 2, 0), 16u);   // palavra 4
    CHECK_EQ(Vram::byteAddress32(0, 1, 0, 2), 64u);   // coluna 1
    CHECK_EQ(Vram::byteAddress32(0, 1, 8, 0), 256u);  // bloco 1
    CHECK_EQ(Vram::byteAddress32(0, 1, 16, 0), 1024u);  // bloco 4
    CHECK_EQ(Vram::byteAddress32(0, 1, 0, 8), 512u);    // bloco 2
    CHECK_EQ(Vram::byteAddress32(0, 1, 32, 0), 16u * 256);  // bloco 16
    // Páginas: 64x32 pixels; FBW=1 → a próxima página fica em y=32.
    CHECK_EQ(Vram::byteAddress32(0, 1, 0, 32), 8192u);
    CHECK_EQ(Vram::byteAddress32(0, 10, 64, 0), 8192u);
    CHECK_EQ(Vram::byteAddress32(0, 10, 0, 32), 10u * 8192);
    // BP em blocos
    CHECK_EQ(Vram::byteAddress32(3, 1, 0, 0), 3u * 256);
    // Z32: metades da página trocadas (bloco 0 → 24)
    CHECK_EQ(Vram::byteAddress32(0, 1, 0, 0, true), 24u * 256);
    CHECK_EQ(Vram::byteAddress32(0, 1, 32, 0, true), 8u * 256);
}

TEST_CASE(gs, vram_layout_small_formats) {
    // PSMCT16: bloco 16x8, ordem 0,2,8,10 na primeira linha de blocos
    CHECK_EQ(Vram::byteAddress16(0, 1, 16, 0, PSMCT16), 2u * 256);
    CHECK_EQ(Vram::byteAddress16(0, 1, 0, 8, PSMCT16), 1u * 256);
    CHECK_EQ(Vram::byteAddress16(0, 1, 8, 0, PSMCT16), 2u);  // meia-palavra 1
    CHECK_EQ(Vram::byteAddress16(0, 1, 1, 0, PSMCT16), 4u);  // meia-palavra 2
    CHECK_EQ(Vram::byteAddress16(0, 1, 32, 0, PSMCT16S), 16u * 256);
    // PSMT8: (0,2) é o byte 33 do bloco; (4,2) é o byte 1 (metades trocadas)
    CHECK_EQ(Vram::byteAddress8(0, 2, 0, 2), 33u);
    CHECK_EQ(Vram::byteAddress8(0, 2, 4, 2), 1u);
    CHECK_EQ(Vram::byteAddress8(0, 2, 0, 4), 96u);  // coluna 1 começa trocada
    // PSMT4: (0,2) é o nibble 65
    CHECK_EQ(Vram::nibbleAddress4(0, 2, 0, 2), 65u);
    CHECK_EQ(Vram::nibbleAddress4(0, 2, 8, 0), 2u);
    // Cada formato é uma bijeção dentro de uma página.
    std::set<std::uint32_t> s32, s16, s8, s4;
    for (unsigned y = 0; y < 32; ++y)
        for (unsigned x = 0; x < 64; ++x) s32.insert(Vram::byteAddress32(0, 1, x, y));
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 64; ++x) s16.insert(Vram::byteAddress16(0, 1, x, y, PSMCT16));
    for (unsigned y = 0; y < 64; ++y)
        for (unsigned x = 0; x < 128; ++x) s8.insert(Vram::byteAddress8(0, 2, x, y));
    for (unsigned y = 0; y < 128; ++y)
        for (unsigned x = 0; x < 128; ++x) s4.insert(Vram::nibbleAddress4(0, 2, x, y));
    CHECK_EQ(s32.size(), 2048u);
    CHECK_EQ(*s32.rbegin(), 8188u);
    CHECK_EQ(s16.size(), 4096u);
    CHECK_EQ(s8.size(), 8192u);
    CHECK_EQ(s4.size(), 16384u);
}

TEST_CASE(gs, host_to_local_transfers) {
    Gs g(nullptr);
    // PSMCT32 2x2 em (5,7)
    hostToLocal(g, 0, 1, PSMCT32, 5, 7, 2, 2, {0x2222222211111111ull, 0x4444444433333333ull});
    CHECK_EQ(px(g, 5, 7), 0x11111111u);
    CHECK_EQ(px(g, 6, 7), 0x22222222u);
    CHECK_EQ(px(g, 5, 8), 0x33333333u);
    CHECK_EQ(px(g, 6, 8), 0x44444444u);
    // PSMCT24: 8 pixels em 3 palavras de 64 bits; byte alto preservado
    g.vram().writePixel(PSMCT32, 0, 1, 0, 0, 0xAA000000u);
    std::vector<std::uint8_t> bytes;
    for (unsigned i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(i));
        bytes.push_back(static_cast<std::uint8_t>(0x10 + i));
        bytes.push_back(static_cast<std::uint8_t>(0x20 + i));
    }
    std::vector<std::uint64_t> words(3);
    std::memcpy(words.data(), bytes.data(), 24);
    hostToLocal(g, 0, 1, PSMCT24, 0, 0, 8, 1, words);
    CHECK_EQ(px(g, 0, 0), 0xAA201000u);
    CHECK_EQ(px(g, 7, 0), 0x00271707u);
    // PSMT4: nibble baixo primeiro
    hostToLocal(g, 64, 2, PSMT4, 0, 0, 16, 1, {0xFEDCBA9876543210ull});
    for (unsigned x = 0; x < 16; ++x) CHECK_EQ(g.vram().readPixel(PSMT4, 64, 2, x, 0), x);
    // Dados extras depois do fim são ignorados
    g.writeRegister(HWREG, 0xFFFFFFFFFFFFFFFFull, 0);
    CHECK_EQ(g.vram().readPixel(PSMT4, 64, 2, 0, 1), 0u);
}

TEST_CASE(gs, local_to_local) {
    Gs g(nullptr);
    hostToLocal(g, 0, 1, PSMCT32, 0, 0, 2, 1, {0xBBBBBBBBAAAAAAAAull});
    g.writeRegister(BITBLTBUF, 0 | (1ull << 16) | (std::uint64_t{PSMCT32} << 24) | (32ull << 32) | (1ull << 48), 0);
    g.writeRegister(TRXPOS, 0 | (3ull << 32) | (4ull << 48), 0);
    g.writeRegister(TRXREG, 2 | (1ull << 32), 0);
    g.writeRegister(TRXDIR, 2, 0);
    CHECK_EQ(g.vram().readPixel(PSMCT32, 32, 1, 3, 4), 0xAAAAAAAAu);
    CHECK_EQ(g.vram().readPixel(PSMCT32, 32, 1, 4, 4), 0xBBBBBBBBu);
}

// LOCAL→HOST: o retângulo sai empacotado como numa HOST→LOCAL do mesmo PSM
// (completado até quadword), pelo DMA do VIF1 com DIR=0 ou pelo VIF1_FIFO,
// com BUSDIR = 1.
TEST_CASE(gs, local_to_host_download) {
    const ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    Runtime rt(info, RuntimeOptions{});
    Gs& g = rt.gs();
    Memory& m = rt.memory();
    for (unsigned y = 0; y < 2; ++y) {
        for (unsigned x = 0; x < 3; ++x) g.vram().writePixel(PSMCT32, 0, 1, 10 + x, 20 + y, 0x01000000u * (y * 3 + x + 1) | 0xABCDu);
    }
    // Origem: BP 0, BW 1, PSMCT32, (10,20), 3×2; sobram 8 bytes no último quadword.
    g.writeRegister(BITBLTBUF, 0 | (1ull << 16) | (std::uint64_t{PSMCT32} << 24), 0);
    g.writeRegister(TRXPOS, 10 | (20ull << 16), 0);
    g.writeRegister(TRXREG, 3 | (2ull << 32), 0);
    g.writeRegister(TRXDIR, 1, 0);
    CHECK_EQ(g.downloadRemaining(), 2u);
    // Sem BUSDIR o DMA é erro do programa.
    m.write<std::uint32_t>(0x10009010, 0x00300000, 0);
    m.write<std::uint32_t>(0x10009020, 2, 0);
    CHECK_THROWS_WITH(m.write<std::uint32_t>(0x10009000, 0x100, 0), "BUSDIR");
    g.writePrivileged(0x12001040, 1, 0);
    m.write<std::uint32_t>(0x10003C00, 1u << 23, 0);  // VIF1_STAT.FDR
    CHECK_EQ(m.read<std::uint32_t>(0x10003C00, 0) & (1u << 23), 1u << 23);
    m.write<std::uint32_t>(0x10009000, 0x100, 0);  // DIR=0, normal, STR
    CHECK_EQ(m.read<std::uint32_t>(0x10009000, 0) & 0x100, 0u);
    CHECK_EQ(m.read<std::uint32_t>(0x10009010, 0), 0x00300020u);
    for (unsigned i = 0; i < 6; ++i) CHECK_EQ(m.read<std::uint32_t>(0x00300000 + 4 * i, 0), 0x01000000u * (i + 1) | 0xABCDu);
    CHECK_EQ(m.read<std::uint64_t>(0x00300018, 0), 0ull);  // enchimento
    CHECK_EQ(g.downloadRemaining(), 0u);
    // PSMCT16 pelo VIF1_FIFO: 8 pixels de 16 bits num quadword, o primeiro nos bits baixos.
    for (unsigned x = 0; x < 8; ++x) g.vram().writePixel(PSMCT16, 64, 1, x, 0, 0x1000u + x);
    g.writeRegister(BITBLTBUF, 64 | (1ull << 16) | (std::uint64_t{PSMCT16} << 24), 0);
    g.writeRegister(TRXPOS, 0, 0);
    g.writeRegister(TRXREG, 8 | (1ull << 32), 0);
    g.writeRegister(TRXDIR, 1, 0);
    Reg128 q{};
    m.read128(0x10005000, q, 0);
    for (unsigned x = 0; x < 8; ++x) CHECK_EQ(q.uh[x], 0x1000u + x);
    g.writePrivileged(0x12001040, 0, 0);
    CHECK_THROWS_WITH(m.read128(0x10005000, q, 0), "BUSDIR");
}

// ---------------------------------------------------------------------------
// Rasterização
// ---------------------------------------------------------------------------

TEST_CASE(gs, sprite_coverage_and_scissor) {
    Gs g(nullptr);
    setupFb(g);
    sprite(g, 10, 20, 30, 40, rgbaq(255, 0, 0, 0x80));
    CHECK_EQ(px(g, 10, 20), 0x800000FFu);
    CHECK_EQ(px(g, 29, 39), 0x800000FFu);
    CHECK_EQ(px(g, 30, 39), 0u);  // borda direita exclusiva
    CHECK_EQ(px(g, 29, 40), 0u);  // borda inferior exclusiva
    CHECK_EQ(px(g, 9, 20), 0u);
    // Scissor
    g.writeRegister(SCISSOR_1, scissorReg(0, 4, 0, 63), 0);
    sprite(g, 0, 0, 10, 2, rgbaq(0, 255, 0, 0));
    CHECK_EQ(px(g, 4, 0), 0x0000FF00u);
    CHECK_EQ(px(g, 5, 0), 0u);
    // XYOFFSET: (100,100) com offset 90 → pixel (10,10)
    g.writeRegister(SCISSOR_1, scissorReg(0, 63, 0, 63), 0);
    g.writeRegister(XYOFFSET_1, (90ull * 16) | ((90ull * 16) << 32), 0);
    sprite(g, 100, 100, 101, 101, rgbaq(1, 2, 3, 4));
    CHECK_EQ(px(g, 10, 10), 0x04030201u);
}

TEST_CASE(gs, triangle_top_left_rule_no_double_hits) {
    // Dois triângulos que formam um quadrado: com blending aditivo
    // (Cs−0)·FIX/128 + Cd e cor 1, cada pixel coberto recebe exatamente 1.
    Gs g(nullptr);
    setupFb(g);
    // A=Cs(0) B=0(2) C=FIX(2) D=Cd(1), FIX=0x80
    g.writeRegister(ALPHA_1, 0 | (2u << 2) | (2u << 4) | (1u << 6) | (0x80ull << 32), 0);
    g.writeRegister(PRIM, prim(3, false, false, true), 0);
    g.writeRegister(RGBAQ, rgbaq(1, 0, 0, 0x80), 0);
    // Coordenadas fracionárias para exercitar a regra
    g.writeRegister(XYZ2, xyz(3 * 16 + 5, 2 * 16 + 3), 0);
    g.writeRegister(XYZ2, xyz(40 * 16 + 9, 2 * 16 + 3), 0);
    g.writeRegister(XYZ2, xyz(40 * 16 + 9, 33 * 16 + 11), 0);
    g.writeRegister(PRIM, prim(3, false, false, true), 0);
    g.writeRegister(XYZ2, xyz(3 * 16 + 5, 2 * 16 + 3), 0);
    g.writeRegister(XYZ2, xyz(40 * 16 + 9, 33 * 16 + 11), 0);
    g.writeRegister(XYZ2, xyz(3 * 16 + 5, 33 * 16 + 11), 0);
    unsigned covered = 0, doubles = 0;
    for (unsigned y = 0; y < 64; ++y) {
        for (unsigned x = 0; x < 64; ++x) {
            const std::uint32_t r = px(g, x, y) & 0xFF;
            if (r == 1) ++covered;
            if (r > 1) ++doubles;
        }
    }
    CHECK_EQ(doubles, 0u);
    // Pixels com centro em [3.3125, 40.5625) x [2.1875, 33.6875): x 4..40, y 3..33
    CHECK_EQ(covered, 37u * 31u);
    CHECK_EQ(px(g, 4, 3) & 0xFF, 1u);
    CHECK_EQ(px(g, 3, 3) & 0xFF, 0u);
    CHECK_EQ(px(g, 40, 33) & 0xFF, 1u);
    CHECK_EQ(px(g, 41, 33) & 0xFF, 0u);
}

TEST_CASE(gs, gouraud_and_flat) {
    Gs g(nullptr);
    setupFb(g);
    // Triângulo com vértice vermelho em (0,0); pixel (0,0) recebe a cor dele.
    g.writeRegister(PRIM, prim(3, true), 0);
    g.writeRegister(RGBAQ, rgbaq(200, 0, 0, 0), 0);
    g.writeRegister(XYZ2, xyzPx(0, 0), 0);
    g.writeRegister(RGBAQ, rgbaq(0, 200, 0, 0), 0);
    g.writeRegister(XYZ2, xyzPx(32, 0), 0);
    g.writeRegister(RGBAQ, rgbaq(0, 0, 200, 0), 0);
    g.writeRegister(XYZ2, xyzPx(0, 32), 0);
    CHECK_EQ(px(g, 0, 0), 0x000000C8u);
    // Meio da aresta superior (16,0): metade vermelho, metade verde
    CHECK_EQ(px(g, 16, 0), 0x00006464u);
    // Flat: cor do último vértice
    g.writeRegister(PRIM, prim(3, false), 0);
    g.writeRegister(RGBAQ, rgbaq(1, 0, 0, 0), 0);
    g.writeRegister(XYZ2, xyzPx(40, 40), 0);
    g.writeRegister(RGBAQ, rgbaq(2, 0, 0, 0), 0);
    g.writeRegister(XYZ2, xyzPx(60, 40), 0);
    g.writeRegister(RGBAQ, rgbaq(3, 0, 0, 0), 0);
    g.writeRegister(XYZ2, xyzPx(40, 60), 0);
    CHECK_EQ(px(g, 41, 41), 0x00000003u);
}

TEST_CASE(gs, strips_and_fans) {
    Gs g(nullptr);
    setupFb(g);
    g.writeRegister(ALPHA_1, 0 | (2u << 2) | (2u << 4) | (1u << 6) | (0x80ull << 32), 0);
    // Strip de 4 vértices = 2 triângulos formando o quadrado [0,16)²
    g.writeRegister(PRIM, prim(4, false, false, true), 0);
    g.writeRegister(RGBAQ, rgbaq(1, 0, 0, 0), 0);
    g.writeRegister(XYZ2, xyzPx(0, 0), 0);
    g.writeRegister(XYZ2, xyzPx(16, 0), 0);
    g.writeRegister(XYZ2, xyzPx(0, 16), 0);
    g.writeRegister(XYZ2, xyzPx(16, 16), 0);
    unsigned n = 0;
    for (unsigned y = 0; y < 20; ++y)
        for (unsigned x = 0; x < 20; ++x) n += px(g, x, y) & 0xFF;
    CHECK_EQ(n, 256u);
    // XYZ3 não desenha, mas o vértice entra na fila
    g.writeRegister(PRIM, prim(6), 0);
    g.writeRegister(RGBAQ, rgbaq(9, 9, 9, 9), 0);
    g.writeRegister(XYZ3, xyzPx(30, 30), 0);
    g.writeRegister(XYZ2, xyzPx(32, 32), 0);
    CHECK_EQ(px(g, 31, 31), 0x09090909u);
    g.writeRegister(XYZ3, xyzPx(40, 40), 0);
    CHECK_EQ(px(g, 40, 40), 0u);
}

TEST_CASE(gs, depth_test) {
    Gs g(nullptr);
    setupFb(g);
    g.writeRegister(ZBUF_1, 8 | (std::uint64_t{PSMZ32 & 0xF} << 24), 0);  // Z32 em ZBP=8, escreve
    g.writeRegister(TEST_1, (1ull << 16) | (2ull << 17), 0);             // GEQUAL
    g.writeRegister(PRIM, prim(6), 0);
    g.writeRegister(RGBAQ, rgbaq(10, 0, 0, 0), 0);
    g.writeRegister(XYZ2, xyzPx(0, 0, 1000), 0);
    g.writeRegister(XYZ2, xyzPx(8, 8, 1000), 0);
    // Mais longe (Z menor): reprovado
    g.writeRegister(RGBAQ, rgbaq(20, 0, 0, 0), 0);
    g.writeRegister(XYZ2, xyzPx(0, 0, 999), 0);
    g.writeRegister(XYZ2, xyzPx(8, 8, 999), 0);
    CHECK_EQ(px(g, 1, 1) & 0xFF, 10u);
    // Igual: aprovado (GEQUAL)
    g.writeRegister(RGBAQ, rgbaq(30, 0, 0, 0), 0);
    g.writeRegister(XYZ2, xyzPx(0, 0, 1000), 0);
    g.writeRegister(XYZ2, xyzPx(8, 8, 1000), 0);
    CHECK_EQ(px(g, 1, 1) & 0xFF, 30u);
    CHECK_EQ(g.vram().readPixel(PSMZ32, 8 * 32, 1, 1, 1), 1000u);
    // Z24 satura em 0xFFFFFF
    g.writeRegister(ZBUF_1, 8 | (std::uint64_t{PSMZ24 & 0xF} << 24), 0);
    g.writeRegister(TEST_1, kTestZAlways, 0);
    g.writeRegister(XYZ2, xyzPx(0, 0, 0x12345678), 0);
    g.writeRegister(XYZ2, xyzPx(1, 1, 0x12345678), 0);
    CHECK_EQ(g.vram().readPixel(PSMZ24, 8 * 32, 1, 0, 0), 0xFFFFFFu);
}

TEST_CASE(gs, z_test_off_does_not_write_z) {
    // ZTE=0 desliga também a escrita de Z. Jogos (o GT4, nos desenhos 2D) deixam
    // o ZBUF no mesmo endereço do FRAME com ZMSK=0: escrever Z apagaria a cor.
    Gs g(nullptr);
    setupFb(g);
    g.writeRegister(ZBUF_1, 0 | (std::uint64_t{PSMZ32 & 0xF} << 24), 0);  // ZBP=0 = FRAME, ZMSK=0
    g.writeRegister(TEST_1, 0, 0);                                      // ZTE=0
    g.writeRegister(PRIM, prim(6), 0);
    g.writeRegister(RGBAQ, rgbaq(10, 20, 30, 0x80), 0);
    g.writeRegister(XYZ2, xyzPx(0, 0, 0x1234), 0);
    g.writeRegister(XYZ2, xyzPx(8, 8, 0x1234), 0);
    // O Z32 arruma os blocos de outro jeito que o CT32: o Z de (1, 1) cairia em
    // outro pixel da página. Nenhum pixel da página pode ter o valor do Z.
    unsigned zs = 0;
    for (unsigned y = 0; y < 32; ++y) {
        for (unsigned x = 0; x < 64; ++x) zs += px(g, x, y) == 0x1234u;
    }
    CHECK_EQ(zs, 0u);
    CHECK_EQ(px(g, 1, 1), 0x801E140Au);
    // Com ZTE=1 (ALWAYS) o Z volta a ser escrito.
    g.writeRegister(TEST_1, kTestZAlways, 0);
    g.writeRegister(XYZ2, xyzPx(0, 0, 0x1234), 0);
    g.writeRegister(XYZ2, xyzPx(8, 8, 0x1234), 0);
    CHECK_EQ(g.vram().readPixel(PSMZ32, 0, 1, 1, 1), 0x1234u);
}

TEST_CASE(gs, alpha_blending_and_tests) {
    Gs g(nullptr);
    setupFb(g);
    sprite(g, 0, 0, 8, 8, rgbaq(200, 100, 0, 0x80));
    // (Cs − Cd)·As/128 + Cd com As = 0x40 (0,5): média
    g.writeRegister(ALPHA_1, 0 | (1u << 2) | (0u << 4) | (1u << 6), 0);
    sprite(g, 0, 0, 8, 8, rgbaq(0, 200, 100, 0x40), true);
    CHECK_EQ(px(g, 0, 0), 0x40329664u);  // R=(0−200)/2+200=100, G=150, B=50, A=As
    // COLCLAMP=0: resultado dá a volta em 8 bits; (Cs+Cd) com C=FIX=0x80
    g.writeRegister(ALPHA_1, 0 | (2u << 2) | (2u << 4) | (1u << 6) | (0x80ull << 32), 0);
    g.writeRegister(COLCLAMP, 0, 0);
    sprite(g, 0, 0, 1, 1, rgbaq(200, 0, 0, 0x80), true);
    CHECK_EQ(px(g, 0, 0) & 0xFF, (100u + 200u) & 0xFF);
    g.writeRegister(COLCLAMP, 1, 0);
    // Teste de alfa: GREATER que 0x50, AFAIL=KEEP
    g.writeRegister(TEST_1, kTestZAlways | 1 | (6u << 1) | (0x50u << 4), 0);
    sprite(g, 10, 10, 11, 11, rgbaq(1, 1, 1, 0x50));
    CHECK_EQ(px(g, 10, 10), 0u);
    sprite(g, 10, 10, 11, 11, rgbaq(1, 1, 1, 0x51));
    CHECK_EQ(px(g, 10, 10), 0x51010101u);
    // AFAIL=RGB_ONLY: escreve RGB, preserva alfa
    g.writeRegister(TEST_1, kTestZAlways | 1 | (0u << 1) | (3u << 12), 0);  // NEVER
    sprite(g, 10, 10, 11, 11, rgbaq(7, 7, 7, 0x22));
    CHECK_EQ(px(g, 10, 10), 0x51070707u);
    // FBMSK protege bits
    g.writeRegister(TEST_1, kTestZAlways, 0);
    g.writeRegister(FRAME_1, frameReg(0, 1, PSMCT32, 0xFFFF00FFu), 0);
    sprite(g, 10, 10, 11, 11, rgbaq(0xAA, 0xBB, 0xCC, 0xDD));
    CHECK_EQ(px(g, 10, 10), 0x5107BB07u);
}

TEST_CASE(gs, psmct16_framebuffer_and_dither) {
    Gs g(nullptr);
    setupFb(g);
    g.writeRegister(FRAME_1, frameReg(0, 1, PSMCT16), 0);
    sprite(g, 0, 0, 2, 2, rgbaq(0xFF, 0x80, 0x08, 0x80));
    // R=31, G=16, B=1, A=1
    CHECK_EQ(g.vram().readPixel(PSMCT16, 0, 1, 0, 0), 0x861Fu);
    // Dither com DIMX[0][0] = −4: 0x08 − 4 = 4 → B = 0
    g.writeRegister(DTHE, 1, 0);
    g.writeRegister(DIMX, 4, 0);  // entrada (0,0) = 4 → −4
    sprite(g, 0, 0, 1, 1, rgbaq(0x08, 0x08, 0x08, 0));
    CHECK_EQ(g.vram().readPixel(PSMCT16, 0, 1, 0, 0), 0u);
}

TEST_CASE(gs, textures_clut_and_filters) {
    Gs g(nullptr);
    setupFb(g);
    // Textura PSMT8 4x4 em TBP=256 (TBW=2) com índices 0..15
    std::vector<std::uint64_t> tex(2);
    std::uint8_t idx[16];
    for (unsigned i = 0; i < 16; ++i) idx[i] = static_cast<std::uint8_t>(i == 5 ? 8 : i);
    std::memcpy(tex.data(), idx, 16);
    hostToLocal(g, 256, 2, PSMT8, 0, 0, 4, 4, tex);
    // CLUT PSMCT32 CSM1 em CBP=512: entrada i fica na posição i com bits 3 e 4
    // trocados num retângulo 16x16. Entrada 8 → (0,1).
    std::vector<std::uint64_t> clut(128, 0);
    for (unsigned p = 0; p < 256; ++p) {
        const unsigned e = (p & ~0x18u) | ((p & 0x08u) << 1) | ((p & 0x10u) >> 1);
        const std::uint32_t color = 0xFF000000u | (e * 0x010101u);
        reinterpret_cast<std::uint32_t*>(clut.data())[p] = color;
    }
    hostToLocal(g, 512, 1, PSMCT32, 0, 0, 16, 16, clut);
    CHECK_EQ(g.vram().readPixel(PSMCT32, 512, 1, 0, 1), 0xFF080808u);
    // TEX0: TBP=256 TBW=2 PSMT8 TW=TH=2 TCC=1 DECAL CBP=512 CPSM=32 CSM1 CLD=1
    const std::uint64_t tex0 = 256 | (2ull << 14) | (std::uint64_t{PSMT8} << 20) | (2ull << 26) | (2ull << 30) |
                               (1ull << 34) | (1ull << 35) | (512ull << 37) | (1ull << 61);
    g.writeRegister(TEX0_1, tex0, 0);
    g.writeRegister(TEX1_1, 0, 0);
    g.writeRegister(CLAMP_1, 0, 0);
    // Sprite 4x4 com UV (0,0)–(4,4) em texels (10.4) e meio texel de offset
    g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
    g.writeRegister(RGBAQ, rgbaq(128, 128, 128, 128), 0);
    g.writeRegister(UV, 8 | (8u << 16), 0);
    g.writeRegister(XYZ2, xyzPx(20, 20), 0);
    g.writeRegister(UV, (4 * 16 + 8) | ((4u * 16 + 8) << 16), 0);
    g.writeRegister(XYZ2, xyzPx(24, 24), 0);
    CHECK_EQ(px(g, 20, 20), 0xFF000000u);
    CHECK_EQ(px(g, 21, 20), 0xFF010101u);
    CHECK_EQ(px(g, 21, 21), 0xFF080808u);  // índice 8 via CLUT trocada
    CHECK_EQ(px(g, 23, 23), 0xFF0F0F0Fu);
    // MODULATE com cor 0x40 (= 0,5): metade
    g.writeRegister(TEX0_1, tex0 & ~(3ull << 35), 0);
    g.writeRegister(RGBAQ, rgbaq(0x40, 0x40, 0x40, 0x80), 0);
    g.writeRegister(UV, 8 | (8u << 16), 0);
    g.writeRegister(XYZ2, xyzPx(20, 20), 0);
    g.writeRegister(UV, (4 * 16 + 8) | ((4u * 16 + 8) << 16), 0);
    g.writeRegister(XYZ2, xyzPx(24, 24), 0);
    CHECK_EQ(px(g, 23, 23), 0xFF070707u);  // 15*0x40>>7 = 7; A = 0xFF*0x80>>7 = 0xFF
    // CLAMP: texel fora de [0,4) com REPEAT dá a volta
    g.writeRegister(TEX0_1, tex0, 0);
    g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
    g.writeRegister(UV, (4 * 16 + 8) | (8u << 16), 0);  // u=4,5 → 0
    g.writeRegister(XYZ2, xyzPx(30, 30), 0);
    g.writeRegister(UV, (5 * 16 + 8) | ((1u * 16 + 8) << 16), 0);
    g.writeRegister(XYZ2, xyzPx(31, 31), 0);
    CHECK_EQ(px(g, 30, 30), 0xFF000000u);
    g.writeRegister(CLAMP_1, 1 | (1u << 2), 0);  // CLAMP: u=4 → 3
    g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
    g.writeRegister(UV, (4 * 16 + 8) | (8u << 16), 0);
    g.writeRegister(XYZ2, xyzPx(30, 30), 0);
    g.writeRegister(UV, (5 * 16 + 8) | ((1u * 16 + 8) << 16), 0);
    g.writeRegister(XYZ2, xyzPx(31, 31), 0);
    CHECK_EQ(px(g, 30, 30), 0xFF030303u);
}

TEST_CASE(gs, clut_csa_offset_8bit) {
    // O CSA desloca a CLUT de 16 em 16 entradas também nas texturas de 8 bits
    // (com volta em 256 no CPSM=32): a carga e a amostragem têm de concordar.
    Gs g(nullptr);
    setupFb(g);
    std::vector<std::uint64_t> tex(2);
    std::uint8_t idx[16];
    for (unsigned i = 0; i < 16; ++i) idx[i] = static_cast<std::uint8_t>(i);
    std::memcpy(tex.data(), idx, 16);
    hostToLocal(g, 256, 2, PSMT8, 0, 0, 4, 4, tex);
    std::vector<std::uint64_t> clut(128, 0);
    for (unsigned p = 0; p < 256; ++p) {
        const unsigned e = (p & ~0x18u) | ((p & 0x08u) << 1) | ((p & 0x10u) >> 1);
        reinterpret_cast<std::uint32_t*>(clut.data())[p] = 0xFF000000u | (e * 0x010101u);
    }
    hostToLocal(g, 512, 1, PSMCT32, 0, 0, 16, 16, clut);
    const std::uint64_t base = 256 | (2ull << 14) | (std::uint64_t{PSMT8} << 20) | (2ull << 26) | (2ull << 30) |
                               (1ull << 34) | (1ull << 35) | (512ull << 37);
    const auto drawQuad = [&](std::uint64_t tex0) {
        g.writeRegister(TEX0_1, tex0, 0);
        g.writeRegister(TEX1_1, 0, 0);
        g.writeRegister(CLAMP_1, 0, 0);
        g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
        g.writeRegister(RGBAQ, rgbaq(128, 128, 128, 128), 0);
        g.writeRegister(UV, 8 | (8u << 16), 0);
        g.writeRegister(XYZ2, xyzPx(20, 20), 0);
        g.writeRegister(UV, (4 * 16 + 8) | ((4u * 16 + 8) << 16), 0);
        g.writeRegister(XYZ2, xyzPx(24, 24), 0);
    };
    // Carga e amostragem com CSA=2: o índice i dá a entrada i da CLUT na VRAM.
    drawQuad(base | (2ull << 56) | (1ull << 61));
    CHECK_EQ(px(g, 20, 20), 0xFF000000u);
    CHECK_EQ(px(g, 21, 21), 0xFF050505u);
    CHECK_EQ(px(g, 23, 23), 0xFF0F0F0Fu);
    // Carga com CSA=0 e amostragem com CSA=2 sem recarregar (CLD=0): o índice
    // i lê a entrada 32+i.
    drawQuad(base | (1ull << 61));
    drawQuad(base | (2ull << 56));
    CHECK_EQ(px(g, 20, 20), 0xFF202020u);
    CHECK_EQ(px(g, 23, 23), 0xFF2F2F2Fu);
}

TEST_CASE(gs, texture_psmct16_texa_and_bilinear) {
    Gs g(nullptr);
    setupFb(g);
    // 2x1 PSMCT16: preto sem bit A, branco com bit A
    hostToLocal(g, 256, 1, PSMCT16, 0, 0, 4, 1, {0x00000000FFFF0000ull});
    g.writeRegister(TEXA, 0x11 | (1ull << 15) | (0x99ull << 32), 0);  // TA0, AEM, TA1
    const std::uint64_t tex0 = 256 | (1ull << 14) | (std::uint64_t{PSMCT16} << 20) | (2ull << 26) | (0ull << 30) |
                               (1ull << 34) | (1ull << 35);
    g.writeRegister(TEX0_1, tex0, 0);
    g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
    g.writeRegister(RGBAQ, rgbaq(128, 128, 128, 128), 0);
    g.writeRegister(UV, 8 | (8u << 16), 0);
    g.writeRegister(XYZ2, xyzPx(0, 0), 0);
    g.writeRegister(UV, (2 * 16 + 8) | (16u << 16), 0);
    g.writeRegister(XYZ2, xyzPx(2, 1), 0);
    CHECK_EQ(px(g, 0, 0), 0x00000000u);  // preto + AEM → alfa 0
    CHECK_EQ(px(g, 1, 0), 0x99F8F8F8u);  // bit A → TA1
    // Bilinear (MMAG=1) entre os dois texels, no meio: média
    g.writeRegister(TEX1_1, 1u << 5, 0);
    g.writeRegister(PRIM, prim(6, false, true, false, true), 0);
    g.writeRegister(UV, 16 | (8u << 16), 0);  // u = 1.0: meio entre os centros 0,5 e 1,5
    g.writeRegister(XYZ2, xyzPx(10, 0), 0);
    g.writeRegister(UV, 32 | (24u << 16), 0);
    g.writeRegister(XYZ2, xyzPx(11, 1), 0);
    CHECK_EQ(px(g, 10, 0) & 0xFFFFFF, 0x7C7C7Cu);
}

TEST_CASE(gs, perspective_stq) {
    Gs g(nullptr);
    setupFb(g);
    // Textura 2x1 PSMCT32: vermelho | verde
    hostToLocal(g, 256, 1, PSMCT32, 0, 0, 2, 1, {0xFF00FF00FF0000FFull});
    const std::uint64_t tex0 = 256 | (1ull << 14) | (std::uint64_t{PSMCT32} << 20) | (1ull << 26) | (0ull << 30) |
                               (1ull << 35);
    g.writeRegister(TEX0_1, tex0, 0);
    // Sprite com STQ: S de 0 a 1 com Q=1 → metade esquerda vermelha
    auto st = [](float s, float t) {
        std::uint32_t a, b;
        std::memcpy(&a, &s, 4);
        std::memcpy(&b, &t, 4);
        return a | (std::uint64_t{b} << 32);
    };
    g.writeRegister(PRIM, prim(6, false, true), 0);
    g.writeRegister(RGBAQ, rgbaq(128, 128, 128, 128), 0);
    g.writeRegister(ST, st(0.0f, 0.0f), 0);
    g.writeRegister(XYZ2, xyzPx(0, 0), 0);
    g.writeRegister(ST, st(1.0f, 1.0f), 0);
    g.writeRegister(XYZ2, xyzPx(16, 1), 0);
    CHECK_EQ(px(g, 0, 0) & 0xFFFFFF, 0x0000FFu);
    CHECK_EQ(px(g, 7, 0) & 0xFFFFFF, 0x0000FFu);
    CHECK_EQ(px(g, 8, 0) & 0xFFFFFF, 0x00FF00u);
    CHECK_EQ(px(g, 15, 0) & 0xFFFFFF, 0x00FF00u);
}

TEST_CASE(gs, signal_finish_and_csr) {
    Gs g(nullptr);
    CHECK_EQ(g.csr() & 3, 0ull);
    g.writeRegister(FINISH, 0, 0);
    CHECK_EQ(g.csr() & 2, 2ull);
    g.writePrivileged(0x12001000, 2, 0);
    CHECK_EQ(g.csr() & 2, 0ull);
    g.writeRegister(SIGNAL, 0x1234 | (0xFFFFull << 32), 0);
    CHECK_EQ(g.csr() & 1, 1ull);
    CHECK_EQ(g.readPrivileged(0x12001080, 0) & 0xFFFFFFFF, 0x1234ull);
    g.vblankStart();
    CHECK((g.csr() & 8) != 0);
    CHECK_EQ(g.csr() >> 16 & 0xFFFF, 0x551Bull);
    CHECK_THROWS_WITH(g.writeRegister(0x7F, 0, 0x100), "registrador geral inexistente");
}

TEST_CASE(gs, display_output) {
    Gs g(nullptr);
    setupFb(g);
    sprite(g, 0, 0, 64, 64, rgbaq(10, 20, 30, 0x80));
    sprite(g, 2, 3, 3, 4, rgbaq(255, 255, 255, 0x80));
    CHECK(!g.displayEnabled());
    g.writePrivileged(0x12000000, 1, 0);                                     // PMODE: EN1
    g.writePrivileged(0x12000070, 0 | (1ull << 9) | (0ull << 15), 0);        // DISPFB1: FBP 0, FBW 1, PSMCT32
    g.writePrivileged(0x12000080, (63ull << 32) | (31ull << 44), 0);         // DISPLAY1: 64x32
    const Frame f = g.display();
    CHECK_EQ(f.width, 64u);
    CHECK_EQ(f.height, 32u);
    CHECK_EQ(f.pixels[0], 0xFF1E140Au);
    CHECK_EQ(f.pixels[3 * 64 + 2], 0xFFFFFFFFu);
    // MAGH=1 (2x): metade da largura
    g.writePrivileged(0x12000080, (63ull << 32) | (31ull << 44) | (1ull << 23), 0);
    CHECK_EQ(g.display().width, 32u);
    // SMODE2 INT+FFMD (modo campo): o framebuffer tem meia altura; cada linha
    // aparece duas vezes (linhas 6 e 7 da saída = linha 3 do framebuffer).
    g.writePrivileged(0x12000080, (63ull << 32) | (31ull << 44), 0);
    g.writePrivileged(0x12000020, 3, 0);
    const Frame field = g.display();
    CHECK_EQ(field.height, 32u);
    CHECK_EQ(field.pixels[6 * 64 + 2], 0xFFFFFFFFu);
    CHECK_EQ(field.pixels[7 * 64 + 2], 0xFFFFFFFFu);
    CHECK_EQ(field.pixels[3 * 64 + 2], 0xFF1E140Au);
}

// ---------------------------------------------------------------------------
// GIF
// ---------------------------------------------------------------------------

namespace {
struct Packet {
    std::vector<std::uint64_t> q;
    void tag(unsigned nloop, bool eop, unsigned flg, unsigned nreg, std::uint64_t regs, bool pre = false,
             std::uint64_t primv = 0) {
        q.push_back(nloop | (std::uint64_t{eop} << 15) | (std::uint64_t{pre} << 46) | (primv << 47) |
                    (std::uint64_t{flg} << 58) | (std::uint64_t{nreg} << 60));
        q.push_back(regs);
    }
    void ad(std::uint8_t reg, std::uint64_t value) {
        q.push_back(value);
        q.push_back(reg);
    }
    void qw(std::uint64_t lo, std::uint64_t hi) {
        q.push_back(lo);
        q.push_back(hi);
    }
    const std::uint8_t* data() const { return reinterpret_cast<const std::uint8_t*>(q.data()); }
    std::size_t qwords() const { return q.size() / 2; }
};
}  // namespace

TEST_CASE(gs, gif_packed_reglist_image) {
    Gs g(nullptr);
    Gif gif(nullptr, g);
    Packet p;
    // A+D: configura o framebuffer
    p.tag(6, false, 0, 1, 0xE);
    p.ad(FRAME_1, frameReg(0, 1, PSMCT32));
    p.ad(ZBUF_1, (1ull << 32) | 8);
    p.ad(SCISSOR_1, scissorReg(0, 63, 0, 63));
    p.ad(TEST_1, kTestZAlways);
    p.ad(XYOFFSET_1, 0);
    p.ad(PRMODECONT, 1);
    // PACKED com PRE (sprite) e registradores RGBAQ, XYZ2, XYZ2
    p.tag(1, false, 0, 3, 0x551, true, 6);
    p.qw(0x0000002200000011ull, 0x0000004400000033ull);  // RGBA = 11 22 33 44
    p.qw(16 | (16ull << 32), 0);  // X | Y<<32 (pixel 1,1)
    p.qw((5ull * 16) | ((5ull * 16) << 32), 0);
    gif.transfer(3, p.data(), p.qwords(), 0);
    CHECK_EQ(px(g, 1, 1), 0x44332211u);
    CHECK_EQ(px(g, 4, 4), 0x44332211u);
    CHECK_EQ(px(g, 5, 5), 0u);
    // REGLIST: PRIM, RGBAQ, XYZ2, XYZ2 (NREG=4, NLOOP=1)
    Packet r;
    r.tag(1, false, 1, 4, 0x5510);
    r.qw(prim(6), rgbaq(1, 2, 3, 4));
    r.qw(xyzPx(10, 10), xyzPx(12, 12));
    gif.transfer(3, r.data(), r.qwords(), 0);
    CHECK_EQ(px(g, 11, 11), 0x04030201u);
    // REGLIST ímpar: NREG=1 NLOOP=3 → 3 registradores, último meio-qword descartado
    Packet r2;
    r2.tag(3, false, 1, 1, 0x1);  // RGBAQ ×3
    r2.qw(rgbaq(9, 0, 0, 0), rgbaq(8, 0, 0, 0));
    r2.qw(rgbaq(7, 0, 0, 0), 0xDEAD);
    r2.tag(1, true, 1, 3, 0x550);
    r2.qw(prim(6), xyzPx(20, 20));
    r2.qw(xyzPx(21, 21), 0);
    gif.transfer(3, r2.data(), r2.qwords(), 0);
    CHECK_EQ(px(g, 20, 20), 0x00000007u);
    // IMAGE: BITBLT via A+D e depois os dados
    Packet im;
    im.tag(4, false, 0, 1, 0xE);
    im.ad(BITBLTBUF, (0ull << 32) | (1ull << 48) | (std::uint64_t{PSMCT32} << 56));
    im.ad(TRXPOS, (30ull << 32) | (30ull << 48));
    im.ad(TRXREG, 4 | (1ull << 32));
    im.ad(TRXDIR, 0);
    im.tag(1, true, 2, 0, 0);
    im.qw(0x0000000200000001ull, 0x0000000400000003ull);
    gif.transfer(3, im.data(), im.qwords(), 0);
    CHECK_EQ(px(g, 30, 30), 1u);
    CHECK_EQ(px(g, 33, 30), 4u);
    // Descritor reservado
    Packet bad;
    bad.tag(1, true, 0, 1, 0xB);
    bad.qw(0, 0);
    CHECK_THROWS_WITH(gif.transfer(3, bad.data(), bad.qwords(), 0x200), "reservado");
}

TEST_CASE(gs, gif_packed_stq_and_xyzf) {
    Gs g(nullptr);
    Gif gif(nullptr, g);
    Packet p;
    // ST seguido de RGBAQ: o Q do ST vai para o RGBAQ
    p.tag(1, true, 0, 3, 0x412);
    const float s = 0.25f, t = 0.5f, q = 2.0f;
    std::uint32_t si, ti, qi;
    std::memcpy(&si, &s, 4);
    std::memcpy(&ti, &t, 4);
    std::memcpy(&qi, &q, 4);
    p.qw(si | (std::uint64_t{ti} << 32), qi);
    p.qw(0x10 | (0x20ull << 32), 0x30 | (0x40ull << 32));
    // XYZF2 com ADC=1 → XYZF3 (sem desenho)
    p.qw(16 | (32ull << 32), (0x123456ull << 4) | (0xABull << 36) | (1ull << 47));
    gif.transfer(3, p.data(), p.qwords(), 0);
    CHECK_EQ(g.reg(ST), si | (std::uint64_t{ti} << 32));
    CHECK_EQ(g.reg(RGBAQ), 0x40302010ull | (std::uint64_t{qi} << 32));
    CHECK_EQ(g.reg(XYZF3), 16ull | (32ull << 16) | (0x123456ull << 32) | (0xABull << 56));
    CHECK_EQ(g.reg(XYZF2), 0ull);
}

// ---------------------------------------------------------------------------
// VIF
// ---------------------------------------------------------------------------

TEST_CASE(gs, vif_unpack_and_direct) {
    Gs g(nullptr);
    Gif gif(nullptr, g);
    VuMemory vu;
    Vif vif(nullptr, 1, vu, &gif);
    auto run = [&](const std::vector<std::uint32_t>& words) {
        vif.transfer(reinterpret_cast<const std::uint8_t*>(words.data()), words.size() * 4, 0);
    };
    auto vu1 = [&](unsigned qw, unsigned c) {
        std::uint32_t v;
        std::memcpy(&v, vu.data1.get() + qw * 16 + c * 4, 4);
        return v;
    };
    // CYCLE nunca escrito (CL=WL=0): escrita normal, não preenchimento
    run({0x6C020008u, 1, 2, 3, 4, 5, 6, 7, 8});
    CHECK(vif.idle());
    CHECK_EQ(vu1(0x08, 0), 1u);
    CHECK_EQ(vu1(0x09, 3), 8u);
    // STCYCL 1,1; UNPACK V4-32 de 2 vetores em 0x10
    run({0x01000101u, 0x6C020010u, 1, 2, 3, 4, 5, 6, 7, 8});
    CHECK_EQ(vu1(0x10, 0), 1u);
    CHECK_EQ(vu1(0x10, 3), 4u);
    CHECK_EQ(vu1(0x11, 3), 8u);
    CHECK(vif.idle());
    // V2-16 com sinal: 0xFFFF → −1; z,w repetem x,y
    run({0x65010020u, 0x0002FFFFu});
    CHECK_EQ(vu1(0x20, 0), 0xFFFFFFFFu);
    CHECK_EQ(vu1(0x20, 1), 2u);
    CHECK_EQ(vu1(0x20, 2), 0xFFFFFFFFu);
    // USN=1 (bit 14): sem sinal
    run({0x65014021u, 0x0002FFFFu});
    CHECK_EQ(vu1(0x21, 0), 0xFFFFu);
    // S-8 replicado, 3 vetores em 1 palavra (padding)
    run({0x62030030u, 0x00030201u});
    CHECK_EQ(vu1(0x30, 0), 1u);
    CHECK_EQ(vu1(0x31, 2), 2u);
    CHECK_EQ(vu1(0x32, 3), 3u);
    // S-8 com sinal: 0xFF → −1; com USN, 255
    run({0x62010034u, 0x000000FFu, 0x62014035u, 0x000000FFu});
    CHECK_EQ(vu1(0x34, 0), 0xFFFFFFFFu);
    CHECK_EQ(vu1(0x35, 0), 0xFFu);
    // V4-5: 0x8000 | B=31 | G=1 | R=2
    run({0x6F010040u, (1u << 15) | (31u << 10) | (1u << 5) | 2u});
    CHECK_EQ(vu1(0x40, 0), 16u);
    CHECK_EQ(vu1(0x40, 1), 8u);
    CHECK_EQ(vu1(0x40, 2), 248u);
    CHECK_EQ(vu1(0x40, 3), 128u);
    // STROW + STMOD offset + máscara: x de dado, y da linha, z de coluna, w protegido
    std::memset(vu.data1.get() + 0x50 * 16, 0xEE, 16);
    run({0x30000000u, 100, 200, 300, 400,   // STROW
         0x31000000u, 7, 8, 9, 10,          // STCOL
         0x20000000u, (3u << 6) | (2u << 4) | (1u << 2) | 0u,  // STMASK
         0x05000001u,                       // STMOD offset
         0x7C010050u, 5, 6, 7, 8});         // UNPACK V4-32 com máscara (bit 4)
    CHECK_EQ(vu1(0x50, 0), 105u);  // dado + linha
    CHECK_EQ(vu1(0x50, 1), 200u);  // linha
    CHECK_EQ(vu1(0x50, 2), 7u);    // coluna 0
    CHECK_EQ(vu1(0x50, 3), 0xEEEEEEEEu);
    run({0x05000000u});
    // Escrita com salto: CL=2 WL=1 → vetores em 0x60 e 0x62
    run({0x01000102u, 0x6C020060u, 1, 1, 1, 1, 2, 2, 2, 2});
    CHECK_EQ(vu1(0x60, 0), 1u);
    CHECK_EQ(vu1(0x61, 0), 0u);
    CHECK_EQ(vu1(0x62, 0), 2u);
    // Escrita de preenchimento, CL=0 WL=4 (GT4): NUM=4 vetores só da máscara
    // (todos ROW), sem ler nada; o STCYCL seguinte é comando.
    run({0x30000000u, 11, 12, 13, 14,          // STROW
         0x20000000u, 0x55555555u,             // STMASK: tudo ROW
         0x01000400u, 0x7C040070u,             // STCYCL CL=0 WL=4; UNPACK V4-32 m NUM=4
         0x01000101u});                        // STCYCL 1,1
    CHECK(vif.idle());
    CHECK_EQ(vu1(0x70, 0), 11u);
    CHECK_EQ(vu1(0x73, 3), 14u);
    // CL=1 WL=3, NUM=5: lê 2 vetores (posições 0 e 3) e grava 5 seguidos;
    // as posições preenchidas, com máscara de linha 1-2 = ROW, vêm do ROW.
    run({0x20000000u, 0x00555500u,             // linha 0 dado; linhas 1-2 ROW
         0x01000301u, 0x7C050080u, 1, 2, 3, 4, 5, 6, 7, 8,
         0x01000101u});
    CHECK(vif.idle());
    CHECK_EQ(vu1(0x80, 0), 1u);
    CHECK_EQ(vu1(0x81, 0), 11u);
    CHECK_EQ(vu1(0x82, 3), 14u);
    CHECK_EQ(vu1(0x83, 0), 5u);
    CHECK_EQ(vu1(0x84, 1), 12u);
    // MPG
    run({0x4A020004u, 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u});
    std::uint32_t m;
    std::memcpy(&m, vu.micro1.get() + 32 + 12, 4);
    CHECK_EQ(m, 0x44444444u);
    // DIRECT: 1 quadword de GIFtag + ... vai para o GS (FINISH via A+D)
    Packet p;
    p.tag(1, true, 0, 1, 0xE);
    p.ad(FINISH, 0);
    std::vector<std::uint32_t> d = {0x50000002u};
    for (std::size_t i = 0; i < p.q.size(); ++i) {
        d.push_back(static_cast<std::uint32_t>(p.q[i]));
        d.push_back(static_cast<std::uint32_t>(p.q[i] >> 32));
    }
    run(d);
    CHECK_EQ(g.csr() & 2, 2ull);
    // MSCAL sem runtime (VIF isolado no teste): erro claro
    CHECK_THROWS_WITH(run({0x14000010u}), "MSCAL sem runtime");
    vif.reset();
    CHECK_THROWS_WITH(run({0x08000000u}), "VIFcode inválido");
}

// ---------------------------------------------------------------------------
// DMAC (com o runtime inteiro)
// ---------------------------------------------------------------------------

TEST_CASE(gs, dmac_chain_to_gif) {
    const ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    Runtime rt(info, RuntimeOptions{});
    Memory& m = rt.memory();
    // Pacote GIF (A+D FINISH) em 0x100010, encadeado: next → ref → end
    Packet p;
    p.tag(1, true, 0, 1, 0xE);
    p.ad(FINISH, 0);
    m.copyToGuest(0x00200000, p.q.data(), 32, 0);
    auto tag = [&](std::uint32_t addr, std::uint64_t qwc, std::uint64_t id, std::uint64_t target) {
        const std::uint64_t t[2] = {qwc | (id << 28) | (target << 32), 0};
        m.copyToGuest(addr, t, 16, 0);
    };
    tag(0x00100000, 0, 2, 0x00100100);  // next → 0x100100
    tag(0x00100100, 2, 3, 0x00200000);  // ref: os 2 qwords em 0x200000
    tag(0x00100110, 0, 7, 0);           // end
    m.write<std::uint32_t>(0x1000A030, 0x00100000, 0);  // D2_TADR
    m.write<std::uint32_t>(0x1000A020, 0, 0);           // D2_QWC
    m.write<std::uint32_t>(0x1000A000, 0x105, 0);       // CHCR: DIR, chain, STR
    CHECK_EQ(m.read<std::uint32_t>(0x1000A000, 0) & 0x100, 0u);  // terminou
    // O FINISH chega depois do trabalho do GS (ver gs.finish_latency); aqui
    // só interessa que ele chegue.
    auto drainedCsr = [&] {
        rt.gs().processEvents(~std::uint64_t{0});
        return rt.gs().csr();
    };
    CHECK_EQ(drainedCsr() & 2, 2ull);
    CHECK_EQ(m.read<std::uint32_t>(0x1000E010, 0) & 4, 4u);  // D_STAT.CIS2
    CHECK_EQ(m.read<std::uint32_t>(0x1000A030, 0), 0x00100120u);
    // Escrever 1 limpa o CIS; escrever 1 nos bits altos inverte o CIM
    m.write<std::uint32_t>(0x1000E010, 4 | (4u << 16), 0);
    CHECK_EQ(m.read<std::uint32_t>(0x1000E010, 0), 4u << 16);
    // D_ENABLEW (escrita) é lido de volta como o D_ENABLER (o GT4 lê os dois)
    m.write<std::uint32_t>(0x1000F590, 0x1201, 0);
    CHECK_EQ(m.read<std::uint32_t>(0x1000F590, 0), 0x1201u);
    CHECK_EQ(m.read<std::uint32_t>(0x1000F520, 0), 0x1201u);
    // Modo normal
    rt.gs().writePrivileged(0x12001000, 2, 0);
    m.write<std::uint32_t>(0x1000A010, 0x00200000, 0);
    m.write<std::uint32_t>(0x1000A020, 2, 0);
    m.write<std::uint32_t>(0x1000A000, 0x101, 0);
    CHECK_EQ(drainedCsr() & 2, 2ull);
    CHECK_EQ(m.read<std::uint32_t>(0x1000A010, 0), 0x00200020u);
    // GIF_FIFO por escrita de 128 bits
    rt.gs().writePrivileged(0x12001000, 2, 0);
    Reg128 q{};
    std::memcpy(&q, p.q.data(), 16);
    m.write128(0x10006000, q, 0);
    std::memcpy(&q, p.q.data() + 2, 16);
    m.write128(0x10006000, q, 0);
    CHECK_EQ(drainedCsr() & 2, 2ull);
    // Endereço fora da RAM: erro claro
    m.write<std::uint32_t>(0x1000A010, 0x0F000000, 0);
    m.write<std::uint32_t>(0x1000A020, 1, 0);
    CHECK_THROWS_WITH(m.write<std::uint32_t>(0x1000A000, 0x101, 0x300), "fora da RAM");
}

// O GS desenha em paralelo com o EE: o FINISH só aparece depois do tempo
// estimado de trabalho (GIF + pixels). Padrão do ps2sdk que depende disso:
// envia o DMA, graph_wait_vsync() escreve CSR |= CSR & 8 (o que limparia um
// FINISH já presente) e draw_wait_finish() espera o FINISH.
TEST_CASE(gs, finish_latency) {
    const ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    RuntimeOptions o;
    o.virtualClock = true;
    Runtime rt(info, o);
    Memory& m = rt.memory();
    Packet p;
    p.tag(3, true, 0, 1, 0xE);
    p.ad(PRIM, 6);                                   // sprite
    p.ad(XYZ2, 0);
    p.ad(FINISH, 0);
    m.copyToGuest(0x00200000, p.q.data(), static_cast<std::uint32_t>(p.q.size() * 8), 0);
    m.write<std::uint32_t>(0x1000A010, 0x00200000, 0);
    m.write<std::uint32_t>(0x1000A020, 4, 0);
    m.write<std::uint32_t>(0x1000A000, 0x101, 0);
    CHECK_EQ(m.read<std::uint32_t>(0x1000A000, 0) & 0x100, 0u);  // o DMA em si terminou
    CHECK_EQ(rt.timing().readGsCsr() & 2, 0ull);                  // o GS ainda não
    rt.gs().writePrivileged(0x12001000, rt.gs().csr(), 0);        // CSR |= CSR & 8
    const std::uint64_t due = rt.gs().nextEventTime();
    CHECK(due > rt.timing().now());
    CHECK(due - rt.timing().now() < 1000);  // 3 registradores: poucos ciclos
    CHECK_EQ(rt.timing().cyclesUntilNextEvent(), due - rt.timing().now());
    rt.timing().consume(static_cast<std::int64_t>(due - rt.timing().now()));
    CHECK_EQ(rt.timing().readGsCsr() & 2, 2ull);
    CHECK_EQ(rt.gs().nextEventTime(), ~std::uint64_t{0});
    // Trabalho antigo (o GS já terminou) não atrasa um FINISH muito depois.
    for (int i = 0; i < 1000; ++i) rt.gs().writeRegister(PRIM, 6, 0);
    rt.timing().consume(1000000);
    rt.gs().writeRegister(FINISH, 0, 0);
    CHECK(rt.gs().nextEventTime() - rt.timing().now() <= 4);
    rt.timing().consume(4);
    CHECK_EQ(rt.timing().readGsCsr() & 2, 2ull);
    rt.gs().writePrivileged(0x12001000, 2, 0);
    // Dois FINISH seguidos: cada um no seu tempo; o reset do GS cancela.
    m.write<std::uint32_t>(0x1000A020, 4, 0);
    m.write<std::uint32_t>(0x1000A010, 0x00200000, 0);
    m.write<std::uint32_t>(0x1000A000, 0x101, 0);
    rt.gs().writePrivileged(0x12001000, 0x200, 0);  // RESET
    CHECK_EQ(rt.gs().nextEventTime(), ~std::uint64_t{0});
    // GS isolado (sem relógio): imediato, ver gs.signal_finish_and_csr.
}

// Relógio real: o GS em software leva tempo de verdade, e dois FINISH que no
// hardware chegariam separados podem vencer juntos. Eles saem um por vez — o
// segundo só depois que o programa limpar o primeiro (o sample draw/teapot do
// ps2sdk espera dois FINISH por quadro e travava com o relógio real).
TEST_CASE(gs, finish_real_clock_one_at_a_time) {
    const ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    RuntimeOptions o;
    o.virtualClock = false;
    Runtime rt(info, o);
    rt.gs().writeRegister(FINISH, 0, 0);
    rt.gs().writeRegister(FINISH, 0, 0);
    const std::uint64_t later = rt.timing().now() + 1000000000ull;
    rt.gs().processEvents(later);
    CHECK_EQ(rt.gs().csr() & 2, 2ull);
    CHECK(rt.gs().nextEventTime() != ~std::uint64_t{0});  // o segundo continua na fila
    rt.gs().processEvents(later);                          // bit ainda ligado: espera
    CHECK(rt.gs().nextEventTime() != ~std::uint64_t{0});
    rt.gs().writePrivileged(0x12001000, 2, 0);             // o programa limpa o primeiro
    CHECK_EQ(rt.gs().csr() & 2, 0ull);
    rt.gs().processEvents(later);
    CHECK_EQ(rt.gs().csr() & 2, 2ull);
    CHECK_EQ(rt.gs().nextEventTime(), ~std::uint64_t{0});
}

// VIFcode com bit I: o VIF para ao fim do comando (STAT.VIS/INT, INTC 5) e o
// DMA do VIF1 pausa no ponto exato, com STR ligado; FBRST.STC processa o
// resto do quadword que ficou no FIFO e o DMA continua. ERR.MII ignora o bit.
// (Gran Turismo 4 sincroniza o desenho assim.)
TEST_CASE(gs, vif1_interrupt_bit_stalls_and_resumes_dma) {
    const ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    RuntimeOptions o;
    o.virtualClock = true;
    Runtime rt(info, o);
    Memory& m = rt.memory();
    constexpr std::uint32_t kStat = 0x10003C00, kFbrst = 0x10003C10, kErr = 0x10003C20, kMark = 0x10003C30,
                            kCode = 0x10003C80, kChcr = 0x10009000, kMadr = 0x10009010, kQwc = 0x10009020,
                            kTadr = 0x10009030;
    constexpr std::uint32_t kInt = 1u << 11, kVis = 1u << 10, kStr = 0x100;
    auto put = [&](std::uint32_t addr, std::initializer_list<std::uint32_t> words) {
        for (std::uint32_t w : words) {
            m.write<std::uint32_t>(addr, w, 0);
            addr += 4;
        }
    };
    const std::uint32_t mark = 0x07000000u, flushI = 0x91010000u, nop = 0;

    // Chain: cnt com [MARK 1, FLUSH+I, MARK 2, NOP]; end com [MARK 3, ...].
    put(0x00200000, {0x10000001u, 0, 0, 0, mark | 1, flushI, mark | 2, nop});
    put(0x00200020, {0x70000001u, 0, 0, 0, mark | 3, nop, nop, nop});
    m.write<std::uint32_t>(kTadr, 0x00200000, 0);
    m.write<std::uint32_t>(kQwc, 0, 0);
    m.write<std::uint32_t>(kChcr, 0x105, 0);  // chain, para o VIF1, STR
    CHECK_EQ(m.read<std::uint32_t>(kChcr, 0) & kStr, kStr);  // pausado
    CHECK_EQ(m.read<std::uint32_t>(kMark, 0), 1u);
    CHECK_EQ(m.read<std::uint32_t>(kStat, 0) & (kInt | kVis), kInt | kVis);
    CHECK_EQ(m.read<std::uint32_t>(kCode, 0), flushI);
    CHECK_EQ(rt.kernel().intcStat() & (1u << 5), 1u << 5);
    CHECK_EQ(m.read<std::uint32_t>(kTadr, 0), 0x00200020u);
    m.write<std::uint32_t>(kFbrst, 8, 0);  // STC
    CHECK_EQ(m.read<std::uint32_t>(kMark, 0), 3u);
    CHECK_EQ(m.read<std::uint32_t>(kChcr, 0) & kStr, 0u);
    CHECK_EQ(m.read<std::uint32_t>(kStat, 0) & (kInt | kVis), 0u);

    // Bit I no DMAtag (TTE): para antes dos dados do tag.
    put(0x00200040, {0x70000001u, 0, mark | 5, flushI, mark | 6, nop, nop, nop});
    m.write<std::uint32_t>(kTadr, 0x00200040, 0);
    m.write<std::uint32_t>(kChcr, 0x145, 0);  // + TTE
    CHECK_EQ(m.read<std::uint32_t>(kMark, 0), 5u);
    CHECK_EQ(m.read<std::uint32_t>(kChcr, 0) & kStr, kStr);
    m.write<std::uint32_t>(kFbrst, 8, 0);
    CHECK_EQ(m.read<std::uint32_t>(kMark, 0), 6u);
    CHECK_EQ(m.read<std::uint32_t>(kChcr, 0) & kStr, 0u);

    // Modo normal: para no meio do 1º quadword; o resto espera no FIFO.
    put(0x00200080, {mark | 7, flushI, mark | 8, nop, mark | 9, nop, nop, nop});
    m.write<std::uint32_t>(kMadr, 0x00200080, 0);
    m.write<std::uint32_t>(kQwc, 2, 0);
    m.write<std::uint32_t>(kChcr, 0x101, 0);
    CHECK_EQ(m.read<std::uint32_t>(kMark, 0), 7u);
    CHECK_EQ(m.read<std::uint32_t>(kQwc, 0), 1u);
    CHECK_EQ((m.read<std::uint32_t>(kStat, 0) >> 24) & 0x1F, 1u);  // FQC
    m.write<std::uint32_t>(kFbrst, 8, 0);
    CHECK_EQ(m.read<std::uint32_t>(kMark, 0), 9u);
    CHECK_EQ(m.read<std::uint32_t>(kChcr, 0) & kStr, 0u);

    // ERR.MII: o bit I é ignorado.
    m.write<std::uint32_t>(kErr, 1, 0);
    m.write<std::uint32_t>(kMadr, 0x00200080, 0);
    m.write<std::uint32_t>(kQwc, 2, 0);
    m.write<std::uint32_t>(kChcr, 0x101, 0);
    CHECK_EQ(m.read<std::uint32_t>(kMark, 0), 9u);
    CHECK_EQ(m.read<std::uint32_t>(kChcr, 0) & kStr, 0u);
    CHECK_EQ(m.read<std::uint32_t>(kStat, 0) & (kInt | kVis), 0u);
}

TEST_CASE(gs, dmac_scratchpad_and_vif_memory_map) {
    const ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    Runtime rt(info, RuntimeOptions{});
    Memory& m = rt.memory();
    for (std::uint32_t i = 0; i < 8; ++i) m.write<std::uint32_t>(0x00300000 + i * 4, 0xC0DE0000u + i, 0);
    // toSPR: RAM → scratchpad
    m.write<std::uint32_t>(0x1000D410, 0x00300000, 0);
    m.write<std::uint32_t>(0x1000D480, 0x100, 0);
    m.write<std::uint32_t>(0x1000D420, 2, 0);
    m.write<std::uint32_t>(0x1000D400, 0x100, 0);
    CHECK_EQ(m.read<std::uint32_t>(0x70000100 + 28, 0), 0xC0DE0007u);
    // fromSPR: scratchpad → RAM
    m.write<std::uint32_t>(0x1000D010, 0x00400000, 0);
    m.write<std::uint32_t>(0x1000D080, 0x100, 0);
    m.write<std::uint32_t>(0x1000D020, 2, 0);
    m.write<std::uint32_t>(0x1000D000, 0x100, 0);
    CHECK_EQ(m.read<std::uint32_t>(0x00400004, 0), 0xC0DE0001u);
    // Memória do VU1 visível em 0x1100C000
    m.write<std::uint32_t>(0x1100C010, 0x12345678u, 0);
    std::uint32_t v;
    std::memcpy(&v, rt.vu().data1.get() + 0x10, 4);
    CHECK_EQ(v, 0x12345678u);
}
