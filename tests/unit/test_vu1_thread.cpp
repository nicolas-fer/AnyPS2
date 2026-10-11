// VU1/GIF/GS numa thread de verdade (fatia 5 do plano do VU1 em outra thread, W = 0): o resultado
// com a thread consumidora (ANYPS2_VU1_THREAD=1) é idêntico ao do consumidor inline em tudo o
// que o programa observa, não importa o escalonamento.
//
// O diferencial roda o MESMO programa de operações do EE (DMA do GIF e do VIF1 com UNPACK/MPG/
// MSCAL/DIRECT, desenhos e SIGNAL/LABEL/FINISH, safepoints que entregam o INTC, leituras e
// escritas do CSR/IMR/INTC_STAT, CFC2, VU0 tocando o VU1, VBlank) sorteado por semente, uma vez
// com o consumidor inline (a referência) e várias com a thread: jitter aleatório com semente no
// consumidor (ceder a CPU, dormir), anel minúsculo (o EE bloqueia, nunca descarta), fence forçado
// em todo item e 1, 2 e 4 faixas do GS. Compara cada observação feita no meio do programa (CSR,
// INTC_STAT depois de cada safepoint, ...) e o estado final inteiro (VRAM, registradores do GS,
// memórias e registradores do VU1, espelho do CSR/IMR/SIGLBLID, FINISH pendentes).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "anyps2/runtime/context.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/host_profile.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/memory.h"
#include "anyps2/runtime/ops.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/timing.h"
#include "anyps2/runtime/vif.h"
#include "anyps2/runtime/vu/vu.h"
#include "anyps2/runtime/vu1_stream.h"
#include "minitest.h"

using namespace anyps2::rt;
using namespace anyps2::rt::gs;

namespace {

constexpr std::uint32_t kLNop = 0x8000033Cu, kUNop = 0x000002FFu, kEnd = 1u << 30;

std::uint32_t lo(std::uint32_t op, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs, std::int32_t imm11) {
    return (op << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (static_cast<std::uint32_t>(imm11) & 0x7FF);
}
std::uint32_t lox(std::uint32_t idx, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs) {
    return (0x40u << 25) | (dest << 21) | (ft << 16) | (fs << 11) | ((idx >> 2) << 6) | 0x3C | (idx & 3);
}

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {
        for (int i = 0; i < 4; ++i) next();
    }
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>((next() >> 11) % n); }
};

