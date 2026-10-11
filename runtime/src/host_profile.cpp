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
    char buf[640];  // texto em UTF-8 com setas de 3 bytes e muitos campos: folga para não cortar
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
                  " barreiras de submit: LOCAL→LOCAL %.1f, CLUT %.1f, reset %.1f, desenho %.1f;"
                  " CLUT sem barreira %.1f; HOST→LOCAL direto: sem espera %.1f, espera no início %.1f, "
                  "espera de ordem %.1f; HOST→LOCAL na fila: transferências %.1f, submissões %.1f;",
                  d(cur.submit[0], prev.submit[0]), d(cur.submit[1], prev.submit[1]), d(cur.submit[2], prev.submit[2]),
                  d(cur.submit[3], prev.submit[3]), d(cur.clutDirect, prev.clutDirect),
                  d(cur.hostDirect, prev.hostDirect), d(cur.hostBarrier, prev.hostBarrier),
                  d(cur.hostOrder, prev.hostOrder), d(cur.hostQueued, prev.hostQueued),
                  d(cur.hostQueuedSubmits, prev.hostQueuedSubmits));
    s += buf;
    static const char* kWait[BandStats::WaitCount] = {"vídeo", "vram()", "LOCAL→HOST", "SIGNAL", "FINISH", "HOST→LOCAL", "outros"};
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

// Diagnóstico de texturas: buscas por formato, filtro, tempo amostrado (1 de cada 64
// chamadas, escalado) e histograma de pixels por desenho texturizado.
std::string formatTexels(const gs::TexReport& cur, const gs::TexReport& prev, double per, const char* unit) {
    using gs::TexReport;
    auto d = [&](std::uint64_t c, std::uint64_t p) { return static_cast<double>(diff(c, p)) / per; };
    char buf[512];
    std::string s = "[perfil] texels";
    s += unit;
    s += ":";
    static const char* kFmt[TexReport::FmtCount] = {"T4", "T8", "CT16", "CT32", "outros"};
    std::uint64_t fetches = 0;
    for (unsigned i = 0; i < TexReport::FmtCount; ++i) {
        std::snprintf(buf, sizeof buf, " %s %.0f%s", kFmt[i], d(cur.fetch[i], prev.fetch[i]),
                      i + 1 < TexReport::FmtCount ? "," : ";");
        s += buf;
        fetches += diff(cur.fetch[i], prev.fetch[i]);
    }
    const double samples = static_cast<double>(diff(cur.samples, prev.samples));
    auto pct = [&](std::uint64_t c, std::uint64_t p) { return samples > 0 ? 100.0 * static_cast<double>(diff(c, p)) / samples : 0.0; };
    std::snprintf(buf, sizeof buf, " amostras %.0f (bilinear %.1f%%, nearest %.1f%%, trilinear %.1f%%);",
                  samples / per, pct(cur.linear, prev.linear), 100.0 - pct(cur.linear, prev.linear),
                  pct(cur.trilinear, prev.trilinear));
    s += buf;
    // Tempo de CPU somado das faixas, estimado pelas chamadas medidas.
    const double shT = static_cast<double>(diff(cur.shadeTimed, prev.shadeTimed));
    const double saT = static_cast<double>(diff(cur.sampleTimed, prev.sampleTimed));
    const double shNs = shT > 0 ? static_cast<double>(diff(cur.shadeNs, prev.shadeNs)) / shT : 0.0;
    const double saNs = saT > 0 ? static_cast<double>(diff(cur.sampleNs, prev.sampleNs)) / saT : 0.0;
    const double shMs = shNs * static_cast<double>(diff(cur.shadeCalls, prev.shadeCalls)) / 1e6 / per;
    const double saMs = saNs * samples / 1e6 / per;
    std::snprintf(buf, sizeof buf,
                  " pixels sombreados %.0f, %.0f ns/pixel => %.2f ms; sampleTexture %.0f ns/amostra => %.2f ms (f = %.2f);"
                  " texels por amostra %.2f;",
                  d(cur.shadeCalls, prev.shadeCalls), shNs, shMs, saNs, saMs, shMs > 0 ? saMs / shMs : 0.0,
                  samples > 0 ? static_cast<double>(fetches) / samples : 0.0);
    s += buf;
    s += " desenhos texturizados por pixels (2^k):";
    for (unsigned i = 0; i < gs::BandStats::kPixBuckets; ++i) {
        const double n = d(cur.pixHistTex[i], prev.pixHistTex[i]);
        if (n <= 0) continue;
        if (i == 0) {
            std::snprintf(buf, sizeof buf, " 0:%.1f", n);
        } else {
            std::snprintf(buf, sizeof buf, " <%llu:%.1f", 1ull << i, n);
        }
        s += buf;
    }
    return s + "\n";
}

