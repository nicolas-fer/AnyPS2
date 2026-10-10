// Testes do rastreador de desenhos do GS (gs_trace.cpp): a sonda tem de listar os
// desenhos que escrevem no ponto, com a cor e o Z antes e depois de cada um, e
// deixar de fora os que não tocam o ponto, estão fora do intervalo de VBlanks ou
// têm outro FBP. O log de desenhos tem de dar uma linha por desenho do intervalo.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <initializer_list>
#include <fstream>
#include <sstream>
#include <string>

#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/gs/gs_trace.h"
#include "minitest.h"

using namespace anyps2::rt;
using namespace anyps2::rt::gs;

namespace {

std::uint64_t frameReg(std::uint64_t fbp, std::uint64_t fbw, std::uint64_t psm) {
    return fbp | (fbw << 16) | (psm << 24);
}
std::uint64_t scissorReg(std::uint64_t x0, std::uint64_t x1, std::uint64_t y0, std::uint64_t y1) {
    return x0 | (x1 << 16) | (y0 << 32) | (y1 << 48);
}
std::uint64_t xyzPx(std::uint64_t x, std::uint64_t y) {
    return (x * 16) | ((y * 16) << 16);
}
std::uint64_t rgbaq(unsigned r, unsigned g, unsigned b, unsigned a) {
    return r | (g << 8) | (b << 16) | (std::uint64_t{a} << 24) | (std::uint64_t{0x3F800000u} << 32);
}
// Sprite com ABE: PRIM=6, o atributo vem do PRIM (PRMODECONT=1).
void sprite(Gs& g, unsigned x0, unsigned y0, unsigned x1, unsigned y1, std::uint64_t color, std::uint32_t pc) {
    g.writeRegister(PRIM, 6 | 64, pc);
    g.writeRegister(RGBAQ, color, pc);
    g.writeRegister(XYZ2, xyzPx(x0, y0), pc);
    g.writeRegister(XYZ2, xyzPx(x1, y1), pc);
}

// PSMCT32 em FBP 0 (FBW 1), Z desligado (ZMSK), TEST sem teste de Z, blending
// (A−B)·C/128+D com A=Cs, B=Cd, C=As, D=Cd (mistura alfa padrão).
void setupFb(Gs& g) {
    g.writeRegister(FRAME_1, frameReg(0, 1, PSMCT32), 0);
    g.writeRegister(ZBUF_1, std::uint64_t{1} << 32, 0);
    g.writeRegister(SCISSOR_1, scissorReg(0, 63, 0, 63), 0);
    g.writeRegister(TEST_1, (1ull << 16) | (1ull << 17), 0);
    g.writeRegister(ALPHA_1, 4 | 64, 0);
    g.writeRegister(XYOFFSET_1, 0, 0);
    g.writeRegister(PRMODECONT, 1, 0);
    g.writeRegister(COLCLAMP, 1, 0);
}

std::string readFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());  // Windows grava \r\n
    return text;
}

// Bloco de um desenho ("=== VBlank ... | desenho N | ...") até o próximo.
std::string blockOf(const std::string& text, unsigned draw) {
    const std::string key = "| desenho " + std::to_string(draw) + " |";
    const std::size_t at = text.find(key);
    if (at == std::string::npos) return "";
    const std::size_t start = text.rfind("=== VBlank", at);
    const std::size_t end = text.find("=== VBlank", at + 1);
    return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// Valor "0x........" que vem depois do rótulo (primeiro "raw=" a partir dele).
std::string rawAfter(const std::string& block, const std::string& label) {
    const std::size_t at = block.find(label);
    if (at == std::string::npos) return "";
    const std::size_t raw = block.find("raw=", at);
    if (raw == std::string::npos) return "";
    return block.substr(raw + 4, 10);
}

void closeTrace(Gs& g) {
    g.trace().configure(GsTraceConfig{});
}

}  // namespace

