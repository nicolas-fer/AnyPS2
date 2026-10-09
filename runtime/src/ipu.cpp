#include "anyps2/runtime/ipu.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "anyps2/common/error.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

namespace {

constexpr unsigned kIntcIpu = 8;

enum Command : unsigned { BCLR, IDEC, BDEC, VDEC, FDEC, SETIQ, SETVQ, CSC, PACK, SETTH };

const char* commandName(unsigned c) {
    static const char* kNames[] = {"BCLR", "IDEC", "BDEC", "VDEC", "FDEC", "SETIQ",
                                   "SETVQ", "CSC", "PACK", "SETTH"};
    return c < 10 ? kNames[c] : "?";
}

// CTRL: ECD (14), SCD (15), RST (30).
constexpr std::uint32_t kCtrlEcd = 1u << 14, kCtrlScd = 1u << 15, kCtrlRst = 1u << 30;
// Bits graváveis do CTRL: IDP, AS, IVF, QST, MP1, PCT e RST.
constexpr std::uint32_t kCtrlWritable = 0x47F30000u;
// O que o reset preserva: CBP e os modos de decodificação.
constexpr std::uint32_t kCtrlKeptOnReset = 0x07F33F00u;

}  // namespace

Ipu::Ipu(Runtime* rt) : rt_(rt) {}

void Ipu::reset() {
    *this = Ipu(rt_);
}

// CTRL.RST: esvazia os FIFOs e o fluxo de bits e cancela o comando; as
// matrizes de quantização ficam.
void Ipu::softReset() {
    std::memset(internal_, 0, sizeof internal_);
    bp_ = fp_ = 0;
    fifoRead_ = ifc_ = 0;
    outRead_ = ofc_ = 0;
    th_[0] = th_[1] = 0;
    ctrl_ &= kCtrlKeptOnReset;
    data_ = top_ = 0;
    busy_ = false;
    pos_ = 0;
    if (rt_) rt_->kernel().raiseIntc(kIntcIpu);
}

// ---- Fluxo de bits ----------------------------------------------------------

bool Ipu::popFifo(std::uint8_t (&qw)[16]) {
    if (ifc_ == 0) return false;
    std::memcpy(qw, fifo_[fifoRead_], 16);
    fifoRead_ = (fifoRead_ + 1) % kFifoQw;
    --ifc_;
    return true;
}

bool Ipu::fill(unsigned bits) {
    while (fp_ * 128 < bp_ + bits) {
        std::uint8_t qw[16];
        if (!popFifo(qw)) return false;
        std::memcpy(internal_ + fp_ * 16, qw, 16);
        ++fp_;
    }
    return true;
}

std::uint32_t Ipu::peek(unsigned bits) const {
    // Os bits saem do mais significativo de cada byte, na ordem da memória.
    const unsigned at = bp_ / 8, shift = bp_ % 8;
    std::uint64_t w = 0;
    for (unsigned i = 0; i < 5; ++i) w = (w << 8) | (at + i < sizeof internal_ ? internal_[at + i] : 0);
    const auto v = static_cast<std::uint32_t>(w >> (8 - shift));
    return bits >= 32 ? v : v >> (32 - bits);
}

void Ipu::advance(unsigned bits) {
    bp_ += bits;
    while (bp_ >= 128) {
        bp_ -= 128;
        if (fp_ == 2) {
            std::memcpy(internal_, internal_ + 16, 16);
            fp_ = 1;
        } else {
            // O quadword carregado acabou: o próximo vem do FIFO, se houver.
            std::uint8_t qw[16];
            fp_ = 0;
            if (popFifo(qw)) {
                std::memcpy(internal_, qw, 16);
                fp_ = 1;
            }
        }
    }
}

// ---- FIFOs ------------------------------------------------------------------

bool Ipu::fifoWrite(const std::uint8_t (&qw)[16], std::uint32_t pc) {
    if (ifc_ == kFifoQw) return false;
    std::memcpy(fifo_[(fifoRead_ + ifc_) % kFifoQw], qw, 16);
    ++ifc_;
    if (busy_ && !running_) run(pc);
    return true;
}

void Ipu::fifoRead(std::uint8_t (&qw)[16], std::uint32_t) {
    if (ofc_ == 0) {
        std::memset(qw, 0, 16);  // FIFO vazio: o programa devia ter olhado OFC
        return;
    }
    std::memcpy(qw, outFifo_[outRead_], 16);
    outRead_ = (outRead_ + 1) % kFifoQw;
    --ofc_;
}

// ---- Registradores ----------------------------------------------------------

