// Teste diferencial do microcódigo recompilado: cada programa aleatório
// (vu_fuzz_gen) roda no interpretador e no C++ gerado a partir de vários
// estados iniciais aleatórios; registradores, flags, ACC, Q/P/I/R, memória
// de dados, ciclos e TPC têm de ser idênticos. Também verifica:
//  * o bloco recompilado carregado em outro endereço (base diferente);
//  * um par alterado na micro memória: o código recompilado devolve o
//    controle ao interpretador no par alterado e retoma depois;
//  * a vazão de cada caminho (informativa).

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

struct Machine {
    Reg128 vf[32]{};
    std::uint32_t vi[32]{};
    Reg128 acc{};
    std::vector<std::uint8_t> data = std::vector<std::uint8_t>(kSize);
    std::vector<std::uint8_t> micro = std::vector<std::uint8_t>(kSize);
    Vu vu{nullptr, 1, anyps2::rt::vucore::Regs{vf, vi, &acc}, data.data(), kSize, micro.data(), kSize};
};

void randomState(Machine& m, Rng& r, std::uint32_t endPc) {
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

void copyState(Machine& to, const Machine& from) {
    std::memcpy(to.vf, from.vf, sizeof(to.vf));
    std::memcpy(to.vi, from.vi, sizeof(to.vi));
    to.acc = from.acc;
    to.data = from.data;
    to.micro = from.micro;
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
    // --bench N: só a medição de vazão (N repetições), para perfilar.
    const bool benchOnly = argc == 3 && std::string(argv[1]) == "--bench";
    const int benchIterations = benchOnly ? std::atoi(argv[2]) : 200;
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
        const std::uint32_t at = p.offset + 8 * (1 + rng.below(p.pairs - 3));
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

    // 4. Vazão (informativa).
    auto bench = [&](Machine& m, bool compiled) {
        std::memcpy(m.micro.data(), kFuzzImage, kSize);
        Rng r{42};
        randomState(m, r, 0);
        m.vu.setMode(compiled ? Vu::Mode::CompiledOnly : Vu::Mode::Interpret);
        const auto pairs0 = m.vu.compiledPairs() + m.vu.interpretedPairs();
        const auto t0 = std::chrono::steady_clock::now();
        for (int it = 0; it < benchIterations; ++it) {
            for (const auto& p : kFuzzPrograms) {
                m.vi[14] = 1;
                m.vi[15] = (p.offset + (p.pairs - 2) * 8) / 8;
                (void)guarded([&] { m.vu.start(p.offset, 0); });
            }
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return static_cast<double>(m.vu.compiledPairs() + m.vu.interpretedPairs() - pairs0) / s / 1e6;
    };
    const double mi = bench(interp, false), mc = bench(comp, true);

    std::printf("vu_diff: %zu programas, %zu execuções comparadas, %zu realocadas, %zu com par alterado\n",
                std::size(kFuzzPrograms), runs, relocated, patched);
    std::printf("vu_diff: interpretador %.1f Mpares/s, recompilado %.1f Mpares/s (%.1fx)\n", mi, mc, mc / mi);
    if (failures) {
        std::printf("vu_diff: %d diferença(s)\n", failures);
        return 1;
    }
    std::printf("vu_diff: OK\n");
    return 0;
}
