// Teste diferencial do microcódigo recompilado: cada programa aleatório
// (vu_fuzz_gen) roda no interpretador e no C++ gerado a partir de vários
// estados iniciais aleatórios; registradores, flags, ACC, Q/P/I/R, memória
// de dados, ciclos e TPC têm de ser idênticos. Também verifica:
//  * o bloco recompilado carregado em outro endereço (base diferente);
//  * um par alterado na micro memória: o código recompilado devolve o
//    controle ao interpretador no par alterado e retoma depois;
//  * o estado de entrada do pipeline: flags, Q/P e VF em voo quando o
//    microprograma começa (rede de segurança para mudanças na contabilidade);
//  * o endereço e o ciclo de cada XGKICK;
//  * a vazão de cada caminho (informativa), em programas curtos e num laço
//    longo de transformação de vértices.
//
// uso: anyps2_vu_diff [--bench N [reps] | --bench-long N [reps]]
//   --bench N       só a vazão dos programas curtos (N passadas), para perfilar
//   --bench-long N  só a vazão do laço longo (N starts)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "anyps2/runtime/vu/vu.h"
#include "vu_fuzz_data.h"

extern const ::anyps2::rt::VuProgramEntry kFuzzVu0[], kFuzzVu1[], kFuzzVu2[], kFuzzVu3[];
extern const std::size_t kFuzzVu0Count, kFuzzVu1Count, kFuzzVu2Count, kFuzzVu3Count;

namespace {

using anyps2::rt::Reg128;
using anyps2::rt::Vu;

constexpr std::uint32_t kSize = 0x4000;

struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(next() % n); }
};

// Floats variados: comuns, zero, enormes, expoente 0 (denormal) e 255.
std::uint32_t randomFloat(Rng& r) {
    const std::uint32_t k = r.below(100);
    const std::uint32_t sign = r.below(2) << 31;
    if (k < 55) {
        const std::uint32_t exp = 119 + r.below(17);  // ~2^-8 .. 2^8
        return sign | (exp << 23) | (static_cast<std::uint32_t>(r.next()) & 0x7FFFFFu);
    }
    if (k < 65) return sign;
    if (k < 75) return sign | ((228 + r.below(26)) << 23) | (static_cast<std::uint32_t>(r.next()) & 0x7FFFFFu);
    if (k < 80) return sign | (static_cast<std::uint32_t>(r.next()) & 0x7FFFFFu);
    if (k < 85) return sign | (255u << 23) | (static_cast<std::uint32_t>(r.next()) & 0x7FFFFFu);
    return static_cast<std::uint32_t>(r.next());
}

// Floats "de vértice" (±0,25..2): evitam que a medição de vazão do laço longo
// seja dominada por saturações e denormais.
std::uint32_t niceFloat(Rng& r) {
    const float f = (0.25f + static_cast<float>(r.below(1750)) / 1000.0f) * (r.below(2) ? -1.0f : 1.0f);
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}

struct Machine {
    Reg128 vf[32]{};
    std::uint32_t vi[32]{};
    Reg128 acc{};
    std::vector<std::uint8_t> data = std::vector<std::uint8_t>(kSize);
    std::vector<std::uint8_t> micro = std::vector<std::uint8_t>(kSize);
    Vu vu{nullptr, 1, anyps2::rt::vucore::Regs{vf, vi, &acc}, data.data(), kSize, micro.data(), kSize};
    std::vector<anyps2::rt::VuXgkickEvent> kicks;  // XGKICK da última execução
    Machine() { vu.setXgkickLog(&kicks); }
    Machine(const Machine&) = delete;
    Machine& operator=(const Machine&) = delete;
};