std::uint64_t Ipu::readRegister(std::uint32_t addr, std::uint32_t) {
    const unsigned cmd = cmd_ >> 28;
    switch (addr & 0xF0) {
        case 0x00: {  // IPU_CMD: DATA (31..0), BUSY (63)
            // Fora do FDEC/VDEC, DATA mostra os próximos 32 bits do fluxo.
            if (cmd != FDEC && cmd != VDEC && !busy_ && fill(32)) data_ = peek(32);
            const bool dataBusy = busy_ && (cmd == FDEC || cmd == VDEC);
            return data_ | (std::uint64_t{dataBusy} << 63);
        }
        case 0x10:  // IPU_CTRL
            return (ctrl_ & ~0x800000FFu) | ifc_ | (ofc_ << 4) | (std::uint32_t{busy_} << 31);
        case 0x20:  // IPU_BP: BP, IFC (11..8), FP (17..16)
            return bp_ | (ifc_ << 8) | (fp_ << 16);
        case 0x30: {  // IPU_TOP: próximos 32 bits; BUSY (63) se ainda não há 32
            const bool have = !(busy_ && cmd != FDEC) && fill(32);
            if (have) top_ = peek(32);
            return top_ | (std::uint64_t{!have} << 63);
        }
        default:
            return 0;
    }
}

void Ipu::writeRegister(std::uint32_t addr, std::uint32_t value, std::uint32_t pc) {
    switch (addr & 0xF0) {
        case 0x00:
            startCommand(value, pc);
            return;
        case 0x10:
            ctrl_ = (value & kCtrlWritable) | (ctrl_ & 0x0000FFFFu);
            if (ctrl_ & kCtrlRst) softReset();
            return;
        default:
            return;  // BP e TOP são só leitura
    }
}

// ---- Comandos ---------------------------------------------------------------

void Ipu::startCommand(std::uint32_t cmd, std::uint32_t pc) {
    if (busy_) {
        throw GuestError("IPU: comando " + anyps2::hex(cmd) + " escrito com o " + commandName(cmd_ >> 28) +
                             " ainda em andamento",
                         pc);
    }
    cmd_ = cmd;
    pos_ = 0;
    ctrl_ &= ~(kCtrlEcd | kCtrlScd);
    const unsigned c = cmd >> 28;
    if (rt_ && rt_->options().traceHardware) {
        std::fprintf(stderr, "[ipu] %s %08x (BP=%u FP=%u IFC=%u)\n", commandName(c), cmd, bp_, fp_, ifc_);
    }
    switch (c) {
        case BCLR:
            std::memset(internal_, 0, sizeof internal_);
            fp_ = 0;
            fifoRead_ = ifc_ = 0;
            bp_ = cmd & 0x7F;
            finishCommand();
            return;
        case SETTH:
            th_[0] = static_cast<std::uint16_t>(cmd & 0x1FF);
            th_[1] = static_cast<std::uint16_t>((cmd >> 16) & 0x1FF);
            finishCommand();
            return;
        case FDEC:
        case SETIQ:
        case SETVQ:
            busy_ = true;
            run(pc);
            return;
        default:
            throw Unimplemented(std::string("IPU: comando ") + commandName(c) + " (" + anyps2::hex(cmd) +
                                    ") ainda não suportado",
                                pc);
    }
}

void Ipu::run(std::uint32_t) {
    running_ = true;
    const unsigned c = cmd_ >> 28;
    // FB (bits 5..0): bits pulados antes do comando (FDEC e SETIQ).
    auto skip = [&] {
        if (pos_ > 0) return true;
        const unsigned fb = cmd_ & 0x3F;
        if (!fill(fb)) return false;
        advance(fb);
        pos_ = 1;
        return true;
    };
    // Lê 8 bytes do fluxo, na ordem em que estão na memória.
    auto read8 = [&](std::uint8_t* out) {
        if (!fill(64)) return false;
        const std::uint32_t hi = peek(32);
        advance(32);
        const std::uint32_t lo = peek(32);
        advance(32);
        for (unsigned i = 0; i < 4; ++i) {
            out[i] = static_cast<std::uint8_t>(hi >> (24 - 8 * i));
            out[4 + i] = static_cast<std::uint8_t>(lo >> (24 - 8 * i));
        }
        return true;
    };
    bool done = false;
    switch (c) {
        case FDEC:
            if (skip() && fill(32)) {
                data_ = top_ = peek(32);
                done = true;
            }
            break;
        case SETIQ: {
            // IQM (bit 27): 0 = matriz intra, 1 = não intra. 64 bytes.
            std::uint8_t* m = (cmd_ & (1u << 27)) ? niq_.data() : iq_.data();
            if (!skip()) break;
            while (pos_ <= 8 && read8(m + (pos_ - 1) * 8)) ++pos_;
            done = pos_ > 8;
            break;
        }
        case SETVQ: {
            // 16 cores RGB555 (16 bits, little-endian), 32 bytes, sem FB.
            std::uint8_t b[8];
            while (pos_ < 4 && read8(b)) {
                for (unsigned i = 0; i < 4; ++i) {
                    vqclut_[pos_ * 4 + i] = static_cast<std::uint16_t>(b[2 * i] | (b[2 * i + 1] << 8));
                }
                ++pos_;
            }
            done = pos_ == 4;
            break;
        }
        default:
            break;
    }
    running_ = false;
    if (done) finishCommand();
}

void Ipu::finishCommand() {
    busy_ = false;
    if (rt_) rt_->kernel().raiseIntc(kIntcIpu);
}

}  // namespace anyps2::rt
