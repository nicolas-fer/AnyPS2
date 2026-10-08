#include "anyps2/runtime/timing.h"

#include <algorithm>
#include <limits>
#include <thread>

#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/memory.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

namespace {
constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();

// Bits do Tn_MODE
constexpr std::uint32_t kModeClks = 0x3, kModeZret = 1u << 6, kModeCue = 1u << 7,
                        kModeCmpe = 1u << 8, kModeOvfe = 1u << 9, kModeEquf = 1u << 10,
                        kModeOvff = 1u << 11;
// INTC
constexpr unsigned kCauseVblankStart = 2, kCauseVblankEnd = 3, kCauseTimer0 = 9;
}  // namespace

Timing::Timing(Runtime& rt, Mode mode) : rt_(rt), mode_(mode), start_(std::chrono::steady_clock::now()) {}

std::uint64_t Timing::now() const {
    if (mode_ == Mode::Virtual) return virtualNow_;
    using namespace std::chrono;
    const auto ns = duration_cast<nanoseconds>(steady_clock::now() - start_).count();
    return static_cast<std::uint64_t>(ns) * (kEeHz / 1000000ull) / 1000ull;
}

void Timing::consume(std::int64_t cycles) {
    if (mode_ == Mode::Virtual && cycles > 0) virtualNow_ += static_cast<std::uint64_t>(cycles);
}

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------

std::uint64_t Timing::divider(const Timer& t) const {
    switch (t.mode & kModeClks) {
        case 0: return 2;                // BUSCLK (147,456 MHz)
        case 1: return 2 * 16;           // BUSCLK/16
        case 2: return 2 * 256;          // BUSCLK/256
        default: return kCyclesPerLine;  // HBLANK
    }
}

void Timing::advanceTimer(unsigned n, std::uint64_t t) {
    Timer& tm = timers_[n];
    if (!(tm.mode & kModeCue)) {
        tm.base = t;
        return;
    }
    const std::uint64_t div = divider(tm);
    if (t <= tm.base) return;
    std::uint64_t ticks = (t - tm.base) / div;
    while (ticks > 0) {
        // Distância até o próximo evento do contador: comparação ou overflow.
        const std::uint32_t toComp = tm.count < tm.comp ? tm.comp - tm.count : 0;
        const std::uint32_t toOverflow = 0x10000u - tm.count;
        std::uint32_t step = toOverflow;
        bool hitComp = false;
        if (toComp > 0 && toComp <= toOverflow) {
            step = toComp;
            hitComp = true;
        }
        if (ticks < step) {
            tm.count += static_cast<std::uint32_t>(ticks);
            tm.base += ticks * div;
            return;
        }
        ticks -= step;
        tm.base += std::uint64_t{step} * div;
        if (hitComp) {
            tm.count = tm.comp;
            if (!(tm.mode & kModeEquf)) {
                tm.mode |= kModeEquf;
                if (tm.mode & kModeCmpe) raise(kCauseTimer0 + n);
            }
            if (tm.mode & kModeZret) tm.count = 0;
        } else {
            tm.count = 0;  // overflow 0xFFFF -> 0
            if (!(tm.mode & kModeOvff)) {
                tm.mode |= kModeOvff;
                if (tm.mode & kModeOvfe) raise(kCauseTimer0 + n);
            }
        }
    }
}

std::uint64_t Timing::timerNextEvent(unsigned n, std::uint64_t t) const {
    const Timer& tm = timers_[n];
    if (!(tm.mode & kModeCue) || !(tm.mode & (kModeCmpe | kModeOvfe))) return kNever;
    const std::uint64_t div = divider(tm);
    std::uint64_t ticks = kNever;
    if ((tm.mode & kModeCmpe) && !(tm.mode & kModeEquf)) {
        const std::uint32_t d = tm.count < tm.comp ? tm.comp - tm.count : 0x10000u - tm.count + tm.comp;
        ticks = std::min<std::uint64_t>(ticks, d == 0 ? 0x10000u : d);
    }
    if ((tm.mode & kModeOvfe) && !(tm.mode & kModeOvff)) {
        ticks = std::min<std::uint64_t>(ticks, 0x10000u - tm.count);
    }
    if (ticks == kNever) return kNever;
    return std::max(t, tm.base + ticks * div);
}

std::uint32_t Timing::readTimer(unsigned n, unsigned reg) {
    const std::uint64_t t = now();
    advanceTimer(n, t);
    const Timer& tm = timers_[n];
    switch (reg) {
        case 0: {
            if (!(tm.mode & kModeCue)) return tm.count;
            const std::uint64_t partial = (t - tm.base) / divider(tm);
            return static_cast<std::uint32_t>((tm.count + partial) & 0xFFFF);
        }
        case 1: return tm.mode;
        case 2: return tm.comp;
        default: return tm.hold;
    }
}