void randomState(Machine& m, Rng& r, std::uint32_t endPc) {
    m.kicks.clear();
    for (unsigned i = 1; i < 32; ++i) {
        for (auto& c : m.vf[i].uw) c = randomFloat(r);
    }
    for (auto& c : m.acc.uw) c = randomFloat(r);
    for (unsigned i = 1; i < 16; ++i) {
        m.vi[i] = r.below(4) == 0 ? static_cast<std::uint32_t>(r.next()) & 0xFFFF : r.below(64);
    }
    m.vi[14] = 1 + r.below(3);   // contador dos laços (se um desvio pular a inicialização)
    m.vi[15] = endPc / 8;        // alvo de JR (idem)
    m.vi[16] = static_cast<std::uint32_t>(r.next()) & 0xFFF;       // status
    m.vi[17] = static_cast<std::uint32_t>(r.next()) & 0xFFFF;      // MAC
    m.vi[18] = static_cast<std::uint32_t>(r.next()) & 0xFFFFFF;    // clip
    m.vi[20] = 0x3F800000u | (static_cast<std::uint32_t>(r.next()) & 0x7FFFFF);  // R
    m.vi[21] = randomFloat(r);   // I
    m.vi[22] = randomFloat(r);   // Q
    m.vi[23] = randomFloat(r);   // P
    for (std::size_t i = 0; i < kSize; i += 4) {
        const std::uint32_t w = randomFloat(r);
        std::memcpy(m.data.data() + i, &w, 4);
    }
}

// Estado de entrada do pipeline: flags na fila (com o anel em posição
// arbitrária), Q e P em voo, VF e ACC ocupados. Os atrasos passam do que o
// fluxo normal produz (4 ciclos) para cobrir também o que já está pronto e o
// que demora mais.
anyps2::rt::VuPipelineSeed randomPipeline(Rng& r) {
    anyps2::rt::VuPipelineSeed seed;
    for (unsigned i = 1; i < 32; ++i) {
        if (r.below(4) == 0) seed.vfBusy[i] = static_cast<std::uint8_t>(1 + r.below(6));
    }
    const std::uint32_t n = r.below(4) == 0 ? 0 : r.below(7);
    for (std::uint32_t i = 0; i < n; ++i) {
        anyps2::rt::VuPipelineSeed::Flags f;
        f.delay = static_cast<std::uint8_t>(r.below(6));
        f.hasMac = r.below(4) != 0;
        f.mac = static_cast<std::uint16_t>(r.next());
        f.hasClip = r.below(3) == 0;
        f.clip = static_cast<std::uint32_t>(r.next()) & 0xFFFFFFu;
        if (!f.hasMac && !f.hasClip) f.hasMac = true;
        seed.flags.push_back(f);
    }
    seed.head = r.below(16);
    static const std::uint32_t kDivFlags[] = {0, 0x10, 0x20, 0x30};
    if (r.below(2)) {
        seed.q = {true, static_cast<std::uint8_t>(r.below(9)), randomFloat(r), kDivFlags[r.below(4)]};
    }
    if (r.below(3) == 0) seed.p = {true, static_cast<std::uint8_t>(r.below(22)), randomFloat(r), 0};
    return seed;
}

void copyState(Machine& to, const Machine& from) {
    std::memcpy(to.vf, from.vf, sizeof(to.vf));
    std::memcpy(to.vi, from.vi, sizeof(to.vi));
    to.acc = from.acc;
    to.data = from.data;
    to.micro = from.micro;
    to.kicks.clear();
}

int failures = 0;

bool same(const Machine& a, const Machine& b, const std::string& what) {
    std::string diff;
    for (unsigned i = 0; i < 32 && diff.empty(); ++i) {
        if (std::memcmp(&a.vf[i], &b.vf[i], 16) != 0) diff = "vf" + std::to_string(i);
    }
    for (unsigned i = 0; i < 32 && diff.empty(); ++i) {
        if (a.vi[i] != b.vi[i]) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "vi[%u] (interp %08x, recompilado %08x)", i, a.vi[i], b.vi[i]);
            diff = buf;
        }
    }
    if (diff.empty() && std::memcmp(&a.acc, &b.acc, 16) != 0) diff = "ACC";
    if (diff.empty() && a.data != b.data) diff = "memória de dados";
    if (diff.empty() && a.vu.cycle() != b.vu.cycle()) {
        diff = "ciclos (" + std::to_string(a.vu.cycle()) + " vs " + std::to_string(b.vu.cycle()) + ")";
    }
    if (diff.empty() && a.kicks.size() != b.kicks.size()) {
        diff = "número de XGKICK (" + std::to_string(a.kicks.size()) + " vs " + std::to_string(b.kicks.size()) + ")";
    }
    for (std::size_t i = 0; i < a.kicks.size() && diff.empty(); ++i) {
        if (a.kicks[i].addr != b.kicks[i].addr || a.kicks[i].cycle != b.kicks[i].cycle) {
            diff = "XGKICK " + std::to_string(i) + " (endereço " + std::to_string(a.kicks[i].addr) + " ciclo " +
                   std::to_string(a.kicks[i].cycle) + " vs endereço " + std::to_string(b.kicks[i].addr) +
                   " ciclo " + std::to_string(b.kicks[i].cycle) + ")";
        }
    }
    if (diff.empty()) return true;
    if (++failures <= 20) std::printf("DIFERENÇA %s: %s\n", what.c_str(), diff.c_str());
    return false;
}

