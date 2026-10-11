#include "anyps2/runtime/gs/gs_events.h"

#include <cstddef>

#include "anyps2/runtime/host_profile.h"

namespace anyps2::rt::gs {

namespace {
constexpr std::uint64_t kNoEvent = ~std::uint64_t{0};
}  // namespace

void GsEeState::raise(unsigned bit) {
    const std::uint64_t mask = 1ull << bit;
    if (csr_ & mask) {
        HostProfile::gsRaise(bit, SyncStats::OutAlreadySet);
        return;
    }
    csr_ |= mask;
    // IMR: bits 8–12 mascaram SIGNAL, FINISH, HSINT, VSINT, EDWINT.
    const bool interrupt = !(imr_ & (1ull << (bit + 8)));
    HostProfile::gsRaise(bit, interrupt ? SyncStats::OutInterrupt : SyncStats::OutCsrOnly);
    if (interrupt && irq_) irq_(irqCtx_);  // INTC_GS
}

void GsEeState::apply(const GsEvent& e) {
    switch (e.kind) {
        case GsEvent::Kind::Raise:
            raise(e.bit);
            break;
        case GsEvent::Kind::SigLblId:
            siglblid_ = (siglblid_ & ~e.mask) | e.value;
            break;
        case GsEvent::Kind::FinishDue:
            finishDue_.push_back(e.due);
            break;
        case GsEvent::Kind::Vblank:
            csr_ ^= kCsrFieldMask;
            raise(kCsrVsintBit);
            break;
        case GsEvent::Kind::Reset:
            csr_ &= kCsrFieldMask;
            finishDue_.clear();
            break;
    }
}

std::uint64_t GsEeState::csr(std::uint64_t now) {
    if (hasClock_ && cyclesPerLine_ && !(csr_ & (1ull << kCsrHsintBit))) {
        if (now / cyclesPerLine_ > hsyncClearedAt_ / cyclesPerLine_) csr_ |= 1ull << kCsrHsintBit;
    }
    return csr_ | kCsrConstBits;
}

void GsEeState::writeCsr(std::uint64_t value, std::uint64_t now) {
    // SIGNAL/FINISH/HSINT/VSINT/EDWINT: escrever 1 limpa.
    if (value & (1ull << kCsrHsintBit) && hasClock_) hsyncClearedAt_ = now;
    csr_ &= ~(value & 0x1F);
}

std::uint64_t GsEeState::nextEventTime() const {
    return finishDue_.empty() ? kNoEvent : finishDue_.front();
}

void GsEeState::processEvents(std::uint64_t now) {
    std::size_t n = 0;
    while (n < finishDue_.size() && finishDue_[n] <= now) ++n;
    if (n == 0) return;
    if (realTime_) {
        // Relógio real: o GS em software leva tempo de verdade, e dois FINISH
        // separados no hardware podem vencer juntos aqui — uma única limpeza
        // do CSR apagaria os dois e quem espera o segundo esperaria para
        // sempre. No relógio real o FINISH espera o worker, então o desenho já
        // terminou e um FINISH adiantado não faz mal: entrega um por vez, o
        // próximo depois que o programa limpar o anterior.
        if (csr_ & (1ull << kCsrFinishBit)) return;
        finishDue_.erase(finishDue_.begin());
        raise(kCsrFinishBit);
        return;
    }
    finishDue_.erase(finishDue_.begin(), finishDue_.begin() + static_cast<std::ptrdiff_t>(n));
    raise(kCsrFinishBit);
}

}  // namespace anyps2::rt::gs