// Dois sprites com mistura alfa sobre o ponto (14, 14) e um terceiro longe dele.
TEST_CASE(gs_trace, probe_records_overlapping_draws_with_before_and_after) {
    const char* out = "anyps2_gs_trace_probe_test.txt";
    Gs g(nullptr);
    setupFb(g);
    GsTraceConfig cfg;
    cfg.points = {{14, 14}};
    cfg.probeFrom = 1;
    cfg.probeTo = 1;
    cfg.probeOut = out;
    g.trace().configure(cfg);
    CHECK(g.trace().active());

    g.vblankStart();  // VBlank 1: os desenhos abaixo são deste intervalo
    sprite(g, 8, 8, 24, 24, rgbaq(200, 0, 0, 0x40), 0x100);   // desenho 1: 0x40000064
    sprite(g, 12, 12, 20, 20, rgbaq(0, 0, 200, 0x80), 0x200);  // desenho 2: 0x80C80000
    sprite(g, 40, 40, 50, 50, rgbaq(9, 9, 9, 0x80), 0x300);    // desenho 3: não toca (14, 14)
    closeTrace(g);

    const std::string text = readFile(out);
    CHECK(!text.empty());
    const std::string d1 = blockOf(text, 1), d2 = blockOf(text, 2);
    CHECK(!d1.empty());
    CHECK(!d2.empty());
    CHECK(blockOf(text, 3).empty());
    CHECK(d1.find("pc 0x00000100") != std::string::npos);
    CHECK(d2.find("pc 0x00000200") != std::string::npos);

    CHECK(rawAfter(d1, "cor antes:") == "0x00000000");
    CHECK(rawAfter(d1, "cor depois:") == "0x40000064");
    CHECK(rawAfter(d2, "cor antes:") == "0x40000064");
    CHECK(rawAfter(d2, "cor depois:") == "0x80C80000");
    CHECK(d2.find("PONTO (14, 14)") != std::string::npos);
    CHECK(d2.find("PRIM=0x") != std::string::npos);
    CHECK(d2.find("FRAME: FBP=0x000") != std::string::npos);
    std::remove(out);
}

// Fora do intervalo de VBlanks, fora do FBP filtrado ou fora do ponto: nada.
TEST_CASE(gs_trace, probe_ignores_other_points_ranges_and_buffers) {
    const char* out = "anyps2_gs_trace_probe_test2.txt";
    Gs g(nullptr);
    setupFb(g);
    GsTraceConfig cfg;
    cfg.points = {{50, 50}};
    cfg.probeOut = out;
    cfg.probeFrom = 1;
    cfg.probeTo = 1;
    g.trace().configure(cfg);

    sprite(g, 8, 8, 24, 24, rgbaq(200, 0, 0, 0x40), 0x100);  // não toca (50, 50): ignorado
    g.vblankStart();
    sprite(g, 40, 40, 60, 60, rgbaq(9, 9, 9, 0x80), 0x300);  // toca (50, 50), VBlank 1
    closeTrace(g);

    std::string text = readFile(out);
    CHECK(blockOf(text, 1).empty());
    CHECK(!blockOf(text, 2).empty());
    CHECK_EQ(rawAfter(blockOf(text, 2), "cor depois:"), std::string("0x80090909"));

    // Intervalo que ainda não chegou: nenhum desenho.
    cfg.probeFrom = 2;
    cfg.probeTo = 5;
    g.trace().configure(cfg);
    sprite(g, 40, 40, 60, 60, rgbaq(9, 9, 9, 0x80), 0x400);
    closeTrace(g);
    text = readFile(out);
    CHECK(text.find("=== VBlank") == std::string::npos);

    // FBP filtrado: o FRAME deste teste é FBP 0, então o filtro para 1 deixa tudo de fora.
    cfg.probeFrom = 0;
    cfg.probeTo = ~std::uint64_t{0};
    cfg.filterFbp = true;
    cfg.fbp = 1;
    g.trace().configure(cfg);
    sprite(g, 40, 40, 60, 60, rgbaq(9, 9, 9, 0x80), 0x500);
    closeTrace(g);
    text = readFile(out);
    CHECK(text.find("=== VBlank") == std::string::npos);
    std::remove(out);
}

