// vu_replay: reexecuta as execuções do VU1 gravadas com ANYPS2_VU_RECORD (vu_record.h) contra o
// microcódigo recompilado do jogo (vu_000.cpp/vu_programs.cpp gerados, linkados neste executável) ou
// contra o interpretador. Serve para
//   (a) medir ns/par do VU sem rodar o jogo;
//   (b) resumir o estado final de cada execução num hash (dados, VF/ACC, VI/flags/Q/P/TPC, pipeline,
//       XGKICK com ciclo, pares, ciclos);
//   (c) gravar esses hashes (--golden) e conferi-los depois (--check): qualquer mudança no runtime ou
//       no gerador do VU tem de manter o golden idêntico.
//
// uso: vu_replay <gravacao> [--golden arq | --check arq] [--interp] [-n passadas] [-m repeticoes]
//                          [--limit k] [--no-time] [--quiet]
//   --golden arq    grava os hashes (um por entrada) em arq
//   --check arq     compara com um golden e lista as diferenças (código de saída 1 se houver)
//   --interp        executa pelo interpretador em vez do compilado (comparação cruzada)
//   -n N            passadas sobre a gravação em cada repetição de tempo (padrão 3)
//   -m M            repetições de tempo; vale a melhor (padrão 5)
//   --limit K       só as K primeiras entradas
//   --no-time       não mede
// Com a opção ANYPS2_VU_STATS do CMake também imprime os contadores por par (uma passada).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "anyps2/common/error.h"
#include "anyps2/runtime/vu/vu_record.h"

// Geradas pelo "anyps2 recomp" (vu_programs.cpp).
extern const anyps2::rt::VuProgramEntry kVuPrograms[];
extern const std::size_t kVuProgramsCount;

