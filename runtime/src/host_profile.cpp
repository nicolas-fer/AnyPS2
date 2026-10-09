#include "anyps2/runtime/host_profile.h"

#include <cstdio>

namespace anyps2::rt {

void HostProfile::enable() {
    enabled_ = true;
    current_ = Ee;
    start_ = since_ = Clock::now();
    for (auto& n : ns_) n = 0;
    for (auto& n : mark_) n = 0;
    markVblank_ = 0;
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
    static const char* kNames[Count] = {"EE (código e HLE)", "GIF", "GS (desenho)", "VU0", "VU1", "IPU"};
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
    return s;
}

std::string HostProfile::interval(std::uint64_t vblank) {
    if (!enabled_) return "";
    charge(Clock::now());
    static const char* kShort[Count] = {"EE", "GIF", "GS", "VU0", "VU1", "IPU"};
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
    markVblank_ = vblank;
    return s + "\n";
}

}  // namespace anyps2::rt
