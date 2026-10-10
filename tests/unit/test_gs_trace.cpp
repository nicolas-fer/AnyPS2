// Testes do rastreador de desenhos do GS (gs_trace.cpp): a sonda tem de listar os
// desenhos que escrevem no ponto, com a cor e o Z antes e depois de cada um, e
// deixar de fora os que não tocam o ponto, estão fora do intervalo de VBlanks ou
// têm outro FBP. O log de desenhos tem de dar uma linha por desenho do intervalo.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <fstream>
#include <sstream>
#include <string>

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
    return ss.str();
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