std::uint64_t fnv(const void* data, std::size_t n, std::uint64_t h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

struct Cfg {
    bool thread = false;
    std::uint64_t jitter = 0;
    bool forceFence = false;
    std::size_t ring = 0;  // 0 = padrão
    unsigned lanes = 0;  // faixas do GS (ANYPS2_GS_THREADS); 0 = o do ambiente
};

// Restaura uma variável de ambiente ao sair do teste.
struct EnvGuard {
    std::string name, old;
    bool had = false;
    explicit EnvGuard(const char* n) : name(n) {
        if (const char* v = std::getenv(n)) {
            had = true;
            old = v;
        }
    }
    ~EnvGuard() {
#ifdef _WIN32
        _putenv_s(name.c_str(), had ? old.c_str() : "");
#else
        if (had) setenv(name.c_str(), old.c_str(), 1);
        else unsetenv(name.c_str());
#endif
    }
};

struct Rig {
    ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    RuntimeOptions options = [] {
        RuntimeOptions o;
        o.virtualClock = true;
        return o;
    }();
    std::unique_ptr<Runtime> rtp;
    Runtime& rt;
    Context& c;

    static Runtime* make(const ProgramInfo& info, const RuntimeOptions& o, unsigned lanes) {
        if (lanes) setEnv("ANYPS2_GS_THREADS", std::to_string(lanes).c_str());
        return new Runtime(info, o);
    }
    explicit Rig(const Cfg& cfg = {})
        : rtp(make(info, options, cfg.lanes)), rt(*rtp), c(rt.context()) {
        Vu1Stream& s = rt.vu1Stream();
        s.setThreaded(false);  // o ambiente pode ter ANYPS2_VU1_THREAD: o modo é o da configuração
        if (cfg.ring) s.setRingSlots(cfg.ring);
        s.setJitterSeed(cfg.jitter);
        s.setForceFence(cfg.forceFence);
        if (cfg.thread) s.setThreaded(true);
    }
};

// ---------------------------------------------------------------------------
// O programa de operações do EE
// ---------------------------------------------------------------------------

using Qw = std::vector<std::uint64_t>;

// GIFtag A+D com `n` registradores e as entradas (dado, registrador) em seguida.
struct Packet {
    Qw q;
    unsigned n = 0;
    bool eop = true;
    void ad(std::uint64_t value, std::uint8_t reg) {
        q.push_back(value);
        q.push_back(reg);
        ++n;
    }
    // Entradas já adicionadas; coloca a tag na frente.
    Qw finish() const {
        Qw out;
        out.push_back(n | (std::uint64_t{eop} << 15) | (std::uint64_t{1} << 60));
        out.push_back(0xE);
        out.insert(out.end(), q.begin(), q.end());
        return out;
    }
};

void addSprite(Packet& p, Rng& g) {
    const std::uint64_t x1 = g.below(560), y1 = g.below(420), w = 4 + g.below(80), h = 4 + g.below(60);
    p.ad(10ull << 16, FRAME_1);  // FBP 0, FBW 10, PSMCT32
    p.ad(639ull << 16 | 479ull << 48, SCISSOR_1);
    p.ad(g.below(2) ? 0x46ull : 0x6ull, PRIM);  // sprite, às vezes com ABE
    p.ad(g.below(256) | g.below(256) << 8 | g.below(256) << 16 | (0x40ull + g.below(0x40)) << 24, RGBAQ);
    p.ad((x1 << 4) | (y1 << 20), XYZ2);
    p.ad(((x1 + w) << 4) | ((y1 + h) << 20), XYZ2);
}

Qw drawPacket(Rng& g, unsigned sprites, bool eop = true) {
    Packet p;
    p.eop = eop;
    p.ad(0x44ull, ALPHA_1);
    for (unsigned i = 0; i < sprites; ++i) addSprite(p, g);
    return p.finish();
}

Qw eventPacket(Rng& g) {
    Packet p;
    if (g.below(3)) p.ad(g.below(0x10000) | (0xFFFFull << 32), SIGNAL);
    if (g.below(3)) p.ad((std::uint64_t{g.below(0x10000)} << 0) | (0xFFFFull << 32), LABEL);
    if (g.below(2)) addSprite(p, g);
    p.ad(0, FINISH);
    return p.finish();
}

struct Observed {
    std::vector<std::uint64_t> trace;
    bool operator==(const Observed& o) const = default;
};

constexpr std::uint32_t kGifRam = 0x20000, kVifRam = 0x10000;

struct Scenario {
    Rig& r;
    Rng g;
    Observed out;
    explicit Scenario(Rig& rig, std::uint64_t seed) : r(rig), g(seed) {}

    void obs(std::uint64_t v) { out.trace.push_back(v); }
    Memory& mem() { return r.rt.memory(); }

    void put(std::uint32_t addr, const void* src, std::size_t bytes) {
        std::memcpy(r.rt.memory().ram() + addr, src, bytes);
    }
    void dma(std::uint32_t base, std::uint32_t madr, std::uint32_t qwc) {
        mem().write<std::uint32_t>(base + 0x10, madr, 0);
        mem().write<std::uint32_t>(base + 0x20, qwc, 0);
        mem().write<std::uint32_t>(base, 0x101, 0);  // STR + DIR
    }
    void gifDma(const Qw& q) {
        put(kGifRam, q.data(), q.size() * 8);
        dma(0x1000A000, kGifRam, static_cast<std::uint32_t>(q.size() / 2));
    }
    // Uma transferência do VIF1: palavras (VIFcodes e dados), completada com NOP até quadword.
    void vifDma(std::vector<std::uint32_t> w) {
        while (w.size() % 4) w.push_back(0);
        put(kVifRam, w.data(), w.size() * 4);
        dma(0x10009000, kVifRam, static_cast<std::uint32_t>(w.size() / 4));
    }
    static void append(std::vector<std::uint32_t>& w, const Qw& q) {
        for (const std::uint64_t v : q) {
            w.push_back(static_cast<std::uint32_t>(v));
            w.push_back(static_cast<std::uint32_t>(v >> 32));
        }
    }

    void setup() {
        // Microprograma do VU1 por MPG: lê TOP, aponta vi1 para o pacote no qword 0x10 da memória
        // de dados e o envia (XGKICK); guarda o TOP em data1[0].x.
        const std::uint32_t prog[12] = {lox(0x68, 0, 2, 0), kUNop,        lo(0x08, 0, 1, 0, 0x10), kUNop,
                                        lox(0x6C, 0, 0, 1), kUNop,        lo(0x05, 8, 2, 0, 0),    kUNop,
                                        kLNop,              kUNop | kEnd, kLNop,                   kUNop};
        std::vector<std::uint32_t> w;
        w.push_back(0x4A000000u | (6u << 16));  // MPG num=6 (6 pares), endereço 0
        w.insert(w.end(), prog, prog + 12);
        vifDma(w);
        // O pacote que o microprograma envia (qword 0x10) tem de ser sempre válido: o CTC2 CMSAR1
        // do sorteio pode iniciar o VU1 antes do primeiro UNPACK.
        w.clear();
        const Qw first = drawPacket(g, 1);
        w.push_back(0x6C000000u | (static_cast<std::uint32_t>(first.size() / 2) << 16) | 0x10u);
        append(w, first);
        vifDma(w);
        // Interrupções do EE ligadas e o INTC inteiro desmascarado: o safepoint entrega (limpa) as causas.
        r.c.cop0[cop0::Status] = 0x70030C13u;
        mem().write<std::uint32_t>(0x1000F010, 0xFFFFu, 0);
        r.rt.vu1Stream().setImr(0);  // IMR do GS desmascarado: SIGNAL/FINISH/VSINT geram INTC_GS
        r.rt.vu1Stream().fence();
    }

    void op() {
        const std::uint32_t k = g.below(100);
        if (k < 24) {
            gifDma(drawPacket(g, 1 + g.below(3)));
        } else if (k < 34) {
            gifDma(eventPacket(g));
        } else if (k < 54) {
            // UNPACK do pacote no qword 0x10 + MSCAL/MSCNT
            const Qw pkt = g.below(3) ? drawPacket(g, 1 + g.below(2)) : eventPacket(g);
            std::vector<std::uint32_t> w;
            w.push_back(0x6C000000u | (static_cast<std::uint32_t>(pkt.size() / 2) << 16) | 0x10u);
            append(w, pkt);
            w.push_back(g.below(4) ? 0x14000000u : 0x17000000u);
            vifDma(w);
        } else if (k < 62) {
            // DIRECT: um pacote completo pelo PATH2
            const Qw pkt = drawPacket(g, 1 + g.below(2));
            std::vector<std::uint32_t> w;
            w.push_back(0x50000000u | static_cast<std::uint32_t>(pkt.size() / 2));
            append(w, pkt);
            vifDma(w);
        } else if (k < 73) {
            // Safepoint: avança o relógio, VBlank, eventos do GS e entrega (e limpa) o INTC.
            if (g.below(3) == 0) {
                // EI: entrega o INTC que já valia, sem avançar o relógio nem passar pelo Timing.
                r.c.pc = 0x1000;
                ops::onInterruptsEnabled(&r.c);
            } else {
                r.rt.safepoint(&r.c, 0x1000 + g.below(64) * 4, static_cast<std::int64_t>(1 + g.below(1200000)));
            }
            obs(r.rt.kernel().intcStat());
            obs(r.rt.timing().now());
            obs(r.rt.vblanks());
        } else if (k < 79) {
            obs(mem().read<std::uint64_t>(0x12001000, 0));  // CSR
        } else if (k < 83) {
            mem().write<std::uint64_t>(0x12001000, g.below(0x20), 0);  // CSR: escrever 1 limpa
        } else if (k < 86) {
            mem().write<std::uint64_t>(0x12001010, g.below(0x8000) & 0x7F00, 0);  // IMR
            obs(mem().read<std::uint64_t>(0x12001080, 0));                         // SIGLBLID
        } else if (k < 89) {
            obs(mem().read<std::uint32_t>(0x1000F000, 0));  // INTC_STAT
            mem().write<std::uint32_t>(0x1000F000, g.below(0x10000), 0);
        } else if (k < 92) {
            obs(mem().read<std::uint32_t>(0x10003020, 0));  // GIF_STAT
            obs(ops::cfc2(&r.c, 29));                        // VPU_STAT
        } else if (k < 94) {
            ops::ctc2(&r.c, 31, 0, 0x2000);  // CMSAR1 inicia o VU1
        } else if (k < 96) {
            r.rt.timing().consume(g.below(30000));
        } else if (k < 97) {
            ops::vu0Macro(&r.c, lo(0x00, 15, 1, 0, 0x400), kUNop, 0x100);  // VU0 lê os registradores do VU1
            obs(r.c.vf[1].uw[0]);
        } else if (k < 98) {
            ops::ctc2(&r.c, 28, 0x200, 0x2000);  // FBRST: reseta o VU1
        } else {
            r.rt.vu1Stream().fence();
            const Frame f = r.rt.gs().display();
            obs(fnv(f.pixels.data(), f.pixels.size() * sizeof(f.pixels[0])));
        }
        r.rt.timing().consume(g.below(2000));
    }
};

// Estado final inteiro, depois de um fence.
void finalState(Rig& r, Observed& o) {
    r.rt.vu1Stream().fence();
    Gs& gsRef = r.rt.gs();
    o.trace.push_back(fnv(gsRef.vram().data(), Vram::kSize));
    std::uint64_t regs = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < 0x63; ++i) {
        const std::uint64_t v = gsRef.reg(static_cast<std::uint8_t>(i));
        regs = fnv(&v, sizeof v, regs);
    }
    o.trace.push_back(regs);
    const VuMemory& vu = r.rt.vu();
    o.trace.push_back(fnv(vu.data1.get(), VuMemory::kVu1Size));
    o.trace.push_back(fnv(vu.micro1.get(), VuMemory::kVu1Size));
    o.trace.push_back(fnv(vu.data0.get(), VuMemory::kVu0Size));
    o.trace.push_back(fnv(r.rt.vu1().regs().vi, 32 * sizeof(std::uint32_t)));
    o.trace.push_back(fnv(r.rt.vu1().regs().vf, 32 * sizeof(Reg128)));
    o.trace.push_back(r.rt.vu1().compiledRuns() + r.rt.vu1().interpretedRuns());
    GsEeState& ee = r.rt.vu1Stream().eeState();
    o.trace.push_back(ee.csr(r.rt.timing().now()));
    o.trace.push_back(ee.imr());
    o.trace.push_back(ee.siglblid());
    o.trace.push_back(ee.pendingFinishes());
    o.trace.push_back(ee.nextEventTime());
    o.trace.push_back(r.rt.kernel().intcStat());
    o.trace.push_back(gsRef.drawCount());
    o.trace.push_back(gsRef.xgkicks());
    o.trace.push_back(r.rt.gif().path3Masked());
    o.trace.push_back(r.rt.vu1Stream().path3Masked());
    o.trace.push_back(r.rt.vblanks());
}