// Executa e devolve a mensagem de erro (vazia se terminou normalmente).
template <typename F>
std::string guarded(F&& f) {
    try {
        f();
    } catch (const std::exception& e) {
        return e.what();
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    // --bench N [reps] / --bench-long N [reps]: só a medição de vazão, para perfilar.
    const std::string mode = argc >= 2 ? argv[1] : "";
    const bool benchShort = (argc == 3 || argc == 4) && mode == "--bench";
    const bool benchLongOnly = (argc == 3 || argc == 4) && mode == "--bench-long";
    const bool benchOnly = benchShort || benchLongOnly;
    if (argc > 1 && !benchOnly) {
        std::fprintf(stderr, "uso: anyps2_vu_diff [--bench N [reps] | --bench-long N [reps]]\n");
        return 2;
    }
    const int benchIterations = benchOnly ? std::atoi(argv[2]) : 200;
    const int benchReps = benchOnly && argc == 4 ? std::max(1, std::atoi(argv[3])) : 1;
    const int kSeeds = benchOnly ? 0 : 48;
    Machine interp, comp;
    std::vector<anyps2::rt::VuProgramEntry> programs;
    using Table = std::pair<const anyps2::rt::VuProgramEntry*, std::size_t>;
    for (const Table& t : {Table{kFuzzVu0, kFuzzVu0Count}, Table{kFuzzVu1, kFuzzVu1Count},
                           Table{kFuzzVu2, kFuzzVu2Count}, Table{kFuzzVu3, kFuzzVu3Count}}) {
        programs.insert(programs.end(), t.first, t.first + t.second);
    }
    comp.vu.setPrograms(programs.data(), programs.size());
    comp.vu.setMode(Vu::Mode::CompiledOnly);
    Rng rng{0xD1FFull};
    constexpr int kStateSeeds = 24;
    std::size_t runs = 0;

    // 1. Mesmo estado, mesma posição: tudo recompilado.
    for (const auto& p : kFuzzPrograms) {
        for (int s = 0; s < kSeeds; ++s) {
            std::memcpy(interp.micro.data(), kFuzzImage, kSize);
            randomState(interp, rng, p.offset + (p.pairs - 2) * 8);
            copyState(comp, interp);
            const std::string ea = guarded([&] { interp.vu.interpret(p.offset, 0); });
            const std::string eb = guarded([&] { comp.vu.start(p.offset, 0); });
            const std::string what = "programa em " + std::to_string(p.offset) + ", semente " + std::to_string(s);
            if (ea != eb) {
                ++failures;
                std::printf("DIFERENÇA %s: erro '%s' vs '%s'\n", what.c_str(), ea.c_str(), eb.c_str());
            }
            same(interp, comp, what);
            if (std::getenv("VUDIFF_VERBOSE")) {
                std::printf("%s: %llu pares %s\n", what.c_str(),
                            static_cast<unsigned long long>(comp.vu.compiledPairs()), ea.c_str());
            }
            ++runs;
        }
    }
    if (comp.vu.interpretedPairs() != 0) {
        ++failures;
        std::printf("FALHA: %llu pares interpretados no modo só-recompilado\n",
                    static_cast<unsigned long long>(comp.vu.interpretedPairs()));
    }

    // 2. Bloco carregado em outra base (programas sem JR absoluto).
    comp.vu.setMode(Vu::Mode::Auto);
    std::size_t relocated = 0;
    for (const auto& p : kFuzzPrograms) {
        if (p.hasJr || benchOnly) continue;
        const std::uint32_t base = (p.offset + 8 * (1 + rng.below(64))) % (kSize - p.pairs * 8) & ~7u;
        std::memset(interp.micro.data(), 0, kSize);
        std::memcpy(interp.micro.data() + base, reinterpret_cast<const std::uint8_t*>(kFuzzImage) + p.offset,
                    p.pairs * 8);
        randomState(interp, rng, base + (p.pairs - 2) * 8);
        copyState(comp, interp);
        const auto before = comp.vu.compiledPairs();
        const std::string ea = guarded([&] { interp.vu.interpret(base, 0); });
        const std::string eb = guarded([&] { comp.vu.start(base, 0); });
        if (ea != eb) ++failures;
        same(interp, comp, "programa realocado para " + std::to_string(base));
        if (comp.vu.compiledPairs() == before) {
            ++failures;
            std::printf("FALHA: programa realocado para %u não usou o código recompilado\n", base);
        }
        ++relocated;
    }

    // 3. Um par alterado na micro memória: volta ao interpretador ali.
    std::size_t patched = 0;
    for (const auto& p : kFuzzPrograms) {
        if (benchOnly) break;
        std::memcpy(interp.micro.data(), kFuzzImage, kSize);
        // No laço longo só se altera um par do corpo (o 11º: madday); trocar o
        // contador ou o desvio por NOP daria um laço infinito.
        const std::uint32_t at = p.longLoop ? p.offset + 8 * 10 : p.offset + 8 * (1 + rng.below(p.pairs - 3));
        const std::uint32_t nopPair[2] = {0x8000033Cu, 0x000002FFu};
        std::memcpy(interp.micro.data() + at, nopPair, 8);
        randomState(interp, rng, p.offset + (p.pairs - 2) * 8);
        copyState(comp, interp);
        const std::string ea = guarded([&] { interp.vu.interpret(p.offset, 0); });
        const std::string eb = guarded([&] { comp.vu.start(p.offset, 0); });
        if (ea != eb) ++failures;
        same(interp, comp, "programa em " + std::to_string(p.offset) + " com o par " + std::to_string(at) +
                               " alterado");
        ++patched;
    }

    // 4. Estado de entrada do pipeline: flags, Q/P e VF em voo.
    comp.vu.setMode(Vu::Mode::CompiledOnly);
    std::size_t seeded = 0;
    for (const auto& p : kFuzzPrograms) {
        if (benchOnly) break;
        for (int s = 0; s < kStateSeeds; ++s) {
            std::memcpy(interp.micro.data(), kFuzzImage, kSize);
            randomState(interp, rng, p.offset + (p.pairs - 2) * 8);
            copyState(comp, interp);
            const auto seed = randomPipeline(rng);
            interp.vu.seedPipeline(seed);
            comp.vu.seedPipeline(seed);
            const std::string ea = guarded([&] { interp.vu.interpret(p.offset, 0); });
            const std::string eb = guarded([&] { comp.vu.start(p.offset, 0); });
            const std::string what = "programa em " + std::to_string(p.offset) + " com o pipeline semeado " +
                                     std::to_string(s);
            if (ea != eb) {
                ++failures;
                std::printf("DIFERENÇA %s: erro '%s' vs '%s'\n", what.c_str(), ea.c_str(), eb.c_str());
            }
            same(interp, comp, what);
            ++seeded;
        }
    }

    // 5. Vazão (informativa). `longLoop` escolhe o laço longo ou os programas curtos.
    auto bench = [&](Machine& m, bool compiled, bool longLoop) {
        std::memcpy(m.micro.data(), kFuzzImage, kSize);
        Rng r{42};
        randomState(m, r, 0);
        if (longLoop) {
            for (std::size_t i = 0; i < kSize; i += 4) {
                const std::uint32_t w = niceFloat(r);
                std::memcpy(m.data.data() + i, &w, 4);
            }
        }
        m.vu.setMode(compiled ? Vu::Mode::CompiledOnly : Vu::Mode::Interpret);
#ifdef ANYPS2_VU_STATS
        m.vu.resetStats();
#endif
        const auto pairs0 = m.vu.compiledPairs() + m.vu.interpretedPairs();
        const auto t0 = std::chrono::steady_clock::now();
        for (int it = 0; it < benchIterations; ++it) {
            for (const auto& p : kFuzzPrograms) {
                if (p.longLoop != longLoop) continue;
                m.vi[14] = 1;
                m.vi[15] = (p.offset + (p.pairs - 2) * 8) / 8;
                m.kicks.clear();
                (void)guarded([&] { m.vu.start(p.offset, 0); });
            }
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return static_cast<double>(m.vu.compiledPairs() + m.vu.interpretedPairs() - pairs0) / s / 1e6;
    };
    struct Rate {
        double median, lo, hi;
    };
    auto rate = [&](Machine& m, bool compiled, bool longLoop) {
        std::vector<double> v;
        for (int i = 0; i < benchReps; ++i) v.push_back(bench(m, compiled, longLoop));
        std::sort(v.begin(), v.end());
        return Rate{v[v.size() / 2], v.front(), v.back()};
    };
    auto report = [&](const char* label, bool longLoop) {
        const Rate mi = rate(interp, false, longLoop), mc = rate(comp, true, longLoop);
        if (benchReps > 1) {
            std::printf("vu_diff %s: interpretador %.1f Mpares/s [%.1f..%.1f], recompilado %.1f Mpares/s [%.1f..%.1f] (%.2fx, %d repetições)\n",
                        label, mi.median, mi.lo, mi.hi, mc.median, mc.lo, mc.hi, mc.median / mi.median, benchReps);
        } else {
            std::printf("vu_diff %s: interpretador %.1f Mpares/s, recompilado %.1f Mpares/s (%.1fx)\n", label,
                        mi.median, mc.median, mc.median / mi.median);
        }
#ifdef ANYPS2_VU_STATS
        const auto& st = comp.vu.stats();
        std::printf("vu_diff %s (recompilado, última passada): %llu starts, %llu pares (%.1f por start, máx. %llu), "
                    "findBlock %llu/%llu acertos, commitReady %llu verificações -> %llu commitPending "
                    "(%.4f por par; %llu flags, %llu Q, %llu P), %llu stalls (%llu ciclos), %llu XGKICK\n",
                    label, static_cast<unsigned long long>(st.starts), static_cast<unsigned long long>(st.pairs),
                    st.starts ? static_cast<double>(st.pairs) / static_cast<double>(st.starts) : 0.0,
                    static_cast<unsigned long long>(st.maxPairsPerStart),
                    static_cast<unsigned long long>(st.findBlockHits), static_cast<unsigned long long>(st.findBlockCalls),
                    static_cast<unsigned long long>(st.commitReadyChecks),
                    static_cast<unsigned long long>(st.commitPendingCalls),
                    st.pairs ? static_cast<double>(st.commitPendingCalls) / static_cast<double>(st.pairs) : 0.0,
                    static_cast<unsigned long long>(st.flagsCommitted), static_cast<unsigned long long>(st.qCommitted),
                    static_cast<unsigned long long>(st.pCommitted), static_cast<unsigned long long>(st.stalls),
                    static_cast<unsigned long long>(st.stallCycles), static_cast<unsigned long long>(st.xgkicks));
#endif
    };
    if (!benchLongOnly) report("curto", false);
    if (!benchShort) report("longo", true);
    if (benchOnly) return failures ? 1 : 0;

    std::printf("vu_diff: %zu programas, %zu execuções comparadas, %zu realocadas, %zu com par alterado, %zu com o pipeline semeado\n",
                std::size(kFuzzPrograms), runs, relocated, patched, seeded);
    if (failures) {
        std::printf("vu_diff: %d diferença(s)\n", failures);
        return 1;
    }
    std::printf("vu_diff: OK\n");
    return 0;
}
