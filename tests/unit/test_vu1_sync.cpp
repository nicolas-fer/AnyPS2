// Medição "vu1 sync" (HostProfile/SyncStats, fatia 0 do plano do VU1 em outra thread): os
// contadores dos pontos de sincronia potenciais, dos lotes do VIF1 e dos eventos do GS
// aparecem com ANYPS2_PROFILE, e ligá-los não muda nada do que o programa observa.

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "anyps2/runtime/dmac.h"
#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/hardware.h"
#include "anyps2/runtime/host_profile.h"
#include "anyps2/runtime/ops.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/vif.h"
#include "anyps2/runtime/vu/vu.h"
#include "anyps2/runtime/vu1_stream.h"
#include "minitest.h"

using namespace anyps2::rt;

namespace {

constexpr std::uint32_t kLNop = 0x8000033Cu, kUNop = 0x000002FFu, kEnd = 1u << 30;
constexpr std::uint32_t XYZW = 15;

std::uint32_t lo(std::uint32_t op, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs, std::int32_t imm11) {
    return (op << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (static_cast<std::uint32_t>(imm11) & 0x7FF);
}
std::uint32_t lox(std::uint32_t idx, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs) {
    return (0x40u << 25) | (dest << 21) | (ft << 16) | (fs << 11) | ((idx >> 2) << 6) | 0x3C | (idx & 3);
}

// Tudo o que o programa de teste pode observar depois da sequência.
struct Observed {
    std::uint64_t csr = 0, siglblid = 0, imr = 0;
    std::uint32_t topSeen = 0, intcStat = 0, dmaStat = 0, gifStat = 0;
    std::uint64_t vu1Runs = 0, xgkicks = 0;
    bool operator==(const Observed&) const = default;
};

struct Rig {
    ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    RuntimeOptions options = [] {
        RuntimeOptions o;
        o.virtualClock = true;
        return o;
    }();
    Runtime rt{info, options};
    Context& c = rt.context();

    void program1(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& pairs) {
        std::uint8_t* m = rt.vu().micro1.get();
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            std::memcpy(m + i * 8, &pairs[i].first, 4);
            std::memcpy(m + i * 8 + 4, &pairs[i].second, 4);
        }
    }
    void ram(std::uint32_t addr, const void* src, std::size_t bytes) { std::memcpy(rt.memory().ram() + addr, src, bytes); }
    // Dispara o DMA normal (memória → dispositivo) do canal em `base`.
    void dma(std::uint32_t base, std::uint32_t madr, std::uint32_t qwc) {
        rt.dmac().write(base + 0x10, madr, 0);
        rt.dmac().write(base + 0x20, qwc, 0);
        rt.dmac().write(base, 0x101, 0);  // STR + DIR
    }
};

// A mesma sequência com o perfil ligado ou desligado.
Observed runScenario(Rig& t) {
    using namespace anyps2::rt::gs;
    // Pacote do PATH1 no qword 0x10 da memória do VU1: SIGNAL, LABEL e FINISH (A+D, EOP).
    const std::uint64_t pkt[8] = {3ull | (1ull << 15) | (1ull << 60), 0xE,
                                  0x1234ull | (0xFFFFull << 32), SIGNAL,
                                  0x55ull | (0xFFFFull << 32), LABEL,
                                  0, FINISH};
    std::memcpy(t.rt.vu().data1.get() + 0x10 * 16, pkt, sizeof(pkt));
    t.program1({
        {lox(0x68, 0, 2, 0), kUNop},       // xtop vi2
        {lo(0x08, 0, 1, 0, 0x10), kUNop},  // iaddiu vi1, vi0, 0x10
        {lox(0x6C, 0, 0, 1), kUNop},       // xgkick vi1
        {lo(0x05, 8, 2, 0, 0), kUNop},     // isw.x vi2, 0(vi0)
        {kLNop, kUNop | kEnd},
        {kLNop, kUNop},
        {kLNop, kUNop | kEnd},             // alvo do MSCNT (TPC depois da primeira execução)
        {kLNop, kUNop},
    });
    // Lote do VIF1 por DMA: BASE, OFFSET, MSCAL 0, MSCNT.
    const std::uint32_t vifcodes[4] = {0x03000100u, 0x02000080u, 0x14000000u, 0x17000000u};
    t.ram(0x1000, vifcodes, sizeof(vifcodes));
    t.dma(0x10009000, 0x1000, 1);
    // Lote do PATH3: A+D com um LABEL.
    const std::uint64_t p3[4] = {1ull | (1ull << 15) | (1ull << 60), 0xE, 0x77ull | (0xFFFFull << 32), LABEL};
    t.ram(0x2000, p3, sizeof(p3));
    t.dma(0x1000A000, 0x2000, 2);
    // Segundo lote do VIF1: ROW (STROW) com MODE 1 e leitura de ROW depois.
    const std::uint32_t row[8] = {0x05000001u, 0x30000000u, 1, 2, 3, 4, 0, 0};  // STMOD 1; STROW
    t.ram(0x3000, row, sizeof(row));
    t.dma(0x10009000, 0x3000, 2);
    t.rt.vif1().readRegister(0x10003D00, 0);
    // Pontos de fence do EE. As chamadas diretas ao GIF e ao GS abaixo não passam pelo Vu1Stream:
    // com o consumidor em outra thread (ANYPS2_VU1_THREAD=1) o teste faz o fence que o Hardware faria.
    t.rt.vu1Stream().fence();
    t.rt.gif().readRegister(0x10003020, 0);
    std::uint64_t scratch = 0;
    t.rt.hardware().read(0x12001000, &scratch, 8, 0);  // CSR (o resultado não importa)
    Observed o;
    o.siglblid = t.rt.gs().readPrivileged(0x12001080, 0);
    o.imr = t.rt.gs().readPrivileged(0x12001010, 0);
    t.rt.gs().readPrivileged(0x12000000, 0);
    ops::cfc2(&t.c, 29);  // VPU_STAT
    ops::cfc2(&t.c, 28);  // FBRST
    ops::cfc2(&t.c, 31);  // CMSAR1
    // VU0 lendo registradores do VU1 (0x4000): LQ vf1, 0x400(vi0) em modo macro.
    ops::vu0Macro(&t.c, lo(0x00, XYZW, 1, 0, 0x400), kUNop, 0x100);
    t.rt.gs().processEvents(~std::uint64_t{0});
    ops::ctc2(&t.c, 31, 0, 0);  // CMSAR1 inicia o VU1 de novo
    t.rt.vu1Stream().fence();
    t.rt.gs().processEvents(~std::uint64_t{0});
    o.csr = t.rt.gs().csr();
    std::memcpy(&o.topSeen, t.rt.vu().data1.get(), 4);
    o.vu1Runs = t.rt.vu1().interpretedRuns() + t.rt.vu1().compiledRuns();
    o.xgkicks = t.rt.gs().xgkicks();
    o.dmaStat = t.rt.dmac().stat();
    o.gifStat = t.rt.gif().readRegister(0x10003020, 0);
    ops::ctc2(&t.c, 28, 0x200, 0);  // FBRST: reseta o VU1
    return o;
}

// Liga o perfil só dentro do teste (ele é global e mexe em outros caminhos).
struct ProfileOn {
    ProfileOn() { HostProfile::enable(); }
    ~ProfileOn() { HostProfile::disable(); }
};

}  // namespace