Observed runScenario(const Cfg& cfg, std::uint64_t seed, unsigned ops) {
    Rig r(cfg);
    Scenario s(r, seed);
    try {
        s.setup();
        for (unsigned i = 0; i < ops; ++i) s.op();
    } catch (const std::exception& e) {
        if (std::getenv("ANYPS2_TEST_VERBOSE")) {
            std::fprintf(stderr, "[vu1_thread] semente %llu: %s\n", static_cast<unsigned long long>(seed), e.what());
        }
        s.obs(fnv(e.what(), std::strlen(e.what())));
    }
    finalState(r, s.out);
    return s.out;
}

std::string firstDiff(const Observed& a, const Observed& b) {
    if (a.trace.size() != b.trace.size()) {
        return "tamanhos " + std::to_string(a.trace.size()) + " != " + std::to_string(b.trace.size());
    }
    for (std::size_t i = 0; i < a.trace.size(); ++i) {
        if (a.trace[i] != b.trace[i]) {
            return "observação " + std::to_string(i) + " de " + std::to_string(a.trace.size()) + ": " +
                   std::to_string(a.trace[i]) + " != " + std::to_string(b.trace[i]);
        }
    }
    return "";
}

}  // namespace

// O diferencial principal: 50 sementes, cada uma contra a referência inline, com jitter, anel
// minúsculo, fence forçado e 1, 2 e 4 faixas do GS.
TEST_CASE(vu1_thread, differential_against_inline_with_jitter) {
    constexpr unsigned kSeeds = 50, kOps = 90;
    const EnvGuard lanesEnv("ANYPS2_GS_THREADS");
    std::uint64_t checks = 0;
    for (unsigned seed = 1; seed <= kSeeds; ++seed) {
        const Observed ref = runScenario(Cfg{}, seed, kOps);
        struct Variant {
            const char* name;
            Cfg cfg;
        };
        const Variant variants[] = {
            {"jitter", Cfg{true, seed * 31ull + 1, false, 0, 1}},
            {"anel de 4 + jitter, 2 faixas", Cfg{true, seed * 17ull + 5, false, 4, 2}},
            {"fence forçado, 4 faixas", Cfg{true, 0, true, 64, 4}},
            {"anel de 2, 4 faixas", Cfg{true, seed * 13ull + 3, false, 2, 4}},
        };
        for (const Variant& v : variants) {
            const Observed got = runScenario(v.cfg, seed, kOps);
            const std::string d = firstDiff(got, ref);
            if (!d.empty()) {
                minitest::reportFailure(__FILE__, __LINE__,
                                        "semente " + std::to_string(seed) + " (" + v.name + "): " + d);
                return;
            }
            ++checks;
        }
        // A referência tem de ter exercitado de verdade o que se compara.
        if (seed == 1) {
            CHECK(ref.trace.size() > 40);
        }
    }
    CHECK_EQ(checks, std::uint64_t{kSeeds} * 4);
}