gs::TexReport& texelsMark() {
    static gs::TexReport mark;
    return mark;
}
std::uint64_t texelsMarkVblank = 0;

std::string formatTexSim(const gs::TexSimReport& cur, const gs::TexSimReport& prev, double per, const char* unit) {
    using gs::TexSimReport;
    auto d = [&](std::uint64_t c, std::uint64_t p) { return static_cast<double>(diff(c, p)) / per; };
    char buf[512];
    std::uint64_t inCache = diff(cur.hits, prev.hits);
    for (unsigned i = 0; i < TexSimReport::MissCount; ++i) inCache += diff(cur.miss[i], prev.miss[i]);
    const double hitPct = inCache ? 100.0 * static_cast<double>(diff(cur.hits, prev.hits)) / static_cast<double>(inCache) : 0.0;
    std::string s = "[perfil] texcache sim";
    s += unit;
    s += ":";
    std::snprintf(buf, sizeof buf, " desenhos texturizados %.1f, no cache %.1f, acertos %.1f (%.1f%%);",
                  d(cur.texDraws, prev.texDraws), static_cast<double>(inCache) / per, d(cur.hits, prev.hits), hitPct);
    s += buf;
    std::snprintf(buf, sizeof buf,
                  " misses: primeira vez %.1f, FRAME %.1f, ZBUF %.1f, HOST %.1f, LOCAL→LOCAL %.1f;",
                  d(cur.miss[0], prev.miss[0]), d(cur.miss[1], prev.miss[1]), d(cur.miss[2], prev.miss[2]),
                  d(cur.miss[3], prev.miss[3]), d(cur.miss[4], prev.miss[4]));
    s += buf;
    std::snprintf(buf, sizeof buf, " fora do cache: volta %.1f, textura no FRAME/ZBUF %.1f, FRAME×ZBUF %.1f\n",
                  d(cur.off[0], prev.off[0]), d(cur.off[1], prev.off[1]), d(cur.off[2], prev.off[2]));
    return s + buf;
}

gs::TexSimReport& texSimMark() {
    static gs::TexSimReport mark;
    return mark;
}
std::uint64_t texSimMarkVblank = 0;

