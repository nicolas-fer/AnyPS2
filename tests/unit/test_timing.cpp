// Testes do relógio do EE, timers T0–T3, VBlank e alarmes (Timing), no modo
// virtual (determinístico).

#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/memory.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/timing.h"
#include "minitest.h"

using namespace anyps2::rt;

namespace {
constexpr unsigned kCount = 0, kMode = 1, kComp = 2;
constexpr std::uint32_t CUE = 1u << 7, CMPE = 1u << 8, OVFE = 1u << 9, EQUF = 1u << 10,
                        OVFF = 1u << 11, ZRET = 1u << 6;

struct VirtualRuntime {
    ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    RuntimeOptions options = [] {
        RuntimeOptions o;
        o.virtualClock = true;
        return o;
    }();
    Runtime rt{info, options};
    Timing& t = rt.timing();
};
}  // namespace

TEST_CASE(timing, virtual_clock_advances_only_when_told) {
    VirtualRuntime v;
    CHECK_EQ(v.t.mode(), Timing::Mode::Virtual);
    CHECK_EQ(v.t.now(), 0ull);
    v.t.consume(1000);
    CHECK_EQ(v.t.now(), 1000ull);
    v.t.consume(-5);  // ignora valores negativos
    CHECK_EQ(v.t.now(), 1000ull);
}

TEST_CASE(timing, timer_counts_with_prescaler) {
    VirtualRuntime v;
    v.t.writeTimer(0, kMode, CUE);  // BUSCLK = ciclos/2
    v.t.consume(1000);
    CHECK_EQ(v.t.readTimer(0, kCount), 500u);
    v.t.writeTimer(1, kMode, CUE | 1);  // BUSCLK/16 = ciclos/32
    v.t.consume(3200);
    CHECK_EQ(v.t.readTimer(1, kCount), 100u);
    v.t.writeTimer(1, kCount, 7);
    CHECK_EQ(v.t.readTimer(1, kCount), 7u);
    v.t.writeTimer(2, kMode, 0);  // sem CUE não conta
    v.t.consume(10000);
    CHECK_EQ(v.t.readTimer(2, kCount), 0u);
}

TEST_CASE(timing, compare_interrupt_flags_and_zret) {
    VirtualRuntime v;
    Kernel& k = v.rt.kernel();
    v.t.writeTimer(2, kComp, 100);
    v.t.writeTimer(2, kMode, CUE | CMPE | ZRET);
    v.t.consume(199);  // 99 ticks
    v.t.process(0);
    CHECK_EQ(k.intcStat() & (1u << 11), 0u);
    v.t.consume(2);  // 100º tick
    v.t.process(0);
    CHECK((k.intcStat() & (1u << 11)) != 0);         // INTC causa 11 = TIMER2
    CHECK((v.t.readTimer(2, kMode) & EQUF) != 0);
    CHECK_EQ(v.t.readTimer(2, kCount), 0u);           // ZRET zerou
    // EQUF continua ligado até escrever 1; escrever 0 não limpa
    const std::uint32_t mode = v.t.readTimer(2, kMode);
    v.t.writeTimer(2, kMode, mode & ~EQUF);
    CHECK((v.t.readTimer(2, kMode) & EQUF) != 0);
    v.t.writeTimer(2, kMode, mode);  // escreve 1 em EQUF: limpa
    CHECK_EQ(v.t.readTimer(2, kMode) & EQUF, 0u);
    // próximo evento: mais 100 ticks
    CHECK_EQ(v.t.cyclesUntilNextEvent() <= 200ull, true);
}

TEST_CASE(timing, overflow_flag) {
    VirtualRuntime v;
    Kernel& k = v.rt.kernel();
    v.t.writeTimer(3, kCount, 0xFFF0);
    v.t.writeTimer(3, kMode, CUE | OVFE);
    v.t.consume(2 * 0x20);  // 32 ticks: passa de 0xFFFF
    v.t.process(0);
    CHECK((k.intcStat() & (1u << 12)) != 0);
    CHECK((v.t.readTimer(3, kMode) & OVFF) != 0);
    CHECK_EQ(v.t.readTimer(3, kCount), 0x10u);
}

TEST_CASE(timing, vblank_and_gs_csr) {
    VirtualRuntime v;
    Kernel& k = v.rt.kernel();
    CHECK_EQ(v.t.readGsCsr() & 8, 0ull);
    CHECK_EQ(v.t.cyclesUntilNextEvent(), Timing::kCyclesPerField);
    CHECK(v.t.advanceToNextEvent(0));  // pula para o primeiro VBlank
    CHECK_EQ(v.t.vblankCount(), 1ull);
    CHECK((k.intcStat() & (1u << 2)) != 0);  // VBLANK_S
    const std::uint64_t csr = v.t.readGsCsr();
    CHECK((csr & 8) != 0);       // VSINT
    CHECK((csr & 0x2000) != 0);  // FIELD alternou
    v.t.writeGsCsr(8);
    CHECK_EQ(v.t.readGsCsr() & 8, 0ull);
    CHECK(v.t.advanceToNextEvent(0));  // VBlank end
    CHECK((k.intcStat() & (1u << 3)) != 0);
    // SetVSyncFlag: o kernel escreve 1 no flag a cada VBlank
    v.rt.memory().write<std::uint32_t>(0x00100000, 0, 0);
    v.t.setVSyncFlag(0x00100000, 0x00100008);
    v.t.consume(static_cast<std::int64_t>(Timing::kCyclesPerField));
    v.t.process(0);
    CHECK_EQ(v.rt.memory().read<std::uint32_t>(0x00100000, 0), 1u);
    CHECK_EQ(v.t.vblankCount(), 2ull);
}

TEST_CASE(timing, alarms) {
    VirtualRuntime v;
    const std::int32_t a = v.t.setAlarm(10, 0x00100000, 1, 0);
    const std::int32_t b = v.t.setAlarm(5, 0x00100000, 2, 0);
    CHECK(a > 0 && b > 0 && a != b);
    CHECK_EQ(v.t.cyclesUntilNextEvent(), 5 * Timing::kCyclesPerLine);
    CHECK(v.t.releaseAlarm(b));
    CHECK(!v.t.releaseAlarm(b));
    CHECK_EQ(v.t.cyclesUntilNextEvent(), 10 * Timing::kCyclesPerLine);
    for (int i = 0; i < 64; ++i) v.t.setAlarm(1000, 0x00100000, 0, 0);
    CHECK_EQ(v.t.setAlarm(1, 0x00100000, 0, 0), -1);  // limite de 64 alarmes
}

TEST_CASE(timing, real_clock_moves) {
    ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    Runtime rt(info, RuntimeOptions{});
    CHECK_EQ(rt.timing().mode(), Timing::Mode::Real);
    const auto a = rt.timing().now();
    volatile unsigned spin = 0;
    for (unsigned i = 0; i < 2000000; ++i) spin = spin + i;
    CHECK(rt.timing().now() > a);
}