namespace {

using namespace anyps2::rt;

struct Options {
    std::string recording, golden, check;
    bool interp = false, time = true;
    int passes = 3, reps = 5;
    std::size_t limit = ~std::size_t{0};
};

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](std::string& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--golden") { if (!next(o.golden)) return false; }
        else if (a == "--check") { if (!next(o.check)) return false; }
        else if (a == "--interp") o.interp = true;
        else if (a == "--no-time") o.time = false;
        else if (a == "-n") { if (!next(v)) return false; o.passes = std::max(1, std::atoi(v.c_str())); }
        else if (a == "-m") { if (!next(v)) return false; o.reps = std::max(1, std::atoi(v.c_str())); }
        else if (a == "--limit") { if (!next(v)) return false; o.limit = static_cast<std::size_t>(std::strtoull(v.c_str(), nullptr, 10)); }
        else if (!a.empty() && a[0] != '-' && o.recording.empty()) o.recording = a;
        else return false;
    }
    return !o.recording.empty() && !(o.golden.size() && o.check.size());
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) {
        std::fputs("uso: vu_replay <gravacao> [--golden arq | --check arq] [--interp] [-n passadas] [-m repeticoes] [--limit k] [--no-time]\n", stderr);
        return 2;
    }
    try {
        VuRecording rec = VuRecording::load(opt.recording);
        if (rec.entries.size() > opt.limit) rec.entries.resize(opt.limit);
        if (rec.entries.empty()) {
            std::fputs("vu_replay: a gravação não tem entradas\n", stderr);
            return 2;
        }
        VuReplayMachine m(rec.microSize, rec.dataSize, rec.unit);
        m.vu().setPrograms(kVuPrograms, kVuProgramsCount);
        const char* what = opt.interp ? "interpretador" : "compilado";

        // ---- passada de verificação: hash por entrada ----
        std::vector<VuRunHash> hashes(rec.entries.size());
        std::vector<std::string> errors(rec.entries.size());
#ifdef ANYPS2_VU_STATS
        m.vu().resetStats();
#endif
        std::uint64_t totalPairs = 0;
        std::set<std::uint32_t> startPcs;
        for (std::size_t i = 0; i < rec.entries.size(); ++i) {
            hashes[i] = m.replay(rec.entries[i], opt.interp, true, nullptr, &errors[i]);
            totalPairs += hashes[i].pairs;
            startPcs.insert(rec.entries[i].startPc);
        }
        std::uint64_t vbMin = ~0ull, vbMax = 0;
        for (const auto& e : rec.entries) {
            vbMin = std::min(vbMin, e.vblank);
            vbMax = std::max(vbMax, e.vblank);
        }
        std::printf("vu_replay (%s): %zu entradas (VBlanks %llu a %llu, %zu inícios distintos), %llu pares, "
                    "%.1f%% dos pares pelo código recompilado\n",
                    what, rec.entries.size(), static_cast<unsigned long long>(vbMin), static_cast<unsigned long long>(vbMax),
                    startPcs.size(), static_cast<unsigned long long>(totalPairs),
                    100.0 * static_cast<double>(m.vu().compiledPairs()) /
                        static_cast<double>(std::max<std::uint64_t>(1, m.vu().compiledPairs() + m.vu().interpretedPairs())));
        std::size_t nerr = 0;
        for (const auto& e : errors) nerr += !e.empty();
        if (nerr) std::printf("vu_replay: %zu entrada(s) terminaram com erro do guest (o hash cobre o estado até ali)\n", nerr);
#ifdef ANYPS2_VU_STATS
        std::fputs(formatVuStats(m.vu().stats(), "replay (uma passada)").c_str(), stdout);
#endif

        // ---- golden ----
        int rc = 0;
        if (!opt.golden.empty()) {
            std::ofstream g(opt.golden, std::ios::trunc);
            if (!g) throw anyps2::Error("não foi possível criar " + opt.golden);
            g << "# vu_replay golden v1 entradas=" << hashes.size()
              << " (índice vblank startPc | todos dados vf/acc vi pipeline xgkick pares ciclos)\n";
            for (std::size_t i = 0; i < hashes.size(); ++i) {
                char head[64];
                std::snprintf(head, sizeof head, "%zu %llu %04x | ", i, static_cast<unsigned long long>(rec.entries[i].vblank),
                              rec.entries[i].startPc);
                g << head << hashes[i].str() << "\n";
            }
            std::printf("vu_replay: golden com %zu hashes gravado em %s\n", hashes.size(), opt.golden.c_str());
        }
        if (!opt.check.empty()) {
            std::ifstream g(opt.check);
            if (!g) throw anyps2::Error("não foi possível abrir " + opt.check);
            std::vector<VuRunHash> want;
            std::string line;
            while (std::getline(g, line)) {
                if (line.empty() || line[0] == '#') continue;
                const auto bar = line.find('|');
                VuRunHash h;
                if (bar == std::string::npos || !VuRunHash::parse(line.substr(bar + 1), h)) {
                    throw anyps2::Error("linha inválida no golden: " + line);
                }
                want.push_back(h);
            }
            std::size_t bad = 0;
            if (want.size() != hashes.size()) {
                std::printf("vu_replay: DIFERENÇA: o golden tem %zu entradas e a gravação %zu\n", want.size(), hashes.size());
                bad = 1;
            }
            for (std::size_t i = 0; i < std::min(want.size(), hashes.size()); ++i) {
                if (want[i] == hashes[i]) continue;
                if (++bad <= 20) {
                    std::printf("vu_replay: DIFERENÇA na entrada %zu (VBlank %llu, início %04x): %s\n", i,
                                static_cast<unsigned long long>(rec.entries[i].vblank), rec.entries[i].startPc,
                                want[i].diff(hashes[i]).c_str());
                }
            }
            if (bad) {
                std::printf("vu_replay: %zu diferença(s) em relação a %s\n", bad, opt.check.c_str());
                rc = 1;
            } else {
                std::printf("vu_replay: OK, %zu hashes iguais ao golden %s\n", hashes.size(), opt.check.c_str());
            }
        }

        // ---- tempo ----
        if (opt.time && totalPairs) {
            std::vector<double> best;
            for (int r = 0; r < opt.reps; ++r) {
                std::uint64_t ns = 0, pairs = 0;
                for (int p = 0; p < opt.passes; ++p) {
                    for (const VuRecEntry& e : rec.entries) {
                        m.apply(e);
                        std::uint64_t t = 0;
                        m.run(e, opt.interp, false, &t);
                        ns += t;
                        pairs += m.vu().pairs();
                    }
                }
                best.push_back(static_cast<double>(ns) / static_cast<double>(pairs));
            }
            std::sort(best.begin(), best.end());
            std::printf("vu_replay (%s): %.2f ns/par (melhor de %d; mediana %.2f, pior %.2f; %d passada(s) de %llu pares cada)\n",
                        what, best.front(), opt.reps, best[best.size() / 2], best.back(), opt.passes,
                        static_cast<unsigned long long>(totalPairs));
        }
        return rc;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "vu_replay: %s\n", ex.what());
        return 2;
    }
}