std::string formatTexCache(const gs::TexCacheReport& cur, const gs::TexCacheReport& prev, double per, const char* unit) {
    auto d = [&](std::uint64_t c, std::uint64_t p) { return static_cast<double>(diff(c, p)) / per; };
    auto mb = [](std::uint64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); };
    char buf[640];
    std::string s = "[perfil] texcache";
    s += unit;
    s += ":";
    const double hits = d(cur.blockHits, prev.blockHits), fills = d(cur.blockFills, prev.blockFills),
                 falls = d(cur.fallbacks, prev.fallbacks);
    // Cada texel lido cai em um dos três: bloco já pronto (acerto), bloco que ele mesmo
    // preencheu (falta) ou fora da grade do nível (readPixel direto).
    const double texels = hits + fills + falls;
    std::snprintf(buf, sizeof buf,
                  " texels: acertos %.0f (%.1f%%), blocos preenchidos %.1f, fora da grade %.0f;",
                  hits, texels > 0 ? 100.0 * hits / texels : 0.0, fills, falls);
    s += buf;
    std::snprintf(buf, sizeof buf,
                  " desenhos: com cache %.1f, sem cache %.1f; níveis: versões criadas %.1f, reaproveitados %.1f;"
                  " despejos %.1f;",
                  d(cur.texDraws, prev.texDraws), d(cur.offDraws, prev.offDraws), d(cur.versions, prev.versions),
                  d(cur.reused, prev.reused), d(cur.evictions, prev.evictions));
    s += buf;
    std::snprintf(buf, sizeof buf,
                  " índice %.1f MB (pico %.1f, orçamento %.0f), %llu entradas; vivos %.1f MB (pico %.1f)\n",
                  mb(cur.bytes), mb(cur.peakBytes), mb(cur.budget), static_cast<unsigned long long>(cur.entries),
                  mb(cur.liveBytes), mb(cur.peakLiveBytes));
    return s + buf;
}

gs::TexCacheReport& texCacheMark() {
    static gs::TexCacheReport mark;
    return mark;
}
std::uint64_t texCacheMarkVblank = 0;

}  // namespace

std::string HostProfile::texCacheInterval(const gs::TexCacheReport& total, std::uint64_t vblank) {
    if (!enabled_ || !total.enabled) return "";
    const std::uint64_t n = vblank > texCacheMarkVblank ? vblank - texCacheMarkVblank : 1;
    gs::TexCacheReport& mark = texCacheMark();
    const std::string s = formatTexCache(total, mark, static_cast<double>(n), " (por VBlank)");
    mark = total;
    texCacheMarkVblank = vblank;
    return s;
}

std::string HostProfile::texCacheReport(const gs::TexCacheReport& total, std::uint64_t vblank) {
    if (!enabled_ || !total.enabled) return "";
    char unit[48];
    std::snprintf(unit, sizeof unit, " (total, %llu VBlanks)", static_cast<unsigned long long>(vblank));
    return formatTexCache(total, gs::TexCacheReport{}, 1.0, unit);
}

std::string HostProfile::texSimInterval(const gs::TexSimReport& total, std::uint64_t vblank) {
    if (!enabled_) return "";
    const std::uint64_t n = vblank > texSimMarkVblank ? vblank - texSimMarkVblank : 1;
    gs::TexSimReport& mark = texSimMark();
    const std::string s = formatTexSim(total, mark, static_cast<double>(n), " (por VBlank)");
    mark = total;
    texSimMarkVblank = vblank;
    return s;
}

std::string HostProfile::texSimReport(const gs::TexSimReport& total, std::uint64_t vblank) {
    if (!enabled_) return "";
    char unit[48];
    std::snprintf(unit, sizeof unit, " (total, %llu VBlanks)", static_cast<unsigned long long>(vblank));
    return formatTexSim(total, gs::TexSimReport{}, 1.0, unit);
}

std::string HostProfile::texelsInterval(const gs::TexReport& total, std::uint64_t vblank) {
    if (!enabled_) return "";
    const std::uint64_t n = vblank > texelsMarkVblank ? vblank - texelsMarkVblank : 1;
    gs::TexReport& mark = texelsMark();
    const std::string s = formatTexels(total, mark, static_cast<double>(n), " (por VBlank)");
    mark = total;
    texelsMarkVblank = vblank;
    return s;
}

std::string HostProfile::texelsReport(const gs::TexReport& total, std::uint64_t vblank) {
    if (!enabled_) return "";
    char unit[48];
    std::snprintf(unit, sizeof unit, " (total, %llu VBlanks)", static_cast<unsigned long long>(vblank));
    return formatTexels(total, gs::TexReport{}, 1.0, unit);
}

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
