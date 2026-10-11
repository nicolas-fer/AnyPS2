#include "anyps2/runtime/host_profile.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "anyps2/runtime/gs/gs.h"

namespace anyps2::rt {

void HostProfile::enable() {
    enabled_ = true;
    const std::int64_t now = nowNs();
    start_ = Clock::now();
    for (Track& t : tracks_) {
        t.current = Ee;
        t.sinceNs = now;
        for (auto& n : t.ns) n = 0;
        for (auto& n : t.mark) n = 0;
    }
    // O consumidor (se já houver) começa de novo em Idle.
    tracks_[1].current = Idle;
    markVblank_ = 0;
    workerNs_ = 0;
    workerMark_ = 0;
    bandsMarkVblank_ = 0;
    // Quem mapeou a memória do VU1 com contador (vu1MemCounted) o fez antes: preserva.
    const bool memCounted = sync_.vu1MemCounted;
    sync_ = SyncStats{};
    sync_.vu1MemCounted = memCounted;
    consSync_ = SyncStats{};
    syncMarkVblank_ = 0;
    batchOpen_ = false;
    batchMscal_ = batchXgkick_ = 0;
    lastStartValid_ = false;
}

void HostProfile::bindConsumerThread() {
    track_ = &tracks_[1];
    tracks_[1].current = Idle;
    tracks_[1].sinceNs = nowNs();
}

void HostProfile::charge(Track& t, std::int64_t now) {
    const std::int64_t since = t.sinceNs.load(std::memory_order_relaxed);
    const unsigned cur = t.current.load(std::memory_order_relaxed);
    t.ns[cur].store(t.ns[cur].load(std::memory_order_relaxed) + (now - since), std::memory_order_relaxed);
    t.sinceNs.store(now, std::memory_order_relaxed);
}

void HostProfile::snapshot(const Track& t, std::int64_t (&out)[Count]) {
    const std::int64_t since = t.sinceNs.load(std::memory_order_relaxed);
    const unsigned cur = t.current.load(std::memory_order_relaxed);
    const std::int64_t open = nowNs() - since;
    for (unsigned i = 0; i < Count; ++i) {
        out[i] = t.ns[i].load(std::memory_order_relaxed) + (i == cur && open > 0 ? open : 0);
    }
}

void HostProfile::Scope::enter(Part p) {
    Track& t = *track_;
    charge(t, nowNs());
    previous_ = static_cast<Part>(t.current.load(std::memory_order_relaxed));
    t.current.store(p, std::memory_order_relaxed);
    active_ = true;
}

void HostProfile::Scope::leave() {
    Track& t = *track_;
    charge(t, nowNs());
    t.current.store(previous_, std::memory_order_relaxed);
}

namespace {
// Partes da trilha do EE e da trilha do consumidor do VU1, na ordem do relatório.
constexpr HostProfile::Part kEeParts[] = {HostProfile::Ee,    HostProfile::Gif,  HostProfile::Gs,
                                          HostProfile::GsWait, HostProfile::Vu0, HostProfile::Vu1,
                                          HostProfile::Ipu,   HostProfile::Vif,  HostProfile::VuWait,
                                          HostProfile::RingFull};
constexpr HostProfile::Part kConsParts[] = {HostProfile::Item, HostProfile::Gif, HostProfile::Gs,
                                            HostProfile::GsWait, HostProfile::Vu1, HostProfile::Vif,
                                            HostProfile::Idle};
}  // namespace

std::string HostProfile::report() {
    if (!enabled_) return "";
    static const char* kNames[Count] = {"EE (código e HLE)", "GIF", "GS (desenho no EE)", "GS (EE esperando o worker)",
                                        "VU0", "VU1", "IPU", "VIF (parse e UNPACK)",
                                        "espera pelo VU1 (fence)", "anel cheio",
                                        "ocioso", "item (outros)"};
    static const char* kConsNames[Count] = {"", "GIF", "GS (produtor)", "GS (esperando o worker)", "", "VU1", "",
                                            "VIF (UNPACK e MPG)", "", "", "ocioso (anel vazio)", "item (outros)"};
    std::int64_t ns[Count];
    snapshot(tracks_[0], ns);
    std::int64_t total = 0;
    for (auto n : ns) total += n;
    std::string s;
    char line[160];
    std::snprintf(line, sizeof line, "[perfil] tempo do host: %.2f s%s\n", static_cast<double>(total) / 1e9,
                  threaded_ ? " (thread do EE)" : "");
    s += line;
    for (const Part i : kEeParts) {
        if ((i == VuWait || i == RingFull) && !threaded_) continue;
        std::snprintf(line, sizeof line, "[perfil] %6.2f%%  %8.2f s  %s\n",
                      total ? 100.0 * static_cast<double>(ns[i]) / static_cast<double>(total) : 0.0,
                      static_cast<double>(ns[i]) / 1e9, kNames[i]);
        s += line;
    }
    if (threaded_) {
        std::int64_t cs[Count];
        snapshot(tracks_[1], cs);
        std::int64_t ctotal = 0;
        for (auto n : cs) ctotal += n;
        std::snprintf(line, sizeof line, "[perfil] thread do VU1: %.2f s\n", static_cast<double>(ctotal) / 1e9);
        s += line;
        for (const Part i : kConsParts) {
            std::snprintf(line, sizeof line, "[perfil] %6.2f%%  %8.2f s  %s\n",
                          ctotal ? 100.0 * static_cast<double>(cs[i]) / static_cast<double>(ctotal) : 0.0,
                          static_cast<double>(cs[i]) / 1e9, kConsNames[i]);
            s += line;
        }
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
    static const char* kShort[Count] = {"EE", "GIF", "GS", "GSesp", "VU0", "VU1", "IPU", "VIF",
                                        "VUesp", "Anel", "ocioso", "item"};
    Track& t0 = tracks_[0];
    std::int64_t cur[Count];
    snapshot(t0, cur);
    std::int64_t total = 0;
    for (unsigned i = 0; i < Count; ++i) total += cur[i] - t0.mark[i];
    char line[160];
    std::snprintf(line, sizeof line, "[perfil] VBlank %llu-%llu: %.1f VBlanks/s;",
                  static_cast<unsigned long long>(markVblank_), static_cast<unsigned long long>(vblank),
                  total ? static_cast<double>(vblank - markVblank_) * 1e9 / static_cast<double>(total) : 0.0);
    std::string s = line;
    for (const Part i : kEeParts) {
        if ((i == VuWait || i == RingFull) && !threaded_) continue;
        std::snprintf(line, sizeof line, " %s %.0f%%", kShort[i],
                      total ? 100.0 * static_cast<double>(cur[i] - t0.mark[i]) / static_cast<double>(total) : 0.0);
        s += line;
    }
    for (unsigned i = 0; i < Count; ++i) t0.mark[i] = cur[i];
    const std::int64_t worker = workerNs_.load() - workerMark_;
    workerMark_ += worker;
    const double lanes = static_cast<double>(workerLanes_.load());
    std::snprintf(line, sizeof line, "; GS por faixa %.0f%%",
                  total ? 100.0 * static_cast<double>(worker) / (lanes * static_cast<double>(total)) : 0.0);
    s += line;
    s += "\n";
    if (threaded_) {
        // Thread do VU1: percentagem do tempo de parede do trecho (o mesmo `total` do EE).
        Track& t1 = tracks_[1];
        std::int64_t cs[Count];
        snapshot(t1, cs);
        s += "[perfil] thread do VU1:";
        for (const Part i : kConsParts) {
            std::snprintf(line, sizeof line, " %s %.0f%%", kShort[i],
                          total ? 100.0 * static_cast<double>(cs[i] - t1.mark[i]) / static_cast<double>(total) : 0.0);
            s += line;
        }
        for (unsigned i = 0; i < Count; ++i) t1.mark[i] = cs[i];
        s += "\n";
    }
    markVblank_ = vblank;
    return s;
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

// Linha "[perfil] clut": cargas de CLUT que viraram barreira (classificação da fatia 1 do
// plano), distância issued−done e espera das faixas por tipo de barreira.
std::string formatClut(const BandStats& cur, const BandStats& prev, double per, const char* unit) {
    auto d = [&](std::uint64_t c, std::uint64_t p) { return static_cast<double>(diff(c, p)) / per; };
    char buf[768];
    std::string s = "[perfil] clut";
    s += unit;
    s += ":";
    std::snprintf(buf, sizeof buf,
                  " diferidas (sem barreira) %.1f, cargas em barreira %.1f: cobertas %.1f, recentes por faixas 1:%.1f 2:%.1f 3:%.1f 4+:%.1f (sem faixa %.1f),"
                  " com bloco verdadeiro %.1f, HOST %.1f, perdidas %.1f, WAR depois %.1f (das cobertas %.1f);",
                  d(cur.clutLazy, prev.clutLazy), d(cur.clutBarriers, prev.clutBarriers), d(cur.clutCovered, prev.clutCovered),
                  d(cur.clutRecent[1], prev.clutRecent[1]), d(cur.clutRecent[2], prev.clutRecent[2]),
                  d(cur.clutRecent[3], prev.clutRecent[3]), d(cur.clutRecent[4], prev.clutRecent[4]),
                  d(cur.clutRecent[0], prev.clutRecent[0]), d(cur.clutRecentTrueBlock, prev.clutRecentTrueBlock),
                  d(cur.clutHost, prev.clutHost), d(cur.clutLost, prev.clutLost),
                  d(cur.clutWarAfter, prev.clutWarAfter), d(cur.clutWarAfterCovered, prev.clutWarAfterCovered));
    s += buf;
    s += " issued−done na carga (<2^k):";
    for (unsigned i = 0; i < BandStats::kLagBuckets; ++i) {
        const double n = d(cur.clutLag[i], prev.clutLag[i]);
        if (n <= 0) continue;
        if (i == 0) {
            std::snprintf(buf, sizeof buf, " 0:%.1f", n);
        } else {
            std::snprintf(buf, sizeof buf, " <%llu:%.1f", 1ull << i, n);
        }
        s += buf;
    }
    static const char* kKind[BandStats::BkCount] = {"CLUT", "HOST", "vídeo", "conflito", "reset", "LOCAL→LOCAL", "desenho", "outros"};
    s += "; barreiras (n; espera somada das faixas em ms; média por barreira em µs):";
    for (unsigned k = 0; k < BandStats::BkCount; ++k) {
        const double n = d(cur.barrierKind[k], prev.barrierKind[k]);
        const double ns = static_cast<double>(cur.barrierWaitNs[k] - prev.barrierWaitNs[k]) / per;
        std::snprintf(buf, sizeof buf, " %s %.1f/%.2f/%.1f%s", kKind[k], n, ns / 1e6, n > 0 ? ns / n / 1e3 : 0.0,
                      k + 1 < BandStats::BkCount ? "," : ";");
        s += buf;
    }
    s += " espera por faixa (ms):";
    const unsigned lanes = std::min(cur.lanes, BandStats::kLaneSlots);
    for (unsigned l = 0; l < lanes; ++l) {
        std::snprintf(buf, sizeof buf, " %.2f", static_cast<double>(cur.laneWaitNs[l] - prev.laneWaitNs[l]) / 1e6 / per);
        s += buf;
    }
    return s + "\n";
}

// `per`: divisor das contagens (VBlanks do trecho; 1 no total).
std::string formatBands(const BandStats& cur, const BandStats& prev, double per, const char* unit) {
    auto d = [&](std::uint64_t c, std::uint64_t p) { return static_cast<double>(diff(c, p)) / per; };
    char buf[768];  // texto em UTF-8 com setas de 3 bytes e muitos campos: folga para não cortar
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
                  "espera de ordem %.1f; HOST→LOCAL na fila: transferências %.1f, submissões %.1f;"
                  " quadros de vídeo na fila %.1f, esperas de atraso %.1f;",
                  d(cur.submit[0], prev.submit[0]), d(cur.submit[1], prev.submit[1]), d(cur.submit[2], prev.submit[2]),
                  d(cur.submit[3], prev.submit[3]), d(cur.clutDirect, prev.clutDirect),
                  d(cur.hostDirect, prev.hostDirect), d(cur.hostBarrier, prev.hostBarrier),
                  d(cur.hostOrder, prev.hostOrder), d(cur.hostQueued, prev.hostQueued),
                  d(cur.hostQueuedSubmits, prev.hostQueuedSubmits), d(cur.displayQueued, prev.displayQueued),
                  d(cur.displayLagWaits, prev.displayLagWaits));
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
    return s + "\n" + formatClut(cur, prev, per, unit);
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

// ---------------------------------------------------------------------------
// Medição "vu1 sync"
// ---------------------------------------------------------------------------

namespace anyps2::rt {

namespace {

unsigned log2Bucket(std::uint64_t v) {
    unsigned b = 0;
    while (v && b < SyncStats::kBuckets - 1) {
        v >>= 1;
        ++b;
    }
    return b;  // v < 2^b
}

std::string fmtNs(std::uint64_t ns) {
    char b[32];
    if (ns < 1000) std::snprintf(b, sizeof b, "%lluns", static_cast<unsigned long long>(ns));
    else if (ns < 1000000) std::snprintf(b, sizeof b, "%lluµs", static_cast<unsigned long long>(ns / 1000));
    else std::snprintf(b, sizeof b, "%llums", static_cast<unsigned long long>(ns / 1000000));
    return b;
}

// Histograma em potências de 2 do trecho (cur − prev); `ns` formata o limite como tempo.
std::string formatHist(const std::uint64_t (&cur)[SyncStats::kBuckets], const std::uint64_t (&prev)[SyncStats::kBuckets],
                       double per, bool ns) {
    std::string s;
    char buf[48];
    for (unsigned i = 0; i < SyncStats::kBuckets; ++i) {
        const std::uint64_t n = diff(cur[i], prev[i]);
        if (!n) continue;
        const std::uint64_t bound = std::uint64_t{1} << i;
        if (ns) {
            std::snprintf(buf, sizeof buf, " <%s:%.2f", fmtNs(bound).c_str(), static_cast<double>(n) / per);
        } else {
            std::snprintf(buf, sizeof buf, " <%llu:%.2f", static_cast<unsigned long long>(bound),
                          static_cast<double>(n) / per);
        }
        s += buf;
    }
    return s.empty() ? " -" : s;
}

std::string formatSync(const SyncStats& cur, const SyncStats& prev, double per, const char* unit) {
    auto d = [&](std::uint64_t c, std::uint64_t p) { return static_cast<double>(diff(c, p)) / per; };
    char buf[1024];
    std::string s = "[perfil] vu1 sync: ";
    s += unit;
    // Pontos de sincronia potenciais.
    std::snprintf(buf, sizeof buf,
                  "; pontos de fence: leituras do GIF %.1f, leituras privilegiadas do GS: CSR %.1f SIGLBLID %.1f outras %.1f,",
                  d(cur.gifReads, prev.gifReads), d(cur.gsCsrReads, prev.gsCsrReads),
                  d(cur.gsSiglblidReads, prev.gsSiglblidReads), d(cur.gsOtherPrivReads, prev.gsOtherPrivReads));
    s += buf;
    if (cur.vu1MemCounted) {
        std::snprintf(buf, sizeof buf, " memória do VU1 pelo EE: leituras %.1f escritas %.1f,",
                      d(cur.vu1MemReads, prev.vu1MemReads), d(cur.vu1MemWrites, prev.vu1MemWrites));
    } else {
        std::snprintf(buf, sizeof buf, " memória do VU1 pelo EE: não contada (ANYPS2_VU1_MEMCOUNT=1),");
    }
    s += buf;
    std::snprintf(buf, sizeof buf,
                  " VU0 tocando o VU1 %.1f, CFC2 VPU_STAT %.1f FBRST %.1f CMSAR1 %.1f, CTC2 CMSAR1 %.1f FBRST %.1f,"
                  " downloads do GS: DMA do VIF1 %.1f VIF1_FIFO %.1f LOCAL→HOST %.1f,"
                  " ROW com MODE≠0 %.1f (de %.1f leituras), MSCNT %.1f;",
                  d(cur.vu0Cross, prev.vu0Cross), d(cur.cfc2Vpustat, prev.cfc2Vpustat),
                  d(cur.cfc2Fbrst, prev.cfc2Fbrst), d(cur.cfc2Cmsar1, prev.cfc2Cmsar1),
                  d(cur.ctc2Cmsar1, prev.ctc2Cmsar1), d(cur.ctc2Fbrst, prev.ctc2Fbrst),
                  d(cur.gsDownloadDma, prev.gsDownloadDma), d(cur.gsDownloadFifo, prev.gsDownloadFifo),
                  d(cur.gsLocalToHost, prev.gsLocalToHost), d(cur.rowReadsMode, prev.rowReadsMode),
                  d(cur.rowReads, prev.rowReads), d(cur.mscnt, prev.mscnt));
    s += buf;
    // Lotes.
    const double batches = static_cast<double>(diff(cur.batches, prev.batches));
    auto perBatch = [&](std::uint64_t c, std::uint64_t p) {
        return batches > 0 ? static_cast<double>(diff(c, p)) / batches : 0.0;
    };
    std::snprintf(buf, sizeof buf,
                  " lotes: inícios de DMA do VIF1 %.1f, do GIF (PATH3) %.1f (+%.1f quadwords no GIF_FIFO),"
                  " lotes fechados %.1f, MSCAL/MSCALF/MSCNT por lote %.2f (máx %llu; %.1f no total),"
                  " XGKICK por lote %.2f (máx %llu; %.1f no total);",
                  d(cur.vif1Starts, prev.vif1Starts), d(cur.gifDmaStarts, prev.gifDmaStarts),
                  d(cur.gifFifoWrites, prev.gifFifoWrites), d(cur.batches, prev.batches),
                  perBatch(cur.batchMscal, prev.batchMscal), static_cast<unsigned long long>(cur.batchMscalMax),
                  d(cur.mscalTotal, prev.mscalTotal), perBatch(cur.batchXgkick, prev.batchXgkick),
                  static_cast<unsigned long long>(cur.batchXgkickMax), d(cur.xgkickTotal, prev.xgkickTotal));
    s += buf;
    s += " distância entre inícios do VIF1 (ciclos do EE, n por VBlank):" +
         formatHist(cur.gapHist, prev.gapHist, per, false) + ";";
    s += " tempo de host por lote (n por VBlank):" + formatHist(cur.hostHist, prev.hostHist, per, true) + ";";
    // Eventos do GS: escritas por origem e entregas ao CSR.
    static const char* kEv[SyncStats::EvCount] = {"SIGNAL", "FINISH", "LABEL"};
    s += " eventos do GS (escritas por origem priv/PATH1/PATH2/PATH3/outra):";
    for (unsigned e = 0; e < SyncStats::EvCount; ++e) {
        s += " ";
        s += kEv[e];
        for (unsigned k = 0; k < SyncStats::kSources; ++k) {
            std::snprintf(buf, sizeof buf, "%c%.1f", k ? '/' : ' ', d(cur.evWrites[e][k], prev.evWrites[e][k]));
            s += buf;
        }
        s += e + 1 < SyncStats::EvCount ? "," : ";";
    }
    static const char* kRaise[SyncStats::RaiseCount] = {"SIGNAL", "FINISH", "VSINT"};
    s += " entregues ao CSR (interrupção/só bit/já ligado):";
    for (unsigned r = 0; r < SyncStats::RaiseCount; ++r) {
        std::snprintf(buf, sizeof buf, " %s %.1f/%.1f/%.1f%s", kRaise[r], d(cur.evRaises[r][0], prev.evRaises[r][0]),
                      d(cur.evRaises[r][1], prev.evRaises[r][1]), d(cur.evRaises[r][2], prev.evRaises[r][2]),
                      r + 1 < SyncStats::RaiseCount ? "," : "");
        s += buf;
    }
    return s + "\n";
}

SyncStats& syncMark() {
    static SyncStats mark;
    return mark;
}

}  // namespace

std::string HostProfile::syncInterval(std::uint64_t vblank) {
    if (!enabled_) return "";
    const std::uint64_t n = vblank > syncMarkVblank_ ? vblank - syncMarkVblank_ : 1;
    SyncStats& mark = syncMark();
    char unit[48];
    std::snprintf(unit, sizeof unit, "por VBlank (%llu VBlanks)", static_cast<unsigned long long>(n));
    const SyncStats cur = merged();
    const std::string s = formatSync(cur, mark, static_cast<double>(n), unit);
    mark = cur;
    syncMarkVblank_ = vblank;
    return s;
}

std::string HostProfile::syncReport(std::uint64_t vblank) {
    if (!enabled_) return "";
    char unit[48];
    std::snprintf(unit, sizeof unit, "total (%llu VBlanks)", static_cast<unsigned long long>(vblank));
    return formatSync(merged(), SyncStats{}, 1.0, unit);
}

// Os contadores do EE somados aos do consumidor (só chamado com o consumidor ocioso: depois de
// um fence do EE, ou depois do join).
SyncStats HostProfile::merged() {
    SyncStats m = sync_;
    m.mscalTotal += consSync_.mscalTotal;
    m.mscnt += consSync_.mscnt;
    m.xgkickTotal += consSync_.xgkickTotal;
    for (unsigned e = 0; e < SyncStats::EvCount; ++e) {
        for (unsigned k = 0; k < SyncStats::kSources; ++k) m.evWrites[e][k] += consSync_.evWrites[e][k];
    }
    return m;
}

void HostProfile::noteMscal(bool isMscnt) {
    if (!enabled_) return;
    if (onConsumer()) {
        // O lote do VIF1 é do EE: o consumidor só conta o total.
        ++consSync_.mscalTotal;
        if (isMscnt) ++consSync_.mscnt;
        return;
    }
    ++sync_.mscalTotal;
    if (isMscnt) ++sync_.mscnt;
    if (batchOpen_) ++batchMscal_;
}

void HostProfile::noteXgkick() {
    if (!enabled_) return;
    if (onConsumer()) {
        ++consSync_.xgkickTotal;
        return;
    }
    ++sync_.xgkickTotal;
    if (batchOpen_) ++batchXgkick_;
}

void HostProfile::Vif1Batch::begin(std::uint64_t eeCycles) {
    ++sync_.vif1Starts;
    if (lastStartValid_) ++sync_.gapHist[log2Bucket(eeCycles >= lastStart_ ? eeCycles - lastStart_ : 0)];
    lastStartValid_ = true;
    lastStart_ = eeCycles;
    batchOpen_ = true;
    batchMscal_ = batchXgkick_ = 0;
    active_ = true;
    t0_ = Clock::now();
}

void HostProfile::Vif1Batch::end() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0_).count();
    ++sync_.hostHist[log2Bucket(ns > 0 ? static_cast<std::uint64_t>(ns) : 0)];
    ++sync_.batches;
    sync_.batchMscal += batchMscal_;
    sync_.batchXgkick += batchXgkick_;
    sync_.batchMscalMax = std::max(sync_.batchMscalMax, batchMscal_);
    sync_.batchXgkickMax = std::max(sync_.batchXgkickMax, batchXgkick_);
    batchOpen_ = false;
}

}  // namespace anyps2::rt