// O log de desenhos dá uma linha por desenho do intervalo, sem ler a VRAM.
TEST_CASE(gs_trace, drawlog_has_one_line_per_draw_in_range) {
    const char* out = "anyps2_gs_trace_log_test.txt";
    Gs g(nullptr);
    setupFb(g);
    GsTraceConfig cfg;
    cfg.drawLog = out;
    g.trace().configure(cfg);

    sprite(g, 8, 8, 24, 24, rgbaq(200, 0, 0, 0x40), 0x100);
    sprite(g, 12, 12, 20, 20, rgbaq(0, 0, 200, 0x80), 0x200);
    g.vblankStart();
    sprite(g, 40, 40, 50, 50, rgbaq(9, 9, 9, 0x80), 0x300);
    closeTrace(g);

    std::string text = readFile(out);
    std::size_t lines = 0;
    for (std::size_t at = text.find("vb="); at != std::string::npos; at = text.find("vb=", at + 1)) ++lines;
    CHECK_EQ(lines, std::size_t{3});
    CHECK(text.find("vb=0 desenho=1 pc=0x00000100") != std::string::npos);
    CHECK(text.find("vb=1 desenho=3 pc=0x00000300") != std::string::npos);
    CHECK(text.find("ALPHA=0,1,0,1,0x00") != std::string::npos);

    // Intervalo de VBlanks que não tem desenhos: arquivo vazio.
    cfg.drawFrom = 2;
    g.trace().configure(cfg);
    sprite(g, 40, 40, 50, 50, rgbaq(9, 9, 9, 0x80), 0x400);
    closeTrace(g);
    CHECK(readFile(out).empty());
    std::remove(out);
}

// Sem nenhuma configuração o rastreador fica inativo: o Gs não escreve nada.
TEST_CASE(gs_trace, inactive_by_default) {
    Gs g(nullptr);
    setupFb(g);
    CHECK(!g.trace().active());
    sprite(g, 8, 8, 24, 24, rgbaq(200, 0, 0, 0x40), 0x100);
    CHECK_EQ(g.drawCount(), std::uint64_t{1});
}

namespace {

void setTraceEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

}  // namespace

TEST_CASE(gs_trace, from_env_parses_points_and_rejects_bad_ones) {
    const char* out = "gs_trace_env_test.txt";
    setTraceEnv("ANYPS2_GS_PROBE_OUT", out);
    setTraceEnv("ANYPS2_GS_DRAWLOG", "");
    // Dois pontos válidos: liga a sonda.
    setTraceEnv("ANYPS2_GS_PROBE", "10,20,30,40");
    CHECK(GsTrace::fromEnv() != nullptr);
    // Formatos inválidos (coordenada ímpar, negativa, lixo, mais de 8 pontos): sonda
    // desligada, e sem o log não há rastreador.
    for (const char* bad : {"10", "10,-2", "10,2x", "1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9"}) {
        setTraceEnv("ANYPS2_GS_PROBE", bad);
        CHECK(GsTrace::fromEnv() == nullptr);
    }
    setTraceEnv("ANYPS2_GS_PROBE", "");
    CHECK(GsTrace::fromEnv() == nullptr);
    setTraceEnv("ANYPS2_GS_PROBE_OUT", "");
    std::remove(out);
}

// ---------------------------------------------------------------------------
// ANYPS2_GS_TEXDUMP e ANYPS2_GS_VRAMLOG
// ---------------------------------------------------------------------------

namespace {

std::uint64_t bltbuf(std::uint64_t sbp, std::uint64_t sbw, std::uint64_t spsm, std::uint64_t dbp,
                     std::uint64_t dbw, std::uint64_t dpsm) {
    return sbp | (sbw << 16) | (spsm << 24) | (dbp << 32) | (dbw << 48) | (dpsm << 56);
}
std::uint64_t trxreg(std::uint64_t w, std::uint64_t h) {
    return w | (h << 32);
}
std::size_t countOf(const std::string& text, const std::string& what) {
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n;
    return n;
}
std::uint32_t clutColor(unsigned i) {
    return (i * 16 + 1) | ((i * 8u) << 8) | (0x40u << 16) | (0x80u << 24);
}
// O mesmo valor como o despejo escreve: RRGGBBAA.
std::string clutHex(unsigned i) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%02X%02X%02X%02X", (i * 16 + 1) & 0xFF, (i * 8) & 0xFF, 0x40, 0x80);
    return buf;
}