TEST_CASE(vu1_sync, counters_appear_and_change_nothing) {
    Observed off;
    {
        Rig t;
        off = runScenario(t);
    }
    Observed on;
    SyncStats s;
    std::string line, report;
    bool threaded = false;
    {
        ProfileOn profile;
        Rig t;
        on = runScenario(t);
        threaded = t.rt.vu1Stream().threaded();
        t.rt.vu1Stream().fence();
        s = HostProfile::syncTotals();  // com o VU1 em outra thread, soma o que o consumidor contou
        line = HostProfile::syncInterval(1);
        report = HostProfile::syncReport(1);
    }
    CHECK(on == off);  // ligar a medição não muda nada do que o programa vê

    // Pontos de fence.
    CHECK_EQ(s.gifReads, 2ull);  // uma leitura explícita e a do fim
    CHECK_EQ(s.gsCsrReads, 1ull);
    CHECK_EQ(s.gsSiglblidReads, 1ull);
    CHECK_EQ(s.gsOtherPrivReads, 2ull);  // IMR e o registrador de modo (PMODE)
    CHECK_EQ(s.cfc2Vpustat, 1ull);
    CHECK_EQ(s.cfc2Fbrst, 1ull);
    CHECK_EQ(s.cfc2Cmsar1, 1ull);
    CHECK_EQ(s.ctc2Cmsar1, 1ull);
    CHECK_EQ(s.ctc2Fbrst, 1ull);
    CHECK_EQ(s.vu0Cross, 1ull);
    CHECK_EQ(s.rowReads, 1ull);
    CHECK_EQ(s.rowReadsMode, 1ull);
    CHECK_EQ(s.mscnt, 1ull);
    CHECK(!s.vu1MemCounted);  // sem ANYPS2_VU1_MEMCOUNT a memória do VU1 não é contada

    // Lotes: dois inícios do VIF1 (cada um fecha um lote) e um do GIF.
    CHECK_EQ(s.vif1Starts, 2ull);
    CHECK_EQ(s.batches, 2ull);
    CHECK_EQ(s.gifDmaStarts, 1ull);
    CHECK_EQ(s.mscalTotal, 2ull);  // MSCAL + MSCNT (o MSCNT conta como os dois)
    CHECK_EQ(s.batchMscal, 2ull);
    CHECK_EQ(s.batchMscalMax, 2ull);
    // O XGKICK do MSCAL; o do CTC2 veio fora de lote. Com o VU1 em outra thread o XGKICK é contado
    // só no total (o lote é do EE).
    if (!threaded) CHECK_EQ(s.batchXgkick, 1ull);
    CHECK_EQ(s.xgkickTotal, 2ull);
    std::uint64_t gaps = 0, hosts = 0;
    for (unsigned i = 0; i < SyncStats::kBuckets; ++i) {
        gaps += s.gapHist[i];
        hosts += s.hostHist[i];
    }
    CHECK_EQ(gaps, 1ull);  // distância entre o primeiro e o segundo início
    CHECK_EQ(hosts, 2ull);

    // Eventos do GS: SIGNAL/LABEL/FINISH pelo PATH1, LABEL pelo PATH3 (origem 3), duas vezes o
    // do PATH1 porque o CTC2 reexecuta o microprograma.
    CHECK_EQ(s.evWrites[SyncStats::EvSignal][1], 2ull);
    CHECK_EQ(s.evWrites[SyncStats::EvFinish][1], 2ull);
    CHECK_EQ(s.evWrites[SyncStats::EvLabel][1], 2ull);
    CHECK_EQ(s.evWrites[SyncStats::EvLabel][3], 1ull);
    // O primeiro SIGNAL liga o bit (interrupção ou só bit, conforme o IMR inicial); o segundo já o encontra ligado.
    CHECK_EQ(s.evRaises[SyncStats::RaiseSignal][SyncStats::OutInterrupt] +
                 s.evRaises[SyncStats::RaiseSignal][SyncStats::OutCsrOnly],
             1ull);
    CHECK_EQ(s.evRaises[SyncStats::RaiseSignal][SyncStats::OutAlreadySet], 1ull);
    CHECK_EQ(s.evRaises[SyncStats::RaiseFinish][SyncStats::OutInterrupt] +
                 s.evRaises[SyncStats::RaiseFinish][SyncStats::OutCsrOnly],
             1ull);

    // O relatório sai em uma linha só, com o prefixo combinado.
    CHECK(line.rfind("[perfil] vu1 sync: ", 0) == 0);
    CHECK(report.rfind("[perfil] vu1 sync: ", 0) == 0);
    CHECK_EQ(std::count(line.begin(), line.end(), '\n'), 1);
    CHECK(line.find("pontos de fence") != std::string::npos);
    CHECK(line.find("distância entre inícios do VIF1") != std::string::npos);
    CHECK(line.find("eventos do GS") != std::string::npos);
}

TEST_CASE(vu1_sync, disabled_profile_counts_nothing) {
    // Os contadores são globais e guardam o que o teste anterior contou: o que importa é que
    // a mesma sequência, sem o perfil, não some nada.
    const SyncStats before = HostProfile::sync();
    Rig t;
    runScenario(t);
    const SyncStats& s = HostProfile::sync();
    CHECK_EQ(s.gifReads, before.gifReads);
    CHECK_EQ(s.vif1Starts, before.vif1Starts);
    CHECK_EQ(s.batches, before.batches);
    CHECK_EQ(s.evWrites[SyncStats::EvSignal][1], before.evWrites[SyncStats::EvSignal][1]);
    CHECK(HostProfile::syncInterval(1).empty());
}