// O cenário desenha e gera eventos de verdade (senão o diferencial compararia o nada).
TEST_CASE(vu1_thread, scenario_exercises_draws_events_and_interrupts) {
    Rig r;
    Scenario s(r, 7);
    s.setup();
    for (unsigned i = 0; i < 600; ++i) s.op();
    r.rt.vu1Stream().fence();
    CHECK(r.rt.gs().drawCount() > 40);
    CHECK(r.rt.gs().xgkicks() > 5);
    CHECK(r.rt.vblanks() >= 2);
    CHECK(r.rt.vu1().compiledRuns() + r.rt.vu1().interpretedRuns() > 5);
    bool sawIntc = false;
    for (std::size_t i = 0; i + 2 < s.out.trace.size(); ++i) sawIntc |= (s.out.trace[i] & 1) != 0;
    CHECK(sawIntc);
}

// Anel cheio: o EE bloqueia, nunca descarta. 3000 desenhos por um anel de 4 slots.
TEST_CASE(vu1_thread, full_ring_blocks_and_never_drops) {
    Cfg cfg;
    cfg.thread = true;
    cfg.ring = 4;
    cfg.jitter = 99;
    Rig t(cfg);
    Rig ref;
    constexpr unsigned kN = 3000;
    Rng g1(5), g2(5);
    for (unsigned i = 0; i < kN; ++i) {
        const Qw a = drawPacket(g1, 1), b = drawPacket(g2, 1);
        t.rt.vu1Stream().pushPath3(reinterpret_cast<const std::uint8_t*>(a.data()), a.size() / 2, 0x100);
        ref.rt.vu1Stream().pushPath3(reinterpret_cast<const std::uint8_t*>(b.data()), b.size() / 2, 0x100);
    }
    t.rt.vu1Stream().fence();
    ref.rt.vu1Stream().fence();
    CHECK_EQ(t.rt.gs().drawCount(), std::uint64_t{kN});
    CHECK_EQ(t.rt.gs().drawCount(), ref.rt.gs().drawCount());
    CHECK_EQ(fnv(t.rt.gs().vram().data(), Vram::kSize), fnv(ref.rt.gs().vram().data(), Vram::kSize));
    CHECK(t.rt.vu1Stream().stats().ringFull > 100);
    CHECK_EQ(t.rt.vu1Stream().processedSeq(), t.rt.vu1Stream().pushedSeq());
    CHECK_EQ(t.rt.vu1Stream().queued(), 0u);
}