// Textura PSMT4 16x8 em TBP0 0x100 com índice (x + y) & 15 e uma CLUT CT32 em CBP 0x200
// carregada no CSA indicado pelo TEX0 com CLD=1.
void setupPsmt4Texture(Gs& g, std::uint64_t csa) {
    for (std::uint32_t y = 0; y < 8; ++y)
        for (std::uint32_t x = 0; x < 16; ++x) g.vram().writePixel(PSMT4, 0x100, 1, x, y, (x + y) & 15);
    for (unsigned i = 0; i < 16; ++i) g.vram().writePixel(PSMCT32, 0x200, 1, i & 7, i >> 3, clutColor(i));
    const std::uint64_t tex0 = 0x100 | (1ull << 14) | (std::uint64_t{PSMT4} << 20) | (4ull << 26) | (3ull << 30) |
                               (1ull << 34) | (1ull << 35) | (0x200ull << 37) | (std::uint64_t{PSMCT32} << 51) |
                               (csa << 56) | (1ull << 61);
    g.writeRegister(TEX0_1, tex0, 0);
}

void texSprite(Gs& g, std::uint32_t pc) {
    g.writeRegister(PRIM, 6 | 16 | 256, pc);  // sprite, TME, FST
    g.writeRegister(RGBAQ, rgbaq(128, 128, 128, 0x80), pc);
    g.writeRegister(UV, 0, pc);
    g.writeRegister(XYZ2, xyzPx(0, 0), pc);
    g.writeRegister(UV, (16u << 4) | ((8u << 4) << 16), pc);
    g.writeRegister(XYZ2, xyzPx(16, 8), pc);
}

// Registrador A+D (dados, endereço) como quadword de um pacote PACKED.
void adQword(std::vector<std::uint8_t>& v, std::uint64_t data, std::uint8_t reg) {
    const std::size_t at = v.size();
    v.resize(at + 16, 0);
    std::memcpy(v.data() + at, &data, 8);
    v[at + 8] = reg;
}

}  // namespace

// Textura PSMT4 com CLUT no CSA 2: o despejo traz os índices, o RGBA depois da CLUT
// (entradas 32 a 47 do buffer) e as 16 entradas da fatia.
TEST_CASE(gs_trace, texdump_psmt4_with_csa_shows_indices_rgba_and_clut_slice) {
    const char* out = "anyps2_gs_texdump_test.txt";
    const char* png = "anyps2_gs_texdump_test_desenho1.png";
    Gs g(nullptr);
    setupFb(g);
    GsTraceConfig cfg;
    cfg.texDump = out;
    cfg.texDumpDraws = {{1, 1}};
    g.trace().configure(cfg);
    CHECK(g.trace().active());

    setupPsmt4Texture(g, 2);
    texSprite(g, 0x100);  // desenho 1: despejado
    texSprite(g, 0x200);  // desenho 2: fora da lista
    closeTrace(g);

    const std::string text = readFile(out);
    CHECK_EQ(countOf(text, "=== desenho"), std::size_t{1});
    CHECK(text.find("=== desenho 1 | VBlank 0 | pc 0x00000100") != std::string::npos);
    CHECK(text.find("TEX0: TBP0=0x0100 TBW=1 PSM=PSMT4") != std::string::npos);
    CHECK(text.find("CSA=2") != std::string::npos);
    CHECK(text.find("textura do nível 0: 16x8 PSMT4") != std::string::npos);
    // Índices: a linha y é (x + y) & 15, um dígito por texel.
    CHECK(text.find("\n  0123456789ABCDEF\n") != std::string::npos);
    CHECK(text.find("\n  123456789ABCDEF0\n") != std::string::npos);
    CHECK(text.find("\n  789ABCDEF0123456\n") != std::string::npos);
    // RGBA da primeira linha: CLUT[0], CLUT[1], ... (R,G,B,A do raw de 32 bits).
    CHECK(text.find("\n  " + clutHex(0) + " " + clutHex(1) + " " + clutHex(2) + " ") != std::string::npos);
    // Fatia da CLUT: 16 entradas, na ordem, nenhuma vazia (o CSA 0 nunca foi carregado).
    CHECK(text.find("CLUT em uso (16 entradas a partir da entrada 32, CPSM=PSMCT32, CSA=2)") != std::string::npos);
    CHECK(text.find("\n  [0] " + clutHex(0) + "\n") != std::string::npos);
    CHECK(text.find("\n  [15] " + clutHex(15) + "\n") != std::string::npos);
    CHECK(text.find("[16]") == std::string::npos);
    CHECK(!readFile(png).empty());
    std::remove(out);
    std::remove(png);
}