void Timing::writeTimer(unsigned n, unsigned reg, std::uint32_t value) {
    const std::uint64_t t = now();
    advanceTimer(n, t);
    Timer& tm = timers_[n];
    switch (reg) {
        case 0:
            tm.count = value & 0xFFFF;
            tm.base = t;
            break;
        case 1: {
            // EQUF/OVFF: escrever 1 limpa; demais bits são escritos.
            const std::uint32_t flags = tm.mode & (kModeEquf | kModeOvff) & ~value;
            const bool wasCounting = (tm.mode & kModeCue) != 0;
            tm.mode = (value & 0x3FF) | flags;
            if (!wasCounting && (tm.mode & kModeCue)) tm.base = t;
            break;
        }
        case 2:
            tm.comp = value & 0xFFFF;
            break;
        default:
            tm.hold = value & 0xFFFF;
            break;
    }
}

// ---------------------------------------------------------------------------
// GS e VBlank
// ---------------------------------------------------------------------------

std::uint64_t Timing::readGsCsr() {
    process(0);
    return rt_.gs().csr();
}

void Timing::writeGsCsr(std::uint64_t value) {
    rt_.gs().writePrivileged(0x12001000u, value, 0);
}

// ---------------------------------------------------------------------------
// Alarmes
// ---------------------------------------------------------------------------

std::int32_t Timing::setAlarm(std::uint16_t lines, std::uint32_t handler, std::uint32_t arg, std::uint32_t gp) {
    if (alarms_.size() >= 64) return -1;
    const std::int32_t id = nextAlarmId_++;
    alarms_.push_back({id, now() + std::uint64_t{lines} * kCyclesPerLine, lines, handler, arg, gp});
    return id;
}

bool Timing::releaseAlarm(std::int32_t id) {
    const auto before = alarms_.size();
    std::erase_if(alarms_, [id](const Alarm& a) { return a.id == id; });
    return alarms_.size() < before;
}

// ---------------------------------------------------------------------------
// Eventos
// ---------------------------------------------------------------------------

void Timing::raise(unsigned cause) {
    rt_.kernel().raiseIntc(cause);
}

void Timing::process(std::uint32_t pc) {
    const std::uint64_t t = now();
    // Se o host ficou parado muito tempo (modo real), não recupera centenas
    // de quadros: pula para perto do presente.
    if (nextVblankStart_ + 8 * kCyclesPerField < t) {
        const std::uint64_t skip = (t - nextVblankStart_) / kCyclesPerField - 1;
        nextVblankStart_ += skip * kCyclesPerField;
        nextVblankEnd_ += skip * kCyclesPerField;
    }
    while (nextVblankStart_ <= t || nextVblankEnd_ <= t) {
        if (nextVblankStart_ <= nextVblankEnd_) {
            ++vblanks_;
            rt_.gs().vblankStart();
            if (vsyncFlag_) {
                Memory& m = rt_.memory();
                m.write<std::uint32_t>(vsyncFlag_, 1, 0);
                if (vsyncCsr_) m.write<std::uint64_t>(vsyncCsr_, rt_.gs().csr(), 0);
            }
            raise(kCauseVblankStart);
            rt_.onVblank(pc);
            nextVblankStart_ += kCyclesPerField;
        } else {
            raise(kCauseVblankEnd);
            nextVblankEnd_ += kCyclesPerField;
        }
    }
    rt_.gs().processEvents(t);
    for (unsigned n = 0; n < 4; ++n) advanceTimer(n, t);
    if (!alarms_.empty()) {
        std::vector<Alarm> due;
        for (const auto& a : alarms_) {
            if (a.due <= t) due.push_back(a);
        }
        std::erase_if(alarms_, [t](const Alarm& a) { return a.due <= t; });
        std::sort(due.begin(), due.end(), [](const Alarm& a, const Alarm& b) { return a.due < b.due; });
        for (const auto& a : due) rt_.kernel().queueAlarm(a);
    }
}

std::uint64_t Timing::nextEventTime() const {
    const std::uint64_t t = now();
    std::uint64_t next = std::min(nextVblankStart_, nextVblankEnd_);
    for (unsigned n = 0; n < 4; ++n) next = std::min(next, timerNextEvent(n, t));
    for (const auto& a : alarms_) next = std::min(next, a.due);
    next = std::min(next, rt_.gs().nextEventTime());
    return next;
}

std::uint64_t Timing::cyclesUntilNextEvent() const {
    const std::uint64_t t = now();
    const std::uint64_t next = nextEventTime();
    return next <= t ? 0 : next - t;
}

bool Timing::advanceToNextEvent(std::uint32_t pc) {
    const std::uint64_t next = nextEventTime();
    if (next == kNever) return false;
    if (mode_ == Mode::Virtual) {
        virtualNow_ = std::max(virtualNow_, next);
    } else {
        const std::uint64_t t = now();
        if (next > t) {
            const auto ns = std::chrono::nanoseconds((next - t) * 1000ull / (kEeHz / 1000000ull));
            std::this_thread::sleep_for(ns);
        }
    }
    process(pc);
    return true;
}

}  // namespace anyps2::rt
