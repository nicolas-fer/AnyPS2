// Testes do rastreador de dados do VU1 (vu_trace.cpp): um microprograma pequeno
// monta um pacote GIF PACKED (RGBAQ + XYZ2) na memória de dados, dá XGKICK, e o
// arquivo do rastreador tem de trazer a entrada, a memória no XGKICK e o pacote
// decodificado; XGKICK fora da lista, VBlank fora do intervalo e o limite de
// microprogramas não gravam nada.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/vif.h"
#include "anyps2/runtime/vu/vu.h"
#include "anyps2/runtime/vu/vu_trace.h"
#include "minitest.h"

using namespace anyps2::rt;

namespace {

constexpr std::uint32_t kLNop = 0x8000033Cu, kUNop = 0x000002FFu;
constexpr std::uint32_t X = 8;

std::uint32_t lo(std::uint32_t op, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs, std::int32_t imm11) {
    return (op << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (static_cast<std::uint32_t>(imm11) & 0x7FF);
}
std::uint32_t lox(std::uint32_t idx, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs) {
    return (0x40u << 25) | (dest << 21) | (ft << 16) | (fs << 11) | ((idx >> 2) << 6) | 0x3C | (idx & 3);
}

struct Rig {
    ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    Runtime rt{info, RuntimeOptions{}};

    // Pacote no qword 0x10: GIFtag (NLOOP=1, EOP, PACKED, NREG=2, REGS=RGBAQ,XYZ2),
    // RGBAQ (17, 256, -1, 68) e XYZ2 (x=400 px, y=300 px, z=0x1234, ADC=1).
    // O qword 0x20 começa zerado e o microprograma grava 0x77 nele (ISW) antes do
    // XGKICK em 0x18.
    Rig() {
        std::uint8_t* d = rt.vu().data1.get();
        const std::uint64_t tag[2] = {1ull | (1ull << 15) | (2ull << 60), 0x51};
        const std::uint64_t rgba[2] = {17ull | (0x100ull << 32), 0xFFFFFFFFull | (68ull << 32)};
        const std::uint64_t xyz[2] = {6400ull | (4800ull << 32), 0x1234ull | (1ull << 47)};
        std::memcpy(d + 0x10 * 16, tag, 16);
        std::memcpy(d + 0x11 * 16, rgba, 16);
        std::memcpy(d + 0x12 * 16, xyz, 16);
        std::uint8_t* m = rt.vu().micro1.get();
        const std::pair<std::uint32_t, std::uint32_t> prog[] = {
            {lo(0x08, 0, 1, 0, 0x10), kUNop},   // 00 iaddiu vi1, vi0, 0x10
            {lo(0x08, 0, 2, 0, 0x77), kUNop},   // 08 iaddiu vi2, vi0, 0x77
            {lo(0x05, X, 2, 0, 0x20), kUNop},   // 10 isw.x vi2, 0x20(vi0)
            {lox(0x6C, 0, 0, 1), kUNop},        // 18 xgkick vi1
            {kLNop, kUNop | (1u << 30)},        // 20 nop[e]
            {kLNop, kUNop},                     // 28
        };
        for (std::size_t i = 0; i < 6; ++i) {
            std::memcpy(m + i * 8, &prog[i].first, 4);
            std::memcpy(m + i * 8 + 4, &prog[i].second, 4);
        }
    }
};

std::string slurp(const char* path) {
    std::ifstream f(path);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

bool has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

void setTraceEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

}  // namespace

TEST_CASE(vu_trace, records_entry_memory_registers_and_decoded_gif_packet) {
    const char* out = "anyps2_vu_trace_test.txt";
    std::string text;
    {
    Rig t;
    VuTrace trace;
    VuTraceConfig cfg;
    cfg.out = out;
    cfg.kicks = {0x18};
    trace.configure(cfg);
    CHECK(trace.active());
    t.rt.vu1().setTrace(&trace);
    t.rt.vu1().interpret(0, 0x1234);
    t.rt.gs().processEvents(~std::uint64_t{0});
    CHECK_EQ(trace.runsRecorded(), 1u);
    }  // fecha o arquivo

    text = slurp(out);
    CHECK(has(text, "início 0x0000"));
    CHECK(has(text, "XGKICK em 0x0018"));
    CHECK(has(text, "EE em 0x00001234"));
    // A memória na entrada tem o pacote mas não o 0x77 que o microprograma gravou
    // no qword 0x20; a do XGKICK tem os dois.
    const std::size_t split = text.find("-- no XGKICK");
    CHECK(split != std::string::npos);
    const std::string entry = text.substr(0, split), atKick = text.substr(split);
    CHECK(has(entry, "qw 0x0010"));
    CHECK(!has(entry, "qw 0x0020"));
    CHECK(has(atKick, "qw 0x0020"));
    CHECK(has(atKick, "vi01=0x00000010"));
    CHECK(has(atKick, "vi02=0x00000077"));
    CHECK(has(entry, "vi01=0x00000000"));
    // Pacote decodificado
    CHECK(has(atKick, "NLOOP=1 EOP=1"));
    CHECK(has(atKick, "FLG=0 (PACKED) NREG=2 REGS=0x0000000000000051"));
    CHECK(has(atKick, "RGBAQ: R=17 G=256 (GS: 0, FORA DE 0..255) B=-1 (GS: 255, FORA DE 0..255) A=68"));
    CHECK(has(atKick, "XYZ2: X=6400 (400 px) Y=4800 (300 px) Z=4660 ADC=1"));
    std::remove(out);
}

TEST_CASE(vu_trace, filters_by_kick_address_vblank_range_and_program_limit) {
    const char* out = "anyps2_vu_trace_test2.txt";
    {  // XGKICK que não está na lista: nada gravado
        Rig t;
        VuTrace trace;
        VuTraceConfig cfg;
        cfg.out = out;
        cfg.kicks = {0x2A18};
        trace.configure(cfg);
        t.rt.vu1().setTrace(&trace);
        t.rt.vu1().interpret(0, 0);
        t.rt.gs().processEvents(~std::uint64_t{0});
        CHECK_EQ(trace.runsSeen(), 1u);
        CHECK_EQ(trace.runsRecorded(), 0u);
        CHECK(slurp(out).empty());
    }
    {  // VBlank 0 fora do intervalo 1..2
        Rig t;
        VuTrace trace;
        VuTraceConfig cfg;
        cfg.out = out;
        cfg.from = 1;
        cfg.to = 2;
        trace.configure(cfg);
        t.rt.vu1().setTrace(&trace);
        t.rt.vu1().interpret(0, 0);
        CHECK_EQ(trace.runsSeen(), 0u);
        CHECK(slurp(out).empty());
    }
    {  // limite de 1 microprograma; sem lista, qualquer XGKICK vale
        Rig t;
        VuTrace trace;
        VuTraceConfig cfg;
        cfg.out = out;
        cfg.maxPrograms = 1;
        trace.configure(cfg);
        t.rt.vu1().setTrace(&trace);
        t.rt.vu1().interpret(0, 0);
        t.rt.vu1().interpret(0, 0);
        t.rt.gs().processEvents(~std::uint64_t{0});
        CHECK_EQ(trace.runsSeen(), 2u);
        CHECK_EQ(trace.runsRecorded(), 1u);
        const std::string text = slurp(out);
        const std::size_t first = text.find("=== VU1 microprograma");
        CHECK(first != std::string::npos);
        CHECK(text.find("=== VU1 microprograma", first + 1) == std::string::npos);
    }
    std::remove(out);
}

TEST_CASE(vu_trace, from_env_parses_options) {
    const char* out = "anyps2_vu_trace_env_test.txt";
    setTraceEnv("ANYPS2_VU_TRACE", out);
    setTraceEnv("ANYPS2_VU_TRACE_FROM", "");
    setTraceEnv("ANYPS2_VU_TRACE_TO", "");
    setTraceEnv("ANYPS2_VU_TRACE_MAX", "");
    setTraceEnv("ANYPS2_VU_TRACE_KICK", "0x2A18,0x2A10");
    CHECK(VuTrace::fromEnv() != nullptr);
    setTraceEnv("ANYPS2_VU_TRACE_KICK", "lixo");
    CHECK(VuTrace::fromEnv() == nullptr);
    setTraceEnv("ANYPS2_VU_TRACE_KICK", "");
    CHECK(VuTrace::fromEnv() != nullptr);
    setTraceEnv("ANYPS2_VU_TRACE", "");
    CHECK(VuTrace::fromEnv() == nullptr);
    setTraceEnv("ANYPS2_VU_TRACE_KICK", "");
    std::remove(out);
}