// Sem TME o despejo só registra isso.
TEST_CASE(gs_trace, texdump_without_texture_says_so) {
    const char* out = "anyps2_gs_texdump_test2.txt";
    Gs g(nullptr);
    setupFb(g);
    GsTraceConfig cfg;
    cfg.texDump = out;
    cfg.texDumpDraws = {{1, 1}};
    g.trace().configure(cfg);
    sprite(g, 0, 0, 8, 8, rgbaq(1, 2, 3, 0x80), 0x100);
    closeTrace(g);
    const std::string text = readFile(out);
    CHECK(text.find("TME=0: o desenho não usa textura") != std::string::npos);
    CHECK(text.find("ÍNDICES") == std::string::npos);
    std::remove(out);
}

// HOST→LOCAL: dentro da faixa grava início e fim, fora dela nada. A origem é "direto"
// nos registradores escritos no Gs e PATH3 nos dados que passam pelo GIF.
TEST_CASE(gs_trace, vramlog_host_transfer_in_and_out_of_range_with_origin) {
    const char* out = "anyps2_gs_vramlog_test.txt";
    Gs g(nullptr);
    GsTraceConfig cfg;
    cfg.vramLog = out;
    cfg.vramBlocks = {{0x3800, 0x385F}, {0x3660, 0x367F}};
    g.trace().configure(cfg);
    CHECK(g.trace().vramLogActive());
    Gif gif(nullptr, g);

    auto host = [&](std::uint64_t dbp, bool viaGif) {
        g.writeRegister(BITBLTBUF, bltbuf(0, 0, 0, dbp, 2, PSMT4), 0x10);
        g.writeRegister(TRXPOS, 0, 0x10);
        g.writeRegister(TRXREG, trxreg(32, 32), 0x10);
        g.writeRegister(TRXDIR, 0, 0x10);
        // 32x32 de 4 bits = 4096 bits = 64 palavras = 32 quadwords, depois do GIFtag IMAGE.
        std::vector<std::uint8_t> pkt(16 * 33, 0);
        const std::uint64_t tag = 32 | (1ull << 15) | (2ull << 58);
        std::memcpy(pkt.data(), &tag, 8);
        if (viaGif) {
            gif.transfer(3, pkt.data(), 33, 0x20);
        } else {
            for (int i = 0; i < 64; ++i) g.writeTransferData(0, 0x20);
        }
    };
    host(0x3810, false);  // dentro da faixa, escrita direta
    host(0x1000, false);  // fora
    host(0x3810, true);   // dentro; os dados passam pelo PATH3
    closeTrace(g);

    const std::string text = readFile(out);
    CHECK_EQ(countOf(text, "HOST→LOCAL início"), std::size_t{2});
    CHECK_EQ(countOf(text, "HOST→LOCAL fim"), std::size_t{2});
    CHECK_EQ(countOf(text, "DBP=0x1000"), std::size_t{0});
    CHECK(text.find("origem=direto xgkick=0 pc=0x00000010 HOST→LOCAL início DBP=0x3810 DBW=2 DPSM=PSMT4 "
                    "retângulo=(0,0)+32x32") != std::string::npos);
    // Segunda transferência: o início é direto (registradores escritos no Gs), o fim vem do PATH3.
    const std::size_t second = text.find("HOST→LOCAL início", text.find("HOST→LOCAL fim"));
    CHECK(second != std::string::npos);
    CHECK(text.find("origem=PATH3", second) != std::string::npos);
    std::remove(out);
}

