#include "anyps2/runtime/host_profile.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "anyps2/runtime/gs/gs.h"

namespace anyps2::rt {

void HostProfile::enable() {
    enabled_ = true;
    current_ = Ee;
    start_ = since_ = Clock::now();
    for (auto& n : ns_) n = 0;
    for (auto& n : mark_) n = 0;
    markVblank_ = 0;
    workerNs_ = 0;
    workerMark_ = 0;
    bandsMarkVblank_ = 0;
}

void HostProfile::charge(Clock::time_point now) {
    ns_[current_] += std::chrono::duration_cast<std::chrono::nanoseconds>(now - since_).count();
    since_ = now;
}

void HostProfile::Scope::enter(Part p) {
    charge(Clock::now());
    previous_ = current_;
    current_ = p;
    active_ = true;
}

void HostProfile::Scope::leave() {
    charge(Clock::now());
    current_ = previous_;
}

std::string HostProfile::report() {
    if (!enabled_) return "";
    charge(Clock::now());
    static const char* kNames[Count] = {"EE (código e HLE)", "GIF", "GS (desenho no EE)", "GS (EE esperando o worker)",
                                            "VU0", "VU1", "IPU"};
    std::int64_t total = 0;
    for (auto n : ns_) total += n;
    std::string s;
    char line[128];
    std::snprintf(line, sizeof line, "[perfil] tempo do host: %.2f s\n", static_cast<double>(total) / 1e9);
    s += line;
    for (unsigned i = 0; i < Count; ++i) {
        std::snprintf(line, sizeof line, "[perfil] %6.2f%%  %8.2f s  %s\n",
                      total ? 100.0 * static_cast<double>(ns_[i]) / static_cast<double>(total) : 0.0,
                      static_cast<double>(ns_[i]) / 1e9, kNames[i]);
        s += line;
    }
    const unsigned lanes = workerLanes_.load();
    const double busy = static_cast<double>(workerNs_.load());
    std::snprintf(line, sizeof line,
                  "[perfil] faixas do GS ocupadas: %.2f s somados (%.0f%% do tempo do host); "
                  "média por faixa %.0f%% (%u faixas)\n",
                  busy / 1e9, total ? 100.0 * busy / static_cast<double>(total) : 0.0,
                  total ? 100.0 * busy / (static_cast<double>(lanes) * static_cast<double>(total)) : 0.0, lanes);
    s += line;
    return s;
}

std::string HostProfile::interval(std::uint64_t vblank) {
    if (!enabled_) return "";
    charge(Clock::now());
    static const char* kShort[Count] = {"EE", "GIF", "GS", "GSesp", "VU0", "VU1", "IPU"};
    std::int64_t total = 0;
    for (unsigned i = 0; i < Count; ++i) total += ns_[i] - mark_[i];
    char line[96];
    std::snprintf(line, sizeof line, "[perfil] VBlank %llu-%llu: %.1f VBlanks/s;",
                  static_cast<unsigned long long>(markVblank_), static_cast<unsigned long long>(vblank),
                  total ? static_cast<double>(vblank - markVblank_) * 1e9 / static_cast<double>(total) : 0.0);
    std::string s = line;
    for (unsigned i = 0; i < Count; ++i) {
        std::snprintf(line, sizeof line, " %s %.0f%%", kShort[i],
                      total ? 100.0 * static_cast<double>(ns_[i] - mark_[i]) / static_cast<double>(total) : 0.0);
        s += line;
        mark_[i] = ns_[i];
    }
    const std::int64_t worker = workerNs_.load() - workerMark_;
    workerMark_ += worker;
    const double lanes = static_cast<double>(workerLanes_.load());
    std::snprintf(line, sizeof line, "; GS por faixa %.0f%%",
                  total ? 100.0 * static_cast<double>(worker) / (lanes * static_cast<double>(total)) : 0.0);
    s += line;
    markVblank_ = vblank;
    return s + "\n";
}

namespace {

using gs::BandStats;

// Marca do último relatório de trecho (cópia dos contadores acumulados).
BandStats& bandsMark() {
    static BandStats mark;
    return mark;
}

std::uint64_t diff(std::uint64_t cur, std::uint64_t prev) {
    return cur >= prev ? cur - prev : 0;
}

// Os `max` pares mais frequentes de `cur` no trecho (cur − prev), "FBP→TBP0 xN".
std::string topPairs(const BandStats::PairTable& cur, const BandStats::PairTable& prev, double per, unsigned max) {
    std::vector<BandStats::Pair> v;
    for (std::size_t i = 0; i < cur.used; ++i) {
        const std::uint64_t n = diff(cur.items[i].n, prev.count(cur.items[i].fbp, cur.items[i].tbp));
        if (n) v.push_back(BandStats::Pair{cur.items[i].fbp, cur.items[i].tbp, n});
    }
    std::sort(v.begin(), v.end(), [](const BandStats::Pair& a, const BandStats::Pair& b) { return a.n > b.n; });
    if (v.empty()) return " -";
    std::string s;
    char buf[96];
    for (std::size_t i = 0; i < v.size() && i < max; ++i) {
        std::snprintf(buf, sizeof buf, " %X>%X x%.1f", v[i].fbp, v[i].tbp, static_cast<double>(v[i].n) / per);
        s += buf;
    }
    return s;
}

// `per`: divisor das contagens (VBlanks do trecho; 1 no total).
std::string formatBands(const BandStats& cur, const BandStats& prev, double per, const char* unit) {
    auto d = [&](std::uint64_t c, std::uint64_t p) { return static_cast<double>(diff(c, p)) / per; };
    char buf[160];
    std::string s = "[perfil] faixas";
    s += unit;
    s += ":";
    const std::uint64_t draws = diff(cur.bandDraws, prev.bandDraws);
    std::snprintf(buf, sizeof buf, " desenhos nas faixas %.1f (%.2f faixas/desenho);", d(cur.bandDraws, prev.bandDraws),
                  draws ? static_cast<double>(diff(cur.lanesTouched, prev.lanesTouched)) / static_cast<double>(draws)
                        : 0.0);
    s += buf;
    std::snprintf(buf, sizeof buf, " sozinhos: volta %.1f, textura no FRAME/ZBUF %.1f, FRAME×ZBUF %.1f;",
                  d(cur.self[0], prev.self[0]), d(cur.self[1], prev.self[1]), d(cur.self[2], prev.self[2]));
    s += buf;
    std::snprintf(buf, sizeof buf, " barreiras de conflito: fila cheia %.1f, escrita %.1f, textura %.1f;",
                  d(cur.conflict[0], prev.conflict[0]), d(cur.conflict[1], prev.conflict[1]),
                  d(cur.conflict[2], prev.conflict[2]));
    s += buf;
    std::snprintf(buf, sizeof buf,
                  " barreiras de submit: HOST início %.1f, HOST fim %.1f, LOCAL→LOCAL %.1f, CLUT %.1f, reset %.1f, "
                  "desenho %.1f; lotes HOST→LOCAL %.1f; CLUT sem barreira %.1f, HOST→LOCAL sem barreira %.1f;",
                  d(cur.submit[0], prev.submit[0]), d(cur.submit[1], prev.submit[1]), d(cur.submit[2], prev.submit[2]),
                  d(cur.submit[3], prev.submit[3]), d(cur.submit[4], prev.submit[4]), d(cur.submit[5], prev.submit[5]),
                  d(cur.hostBatches, prev.hostBatches), d(cur.clutDirect, prev.clutDirect), d(cur.hostDirect, prev.hostDirect));
    s += buf;
    static const char* kWait[BandStats::WaitCount] = {"vídeo", "vram()", "LOCAL→HOST", "SIGNAL", "FINISH", "outros"};
    s += " esperas do EE (n, ms):";
    for (unsigned i = 0; i < BandStats::WaitCount; ++i) {
        std::snprintf(buf, sizeof buf, " %s %.1f/%.2f%s", kWait[i], d(cur.waits[i], prev.waits[i]),
                      static_cast<double>(cur.waitNs[i] - prev.waitNs[i]) / 1e6 / per,
                      i + 1 < BandStats::WaitCount ? "," : ";");
        s += buf;
    }
    s += " FBP>TBP0 sozinhos por textura:" + topPairs(cur.selfPairs, prev.selfPairs, per, 5) + ";";
    s += " barreiras por textura:" + topPairs(cur.conflictPairs, prev.conflictPairs, per, 5);
    if (cur.selfPairs.lost || cur.conflictPairs.lost) {
        std::snprintf(buf, sizeof buf, " (pares fora da tabela: %llu, %llu)",
                      static_cast<unsigned long long>(cur.selfPairs.lost),
                      static_cast<unsigned long long>(cur.conflictPairs.lost));
        s += buf;
    }
    return s + "\n";
}

}  // namespace

std::string HostProfile::bandsInterval(const gs::BandStats& total, std::uint64_t vblank) {
    if (!enabled_) return "";
    const std::uint64_t n = vblank > bandsMarkVblank_ ? vblank - bandsMarkVblank_ : 1;
    BandStats& mark = bandsMark();
    const std::string s = formatBands(total, mark, static_cast<double>(n), " (por VBlank)");
    mark = total;
    bandsMarkVblank_ = vblank;
    return s;
}

std::string HostProfile::bandsReport(const gs::BandStats& total, std::uint64_t vblank) {
    if (!enabled_) return "";
    char unit[48];
    std::snprintf(unit, sizeof unit, " (total, %llu VBlanks)", static_cast<unsigned long long>(vblank));
    return formatBands(total, BandStats{}, 1.0, unit);
}

}  // namespace anyps2::rt