// Os dados são COPIADOS para o anel: o EE reescreve a memória logo depois do push.
TEST_CASE(vu1_thread, data_is_copied_into_the_ring) {
    Cfg cfg;
    cfg.thread = true;
    cfg.jitter = 3;
    Rig t(cfg);
    for (unsigned i = 0; i < 100; ++i) {
        Packet p;
        p.ad(0x1000ull + i, ALPHA_1);
        p.ad(0x2000ull + i, TEST_1);
        Qw q = p.finish();
        t.rt.vu1Stream().pushPath3(reinterpret_cast<const std::uint8_t*>(q.data()), q.size() / 2, 0);
        std::memset(q.data(), 0xEE, q.size() * 8);  // o DMA reaproveita a memória
        // UNPACK + MPG + DIRECT: os buffers do decodificador também são reaproveitados
        std::uint8_t buf[16];
        Packet d;
        d.ad(0x30ull + i, FOG);
        const Qw dq = d.finish();
        VifItem it{VifItem::Kind::Direct};
        it.data = buf;
        std::memcpy(buf, dq.data(), 16);
        t.rt.vu1Stream().pushVif(it);
        std::memcpy(buf, dq.data() + 2, 16);
        t.rt.vu1Stream().pushVif(it);
        std::memset(buf, 0, sizeof buf);
        t.rt.vu1Stream().fence();
        CHECK_EQ(t.rt.gs().reg(ALPHA_1), 0x1000ull + i);
        CHECK_EQ(t.rt.gs().reg(TEST_1), 0x2000ull + i);
        CHECK_EQ(t.rt.gs().reg(FOG), 0x30ull + i);
    }
}