// Tudo pelo PATH3 (GIFtag PACKED com A+D), e um XGKICK (PATH1) com carga de CLUT fora e
// dentro da faixa: a origem e o contador de XGKICK aparecem na linha.
TEST_CASE(gs_trace, vramlog_clut_load_and_origin_path1_path3) {
    const char* out = "anyps2_gs_vramlog_test2.txt";
    Gs g(nullptr);
    GsTraceConfig cfg;
    cfg.vramLog = out;
    cfg.vramBlocks = {{0x3660, 0x367F}};
    g.trace().configure(cfg);
    Gif gif(nullptr, g);

    const std::uint64_t adRegs = 0xE;  // REGS: A+D
    // PATH3: BITBLTBUF, TRXPOS, TRXREG e TRXDIR (A+D), NLOOP=4.
    std::vector<std::uint8_t> p3(16, 0);
    const std::uint64_t lo3 = 4 | (1ull << 60);  // NLOOP=4, FLG=PACKED, NREG=1
    std::memcpy(p3.data(), &lo3, 8);
    std::memcpy(p3.data() + 8, &adRegs, 8);
    adQword(p3, bltbuf(0, 0, 0, 0x3660, 1, PSMCT32), 0x50);
    adQword(p3, 0, 0x51);
    adQword(p3, trxreg(2, 1), 0x52);
    adQword(p3, 0, 0x53);
    gif.transfer(3, p3.data(), 5, 0x30);

    // PATH1: TEX0_1 com CLD=1 (PSMT4, CSA 2); CBP fora e dentro da faixa.
    auto clutKick = [&](std::uint64_t cbp) {
        std::vector<std::uint8_t> p1(16, 0);
        const std::uint64_t lo1 = 1 | (1ull << 15) | (1ull << 60);  // NLOOP=1, EOP, PACKED, NREG=1
        std::memcpy(p1.data(), &lo1, 8);
        std::memcpy(p1.data() + 8, &adRegs, 8);
        const std::uint64_t tex0 = (std::uint64_t{PSMT4} << 20) | (cbp << 37) | (std::uint64_t{PSMCT32} << 51) |
                                   (2ull << 56) | (1ull << 61);
        adQword(p1, tex0, 0x06);
        p1.resize(64, 0);
        gif.kick(p1.data(), 64, 0, 0x40);
    };
    clutKick(0x1000);
    clutKick(0x366A);
    closeTrace(g);

    const std::string text = readFile(out);
    CHECK(text.find("origem=PATH3 xgkick=0 pc=0x00000030 HOST→LOCAL início DBP=0x3660 DBW=1 DPSM=PSMCT32") !=
          std::string::npos);
    CHECK_EQ(countOf(text, "CLUT carga"), std::size_t{1});
    CHECK(text.find("origem=PATH1 xgkick=2 pc=0x00000040 CLUT carga CBP=0x366A CPSM=PSMCT32 CSM=0 CSA=2 CLD=1 "
                    "TPSM=PSMT4") != std::string::npos);
    CHECK_EQ(g.xgkicks(), std::uint64_t{2});
    std::remove(out);
}