// Erros do consumidor em outra thread: a mesma classe, mensagem e pc do inline, relançados no
// próximo fence ou enfileiramento; o resto é pulado; o stream continua utilizável.
TEST_CASE(vu1_thread, consumer_errors_are_rethrown_in_the_ee) {
    std::string expected;
    std::uint32_t expectedPc = 0;
    {
        Rig ref;
        ref.rt.vu1Stream().setThreaded(false);
        std::uint64_t pkt[4] = {1 | (std::uint64_t{1} << 15) | (std::uint64_t{1} << 60), 0xB, 0, 0};
        try {
            ref.rt.vu1Stream().pushPath3(reinterpret_cast<const std::uint8_t*>(pkt), 2, 0xABC);
        } catch (const Unimplemented& e) {
            expected = e.what();
            expectedPc = e.pc();
        }
    }
    CHECK(!expected.empty());

    std::uint64_t bad[4] = {1 | (std::uint64_t{1} << 15) | (std::uint64_t{1} << 60), 0xB, 0, 0};
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(bad);
    for (unsigned mode = 0; mode < 3; ++mode) {
        Cfg cfg;
        cfg.thread = true;
        cfg.jitter = mode == 1 ? 11 : 0;
        cfg.ring = mode == 2 ? 2 : 0;
        Rig t(cfg);
        Vu1Stream& s = t.rt.vu1Stream();
        std::string got;
        std::uint32_t pc = 0;
        try {
            s.pushPath3(bytes, 2, 0xABC);
            // Itens depois do que falha: não rodam. Sem fence, o enfileiramento relança (o
            // consumidor pode demorar a falhar, então enfileira até o erro subir).
            Rng lg(1);
            const Qw later = drawPacket(lg, 1);
            for (unsigned i = 0; i < 100000; ++i) {
                s.pushPath3(reinterpret_cast<const std::uint8_t*>(later.data()), later.size() / 2, 0x500);
                if (i % 64 == 0) std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
            s.fence();
        } catch (const Unimplemented& e) {
            got = e.what();
            pc = e.pc();
        }
        CHECK_EQ(got, expected);
        CHECK_EQ(pc, expectedPc);
        CHECK(!s.hasPendingError());
        // Utilizável de novo: o GIF parou no meio do pacote (reset) e um pacote válido passa.
        s.pushGifReset();
        Packet ok;
        ok.ad(0x77, TEST_1);
        const Qw q = ok.finish();
        s.pushPath3(reinterpret_cast<const std::uint8_t*>(q.data()), q.size() / 2, 0);
        s.fence();
        CHECK_EQ(t.rt.gs().reg(TEST_1), 0x77u);
    }
}

// Encerramento com a thread viva e itens no anel (e depois de um erro): não trava, não usa
// objetos destruídos. O shutdown explícito devolve o stream ao inline.
TEST_CASE(vu1_thread, shutdown_with_pending_items_does_not_hang) {
    for (unsigned round = 0; round < 12; ++round) {
        Cfg cfg;
        cfg.thread = true;
        cfg.jitter = round % 3 ? round : 0;
        cfg.ring = round % 4 == 0 ? 8 : 0;
        Rig t(cfg);
        Rng g(round + 1);
        for (unsigned i = 0; i < 400; ++i) {
            const Qw q = drawPacket(g, 2);
            t.rt.vu1Stream().pushPath3(reinterpret_cast<const std::uint8_t*>(q.data()), q.size() / 2, 0);
        }
        if (round % 3 == 2) {
            std::uint64_t bad[4] = {1 | (std::uint64_t{1} << 15) | (std::uint64_t{1} << 60), 0xB, 0, 0};
            try {
                t.rt.vu1Stream().pushPath3(reinterpret_cast<const std::uint8_t*>(bad), 2, 0);
            } catch (const Unimplemented&) {
            }
        }
        if (round % 2) {
            t.rt.vu1Stream().shutdown();
            CHECK(!t.rt.vu1Stream().threaded());
            CHECK(t.rt.vu1Stream().inlineMode());
            // De volta ao inline: a próxima entrega vale na hora.
            t.rt.vu1Stream().pushGifReset();
            Packet ok;
            ok.ad(0x55, TEST_1);
            const Qw q = ok.finish();
            t.rt.vu1Stream().pushPath3(reinterpret_cast<const std::uint8_t*>(q.data()), q.size() / 2, 0);
            CHECK_EQ(t.rt.gs().reg(TEST_1), 0x55u);
        }
        // O destrutor do Runtime encerra a thread viva (round par) ou já parada.
    }
}

// O espelho do PATH3 mascarado: o EE o sabe sem fence, na ordem de emissão, e bate com o do GIF.
TEST_CASE(vu1_thread, path3_mask_mirror_needs_no_fence) {
    Cfg cfg;
    cfg.thread = true;
    cfg.jitter = 2;
    Rig t(cfg);
    Vu1Stream& s = t.rt.vu1Stream();
    VifItem m{VifItem::Kind::Path3Mask};
    m.masked = true;
    s.pushVif(m);
    CHECK(s.path3Masked());
    s.fence();
    CHECK(t.rt.gif().path3Masked());
    s.pushVif(VifItem{VifItem::Kind::Reset});
    CHECK(!s.path3Masked());
    s.pushVif(m);
    CHECK(s.path3Masked());
    s.pushGifReset();
    CHECK(!s.path3Masked());
    s.pushVif(m);
    s.pushGifWrite(0x10003000u, 1, 0);  // GIF_CTRL.RST
    CHECK(!s.path3Masked());
    s.fence();
    CHECK(!t.rt.gif().path3Masked());
}

// Sem nada a fazer o fence é só uma verificação de atômicos; com itens, espera de verdade.
TEST_CASE(vu1_thread, fence_waits_for_the_consumer_and_applies_the_responses) {
    Cfg cfg;
    cfg.thread = true;
    cfg.jitter = 21;
    Rig t(cfg);
    Vu1Stream& s = t.rt.vu1Stream();
    s.setImr(0);
    CHECK(!s.needsFence());
    Packet p;
    p.ad(0x1234ull | (0xFFFFull << 32), SIGNAL);
    p.ad(0, FINISH);
    const Qw q = p.finish();
    s.pushPath3(reinterpret_cast<const std::uint8_t*>(q.data()), q.size() / 2, 0);
    CHECK(s.needsFence());
    // A resposta (SIGNAL no CSR, SIGLBLID, INTC_GS) só aparece no EE depois do fence.
    CHECK_EQ(s.eeState().siglblid(), 0u);
    CHECK_EQ(t.rt.kernel().intcStat() & 1u, 0u);
    s.fence();
    CHECK(!s.needsFence());
    CHECK_EQ(s.eeState().siglblid() & 0xFFFF, 0x1234u);
    CHECK_EQ(t.rt.kernel().intcStat() & 1u, 1u);
    CHECK(s.eeState().nextEventTime() != ~std::uint64_t{0});
    CHECK(s.stats().publishes >= 1u);
}

// HostProfile por thread: o que o consumidor mede (GIF, GS produtor, VU1, UNPACK) não vai para os
// baldes do EE, e o relatório mostra as duas threads.
TEST_CASE(vu1_thread, host_profile_has_a_track_per_thread) {
    struct ProfileOn {
        ProfileOn() { HostProfile::enable(); }
        ~ProfileOn() { HostProfile::disable(); }
    } profile;
    Cfg cfg;
    cfg.thread = true;
    Rig t(cfg);
    Rng g(9);
    for (unsigned i = 0; i < 300; ++i) {
        const Qw q = drawPacket(g, 3);
        t.rt.vu1Stream().pushPath3(reinterpret_cast<const std::uint8_t*>(q.data()), q.size() / 2, 0);
    }
    t.rt.vu1Stream().fence();
    CHECK(HostProfile::threaded());
    const std::string rep = HostProfile::report();
    CHECK(rep.find("thread do VU1") != std::string::npos);
    CHECK(rep.find("espera pelo VU1 (fence)") != std::string::npos);
    CHECK(rep.find("anel cheio") != std::string::npos);
    CHECK(rep.find("ocioso (anel vazio)") != std::string::npos);
    // A linha "GIF" aparece duas vezes: na thread do EE (nada: o GIF roda no consumidor) e na do VU1.
    const std::size_t first = rep.find("s  GIF\n");
    CHECK(first != std::string::npos);
    const std::size_t second = first == std::string::npos ? first : rep.find("s  GIF\n", first + 1);
    CHECK(second != std::string::npos);
    if (first != std::string::npos && second != std::string::npos) {
        const std::size_t l1 = rep.rfind("[perfil]", first), l2 = rep.rfind("[perfil]", second);
        CHECK_EQ(rep.substr(l1, 17), std::string("[perfil]   0.00% "));  // EE: 0%
        CHECK(rep.substr(l2, 17) != std::string("[perfil]   0.00% "));   // consumidor: trabalhou
    }
    const std::string iv = HostProfile::interval(1);
    CHECK(iv.find("thread do VU1:") != std::string::npos);
    CHECK(iv.find("VUesp") != std::string::npos);
}

// O modo adiado também usa o anel: quando enche, o próprio EE consome (não há consumidor para
// esperar) e nada se perde nem se reordena.
TEST_CASE(vu1_thread, deferred_mode_drains_when_the_ring_is_full) {
    Rig d, ref;
    d.rt.vu1Stream().setRingSlots(4);
    d.rt.vu1Stream().setDeferred(true);
    Rng g1(11), g2(11);
    for (unsigned i = 0; i < 100; ++i) {
        const Qw a = drawPacket(g1, 1), b = drawPacket(g2, 1);
        d.rt.vu1Stream().pushPath3(reinterpret_cast<const std::uint8_t*>(a.data()), a.size() / 2, 0x100);
        ref.rt.vu1Stream().pushPath3(reinterpret_cast<const std::uint8_t*>(b.data()), b.size() / 2, 0x100);
        CHECK(d.rt.vu1Stream().queued() <= 4u);
    }
    d.rt.vu1Stream().fence();
    CHECK_EQ(d.rt.gs().drawCount(), 100u);
    CHECK_EQ(fnv(d.rt.gs().vram().data(), Vram::kSize), fnv(ref.rt.gs().vram().data(), Vram::kSize));
}