// Desenho cujo FRAME cai nas faixas; LOCAL→LOCAL para as faixas; intervalo de VBlanks.
TEST_CASE(gs_trace, vramlog_draw_frame_local_copy_and_vblank_range) {
    const char* out = "anyps2_gs_vramlog_test3.txt";
    Gs g(nullptr);
    setupFb(g);
    GsTraceConfig cfg;
    cfg.vramLog = out;
    cfg.vramBlocks = {{0x3800, 0x385F}};
    cfg.vramFrom = 1;
    g.trace().configure(cfg);

    // VBlank 0: fora do intervalo, nada é gravado.
    g.writeRegister(FRAME_1, frameReg(0x3800 / 32, 1, PSMCT32), 0);
    sprite(g, 0, 0, 8, 8, rgbaq(1, 2, 3, 0x80), 0x100);
    g.vblankStart();

    sprite(g, 0, 0, 8, 8, rgbaq(1, 2, 3, 0x80), 0x110);  // FRAME em 0x3800: gravado
    g.writeRegister(FRAME_1, frameReg(0, 1, PSMCT32), 0);
    sprite(g, 0, 0, 8, 8, rgbaq(1, 2, 3, 0x80), 0x120);  // FRAME em 0: não
    g.writeRegister(BITBLTBUF, bltbuf(0, 1, PSMCT32, 0x3820, 1, PSMCT32), 0x130);
    g.writeRegister(TRXPOS, 0, 0x130);
    g.writeRegister(TRXREG, trxreg(8, 8), 0x130);
    g.writeRegister(TRXDIR, 2, 0x130);  // LOCAL→LOCAL para a faixa
    g.writeRegister(BITBLTBUF, bltbuf(0x3820, 1, PSMCT32, 0x1000, 1, PSMCT32), 0x140);
    g.writeRegister(TRXDIR, 2, 0x140);  // LOCAL→LOCAL para fora
    closeTrace(g);

    const std::string text = readFile(out);
    CHECK_EQ(countOf(text, "DESENHO"), std::size_t{1});
    CHECK(text.find("vb=1 desenho=2 origem=direto xgkick=0 pc=0x00000110 DESENHO sprite FRAME FBP=0x1C0 "
                    "(bloco 0x3800) FBW=1 PSM=PSMCT32") != std::string::npos);
    CHECK_EQ(countOf(text, "LOCAL→LOCAL"), std::size_t{1});
    CHECK(text.find("pc=0x00000130 LOCAL→LOCAL origem: SBP=0x0000 SBW=1 SPSM=PSMCT32 (0,0) destino: DBP=0x3820") !=
          std::string::npos);
    CHECK(text.find("pc=0x00000100") == std::string::npos);
    std::remove(out);
}

TEST_CASE(gs_trace, from_env_parses_texdump_and_vramlog) {
    setTraceEnv("ANYPS2_GS_PROBE", "");
    setTraceEnv("ANYPS2_GS_DRAWLOG", "");
    setTraceEnv("ANYPS2_GS_TEXDUMP", "gs_trace_env_texdump.txt");
    setTraceEnv("ANYPS2_GS_TEXDUMP_DRAWS", "");
    CHECK(GsTrace::fromEnv() == nullptr);  // sem lista de desenhos o despejo não liga
    setTraceEnv("ANYPS2_GS_TEXDUMP_DRAWS", "5,10-12");
    CHECK(GsTrace::fromEnv() != nullptr);
    setTraceEnv("ANYPS2_GS_TEXDUMP_DRAWS", "12-10");
    CHECK(GsTrace::fromEnv() == nullptr);
    setTraceEnv("ANYPS2_GS_TEXDUMP", "");
    setTraceEnv("ANYPS2_GS_VRAMLOG", "gs_trace_env_vramlog.txt");
    setTraceEnv("ANYPS2_GS_VRAMLOG_BLOCKS", "");
    CHECK(GsTrace::fromEnv() == nullptr);
    setTraceEnv("ANYPS2_GS_VRAMLOG_BLOCKS", "0x3800-0x385F,0x3660-0x367F");
    std::unique_ptr<GsTrace> t = GsTrace::fromEnv();
    CHECK(t != nullptr);
    CHECK(t && t->vramLogActive());
    t.reset();
    setTraceEnv("ANYPS2_GS_VRAMLOG_BLOCKS", "0x3800-0x4000");  // além de 0x3FFF
    CHECK(GsTrace::fromEnv() == nullptr);
    setTraceEnv("ANYPS2_GS_VRAMLOG", "");
    std::remove("gs_trace_env_texdump.txt");
    std::remove("gs_trace_env_vramlog.txt");
}
