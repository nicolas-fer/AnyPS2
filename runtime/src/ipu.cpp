#include "anyps2/runtime/ipu.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "anyps2/common/error.h"
#include "anyps2/runtime/dmac.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/ipu_mpeg.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

namespace {

constexpr unsigned kIntcIpu = 8;
constexpr unsigned kDmaFromIpu = 3, kDmaToIpu = 4;

enum Command : unsigned { BCLR, IDEC, BDEC, VDEC, FDEC, SETIQ, SETVQ, CSC, PACK, SETTH };

const char* commandName(unsigned c) {
    static const char* kNames[] = {"BCLR", "IDEC", "BDEC", "VDEC", "FDEC", "SETIQ",
                                   "SETVQ", "CSC", "PACK", "SETTH"};
    return c < 10 ? kNames[c] : "?";
}

// CTRL: CBP (13..8), ECD (14), SCD (15), IDP (17..16), AS (20), IVF (21),
// QST (22), MP1 (23), PCT (26..24), RST (30).
constexpr std::uint32_t kCtrlCbp = 0x3Fu << 8, kCtrlEcd = 1u << 14, kCtrlScd = 1u << 15;
constexpr std::uint32_t kCtrlAs = 1u << 20, kCtrlIvf = 1u << 21, kCtrlQst = 1u << 22, kCtrlMp1 = 1u << 23;
constexpr std::uint32_t kCtrlRst = 1u << 30;
// Bits graváveis do CTRL: IDP, AS, IVF, QST, MP1, PCT e RST.
constexpr std::uint32_t kCtrlWritable = 0x47F30000u;
// O que o reset preserva: CBP e os modos de decodificação.
constexpr std::uint32_t kCtrlKeptOnReset = 0x07F33F00u;

// Saturação dos coeficientes depois da quantização inversa.
int saturate(int v) {
    return std::clamp(v, -2048, 2047);
}

// Valor de `bits` bits em complemento de dois.
int signExtend(std::uint32_t v, unsigned bits) {
    const std::uint32_t sign = 1u << (bits - 1);
    return static_cast<int>(v ^ sign) - static_cast<int>(sign);
}

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
    fifo_.clear();
    out_.clear();
    popped_.clear();
    restartable_ = waitingOutput_ = false;
    th_[0] = th_[1] = 0;
    ctrl_ &= kCtrlKeptOnReset;
    data_ = top_ = 0;
    busy_ = false;
    pos_ = 0;
    if (rt_) rt_->kernel().raiseIntc(kIntcIpu);
    requestData();
}

// ---- Fluxo de bits ----------------------------------------------------------

unsigned Ipu::inputCount() const {
    return static_cast<unsigned>(std::min<std::size_t>(fifo_.size(), kFifoQw));
}

bool Ipu::popFifo(Qword& qw) {
    if (fifo_.empty()) return false;
    qw = fifo_.front();
    fifo_.pop_front();
    if (restartable_) popped_.push_back(qw);
    requestData();
    return true;
}

void Ipu::requestData() {
    if (rt_) rt_->dmac().resumeChannel(kDmaToIpu, pc_);
}

bool Ipu::fill(unsigned bits) {
    while (fp_ * 128 < bp_ + bits) {
        Qword qw;
        if (!popFifo(qw)) return false;
        std::memcpy(internal_ + fp_ * 16, qw.data(), 16);
        ++fp_;
    }
    return true;
}

std::uint32_t Ipu::peek(unsigned bits) const {
    // Os bits saem do mais significativo de cada byte, na ordem da memória.
    const unsigned at = bp_ / 8, shift = bp_ % 8;
    std::uint64_t w = 0;
    for (unsigned i = 0; i < 5; ++i) w = (w << 8) | (at + i < sizeof internal_ ? internal_[at + i] : 0u);
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
            Qword qw;
            fp_ = 0;
            if (popFifo(qw)) {
                std::memcpy(internal_, qw.data(), 16);
                fp_ = 1;
            }
        }
    }
}

std::uint32_t Ipu::show(unsigned bits) {
    if (!fill(bits)) throw NeedData{};
    return peek(bits);
}

std::uint32_t Ipu::get(unsigned bits) {
    const std::uint32_t v = show(bits);
    advance(bits);
    return v;
}

void Ipu::skip(unsigned bits) {
    if (!fill(bits)) throw NeedData{};
    advance(bits);
}

// ---- FIFOs ------------------------------------------------------------------

bool Ipu::fifoWrite(const std::uint8_t (&qw)[16], std::uint32_t pc) {
    if (dmaWrite(qw, 1) == 0) return false;
    kick(pc);
    return true;
}

std::uint32_t Ipu::dmaWrite(const std::uint8_t* data, std::uint32_t qwc) {
    std::uint32_t n = 0;
    for (; n < qwc && fifo_.size() < kFifoQw; ++n) {
        Qword q;
        std::memcpy(q.data(), data + 16 * n, 16);
        fifo_.push_back(q);
    }
    return n;
}

void Ipu::kick(std::uint32_t pc) {
    pc_ = pc;
    if (busy_ && !running_) run(pc);
}

bool Ipu::fifoRead(std::uint8_t (&qw)[16], std::uint32_t) {
    if (out_.empty()) {
        std::memset(qw, 0, 16);  // FIFO vazio: o programa devia ter olhado OFC
        return false;
    }
    std::memcpy(qw, out_.front().data(), 16);
    out_.pop_front();
    if (waitingOutput_ && out_.size() <= kFifoQw) {
        waitingOutput_ = false;
        finishCommand();
    }
    return true;
}

void Ipu::pushOutput(const std::uint8_t* data, std::size_t qwords) {
    for (std::size_t i = 0; i < qwords; ++i) {
        Qword q;
        std::memcpy(q.data(), data + 16 * i, 16);
        out_.push_back(q);
    }
    // O DMA fromIPU pausado esvazia o que puder.
    if (rt_) rt_->dmac().resumeChannel(kDmaFromIpu, pc_);
}

// ---- Registradores ----------------------------------------------------------

std::uint64_t Ipu::readRegister(std::uint32_t addr, std::uint32_t pc) {
    pc_ = pc;
    const unsigned cmd = cmd_ >> 28;
    switch (addr & 0xF0) {
        case 0x00: {  // IPU_CMD: DATA (31..0), BUSY (63)
            // Fora do FDEC/VDEC, DATA mostra os próximos 32 bits do fluxo.
            if (cmd != FDEC && cmd != VDEC && !busy_ && fill(32)) data_ = peek(32);
            const bool dataBusy = busy_ && (cmd == FDEC || cmd == VDEC);
            return data_ | (std::uint64_t{dataBusy} << 63);
        }
        case 0x10: {  // IPU_CTRL
            const auto ofc = static_cast<std::uint32_t>(std::min<std::size_t>(out_.size(), kFifoQw));
            return (ctrl_ & ~0x800000FFu) | inputCount() | (ofc << 4) | (std::uint32_t{busy_} << 31);
        }
        case 0x20:  // IPU_BP: BP, IFC (11..8), FP (17..16)
            return bp_ | (inputCount() << 8) | (fp_ << 16);
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
    pc_ = pc;
    switch (addr & 0xF0) {
        case 0x00:
            startCommand(value, pc);
            return;
        case 0x10:
            ctrl_ = (value & kCtrlWritable) | (ctrl_ & 0x0000FFFFu);
            // IDP = 3 é inválido; o hardware decodifica como 9 bits.
            if (((ctrl_ >> 16) & 3) == 3) ctrl_ &= ~(2u << 16);
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
        std::fprintf(stderr, "[ipu] %s %08x (BP=%u FP=%u IFC=%zu)\n", commandName(c), cmd, bp_, fp_,
                     fifo_.size());
    }
    switch (c) {
        case BCLR:
            std::memset(internal_, 0, sizeof internal_);
            fp_ = 0;
            fifo_.clear();
            bp_ = cmd & 0x7F;
            finishCommand();
            requestData();  // FIFO vazio: o DMA toIPU volta a enchê-lo
            return;
        case SETTH:
            th_[0] = static_cast<std::uint16_t>(cmd & 0x1FF);
            th_[1] = static_cast<std::uint16_t>((cmd >> 16) & 0x1FF);
            finishCommand();
            return;
        case VDEC:
        case BDEC:
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

void Ipu::run(std::uint32_t pc) {
    pc_ = pc;
    running_ = true;
    const unsigned c = cmd_ >> 28;
    // FB (bits 5..0): bits pulados antes do comando (FDEC e SETIQ).
    auto skipFb = [&] {
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
            if (skipFb() && fill(32)) {
                data_ = top_ = peek(32);
                done = true;
            }
            break;
        case SETIQ: {
            // IQM (bit 27): 0 = matriz intra, 1 = não intra. 64 bytes.
            std::uint8_t* m = (cmd_ & (1u << 27)) ? niq_.data() : iq_.data();
            if (!skipFb()) break;
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
        case VDEC:
        case BDEC: {
            if (waitingOutput_) break;  // já decodificado; a saída ainda não coube
            // Tentativa do começo: sem dados, tudo volta como estava.
            std::memcpy(snap_.internal, internal_, sizeof internal_);
            snap_.bp = bp_;
            snap_.fp = fp_;
            std::copy(std::begin(dcPred_), std::end(dcPred_), snap_.dcPred);
            snap_.ctrl = ctrl_;
            popped_.clear();
            restartable_ = true;
            try {
                if (c == VDEC) runVdec(pc);
                else runBdec(pc);
                restartable_ = false;
                popped_.clear();
                waitingOutput_ = out_.size() > kFifoQw;
                done = !waitingOutput_;
            } catch (const NeedData&) {
                restartable_ = false;
                std::memcpy(internal_, snap_.internal, sizeof internal_);
                bp_ = snap_.bp;
                fp_ = snap_.fp;
                std::copy(std::begin(snap_.dcPred), std::end(snap_.dcPred), dcPred_);
                ctrl_ = snap_.ctrl;
                fifo_.insert(fifo_.begin(), popped_.begin(), popped_.end());
                popped_.clear();
            }
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

// ---- VDEC -------------------------------------------------------------------

// Decodifica um código de comprimento variável. DATA = valor | (bits << 16),
// com as particularidades de cada tabela; 0 (e ECD) para código inválido.
void Ipu::runVdec(std::uint32_t pc) {
    using mpeg::Table;
    skip(cmd_ & 0x3F);
    std::uint32_t d = 0;
    switch ((cmd_ >> 26) & 3) {
        case 0: {  // macroblock_address_increment
            const mpeg::Vlc v = mpeg::decode(Table::Mbai, show(16) << 16);
            if (v.len == 0) break;
            if (v.value == mpeg::kMbaiStuffing && !(ctrl_ & kCtrlMp1)) break;  // só no MPEG-1
            skip(v.len);
            // Escape e stuffing voltam como 0x23 e 0x22.
            const int value = v.value == mpeg::kMbaiEscape ? 0x23 : v.value == mpeg::kMbaiStuffing ? 0x22 : v.value;
            d = static_cast<std::uint32_t>(value) | (v.len << 16);
            break;
        }
        case 1: {  // macroblock_type do tipo de imagem em CTRL.PCT (0 = I)
            const unsigned pct = (ctrl_ >> 24) & 7;
            constexpr std::uint32_t kMcFrame = 2u << 6;  // frame_motion_type = quadro
            switch (pct == 0 ? 1 : pct) {
                case 1: {
                    const mpeg::Vlc v = mpeg::decode(Table::MbTypeI, show(16) << 16);
                    if (v.len == 0) break;
                    skip(v.len);
                    d = static_cast<std::uint32_t>(v.value);
                    break;
                }
                case 2: {
                    const mpeg::Vlc v = mpeg::decode(Table::MbTypeP, show(16) << 16);
                    if (v.len == 0) break;
                    skip(v.len);
                    d = static_cast<std::uint32_t>(v.value) | ((v.value & mpeg::kMbForward) ? kMcFrame : 0);
                    break;
                }
                case 3: {
                    const mpeg::Vlc v = mpeg::decode(Table::MbTypeB, show(16) << 16);
                    if (v.len == 0) break;
                    skip(v.len);
                    d = static_cast<std::uint32_t>(v.value) | kMcFrame | (v.len << 16);
                    break;
                }
                default:
                    throw Unimplemented("IPU: VDEC do tipo de macrobloco de imagem D (CTRL.PCT=" +
                                            std::to_string(pct) + ") ainda não suportado",
                                        pc);
            }
            break;
        }
        case 2: {  // motion_code e o sinal (sinal e comprimento como no hardware)
            if (show(1)) {
                skip(1);
                d = 1u << 16;
                break;
            }
            const mpeg::Vlc v = mpeg::decode(Table::MotionCode, show(16) << 16);
            if (v.len == 0) break;
            skip(v.len);
            const int value = get(1) ? -v.value : v.value;
            d = static_cast<std::uint32_t>(value) | (v.len << 16);
            break;
        }
        default: {  // dmvector
            const mpeg::Vlc v = mpeg::decode(Table::DmVector, show(2) << 30);
            skip(v.len);
            d = static_cast<std::uint32_t>(v.value) | (v.len << 16);
            break;
        }
    }
    const std::uint32_t top = show(32);
    data_ = d;
    top_ = top;
    if (d == 0) ctrl_ |= kCtrlEcd;
}

// ---- BDEC -------------------------------------------------------------------

void Ipu::intraBlock(unsigned cc, std::int32_t (&coef)[64], int quantizerScale) {
    const unsigned idp = (ctrl_ >> 16) & 3;
    const mpeg::Vlc size = mpeg::decode(cc == 0 ? mpeg::Table::DcSizeLuma : mpeg::Table::DcSizeChroma, show(16) << 16);
    skip(size.len);  // as tabelas de tamanho do DC são completas: sempre há código
    int diff = 0;
    if (size.value > 0) {
        const auto n = static_cast<unsigned>(size.value);
        diff = static_cast<int>(get(n));
        if (!(diff >> (n - 1))) diff -= (1 << n) - 1;
    }
    dcPred_[cc] += diff;
    coef[0] = dcPred_[cc] << (3 - idp);
    const bool tableOne = (ctrl_ & kCtrlIvf) != 0;
    const std::uint8_t* scan = (ctrl_ & kCtrlAs) ? mpeg::kAlternate : mpeg::kZigzag;
    for (unsigned i = 1;; ++i) {
        const mpeg::Dct d = mpeg::decodeDct(tableOne, false, show(16) << 16);
        if (d.kind == mpeg::Dct::Invalid) {
            ctrl_ |= kCtrlEcd;
            return;
        }
        skip(d.len);
        if (d.kind == mpeg::Dct::Eob) return;
        i += d.kind == mpeg::Dct::Escape ? get(6) : d.run;
        if (i >= 64) return;
        int val;
        if (d.kind == mpeg::Dct::Escape) {
            // A matriz é indexada pela posição na varredura (como o IPU).
            val = (signExtend(get(12), 12) * quantizerScale * iq_[i]) >> 4;
        } else {
            val = (d.level * quantizerScale * iq_[i]) >> 4;
            if (get(1)) val = -val;
        }
        coef[scan[i]] = saturate(val);
    }
}

void Ipu::nonIntraBlock(std::int32_t (&coef)[64], int quantizerScale) {
    const std::uint8_t* scan = (ctrl_ & kCtrlAs) ? mpeg::kAlternate : mpeg::kZigzag;
    for (unsigned i = 0;; ++i) {
        const mpeg::Dct d = mpeg::decodeDct(false, i == 0, show(16) << 16);
        if (d.kind == mpeg::Dct::Invalid) {
            ctrl_ |= kCtrlEcd;
            return;
        }
        skip(d.len);
        if (d.kind == mpeg::Dct::Eob) return;
        i += d.kind == mpeg::Dct::Escape ? get(6) : d.run;
        if (i >= 64) return;
        int val;
        if (d.kind == mpeg::Dct::Escape) {
            const int level = signExtend(get(12), 12);
            val = ((2 * (level + (level < 0 ? -1 : 0)) + 1) * quantizerScale * niq_[i]) >> 5;
        } else {
            val = ((2 * d.level + 1) * quantizerScale * niq_[i]) >> 5;
            if (get(1)) val = -val;
        }
        coef[scan[i]] = saturate(val);
    }
}

void Ipu::checkStartCode() {
    // Os próximos 8 bits zerados: alinha ao byte e procura um start code
    // (00 00 01). Achou, SCD; outro dado no lugar, ECD.
    if (show(8) != 0) return;
    skip((8 - bp_ % 8) % 8);
    for (;;) {
        const std::uint32_t v = show(24);
        if (v != 0) {
            ctrl_ |= v == 1 ? kCtrlScd : kCtrlEcd;
            return;
        }
        skip(8);
    }
}

// Decodifica um macrobloco: 4 blocos de luminância e 2 de crominância, em
// RAW16 (Y 16×16, Cb 8×8, Cr 8×8; 16 bits com sinal, little-endian).
// Intra: 0–255 depois da IDCT; não intra: o resíduo da IDCT.
void Ipu::runBdec(std::uint32_t pc) {
    if (ctrl_ & kCtrlMp1) throw Unimplemented("IPU: BDEC de MPEG-1 (CTRL.MP1) ainda não suportado", pc);
    skip(cmd_ & 0x3F);
    const unsigned qsc = (cmd_ >> 16) & 0x1F;
    const bool fieldDct = ((cmd_ >> 25) & 1) != 0, resetDc = ((cmd_ >> 26) & 1) != 0, intra = ((cmd_ >> 27) & 1) != 0;
    const int quantizerScale =
        (ctrl_ & kCtrlQst) ? mpeg::kNonLinearQuantizerScale[qsc] : static_cast<int>(qsc * 2);
    if (resetDc) {
        const unsigned idp = (ctrl_ >> 16) & 3;
        for (int& p : dcPred_) p = 128 << idp;
    }
    unsigned cbp = 0x3F;
    if (!intra) {
        const mpeg::Vlc v = mpeg::decode(mpeg::Table::Cbp, show(16) << 16);
        if (v.len == 0) {
            ctrl_ |= kCtrlEcd;
            cbp = 0;
        } else {
            skip(v.len);
            cbp = static_cast<unsigned>(v.value);
        }
    }
    std::int16_t mb[384] = {};  // Y[16][16], Cb[8][8], Cr[8][8]
    for (unsigned b = 0; b < 6; ++b) {
        if (!(cbp & (0x20u >> b))) continue;
        std::int32_t coef[64] = {}, pix[64];
        if (intra) intraBlock(b < 4 ? 0 : b - 3, coef, quantizerScale);
        else nonIntraBlock(coef, quantizerScale);
        mpeg::idct(coef, pix);
        for (unsigned r = 0; r < 8; ++r) {
            std::int16_t* row;
            if (b < 4) {
                // DCT de campo: os blocos de cima e de baixo intercalam as linhas.
                const unsigned line = fieldDct ? (b >> 1) + 2 * r : (b >> 1) * 8 + r;
                row = mb + line * 16 + (b & 1) * 8;
            } else {
                row = mb + 256 + (b - 4) * 64 + r * 8;
            }
            for (unsigned x = 0; x < 8; ++x) {
                const std::int32_t v = pix[r * 8 + x];
                row[x] = static_cast<std::int16_t>(intra ? std::clamp(v, 0, 255) : std::clamp(v, -32768, 32767));
            }
        }
    }
    checkStartCode();
    top_ = show(32);
    ctrl_ = (ctrl_ & ~kCtrlCbp) | (cbp << 8);
    std::uint8_t bytes[768];
    for (unsigned i = 0; i < 384; ++i) {
        const auto v = static_cast<std::uint16_t>(mb[i]);
        bytes[2 * i] = static_cast<std::uint8_t>(v);
        bytes[2 * i + 1] = static_cast<std::uint8_t>(v >> 8);
    }
    pushOutput(bytes, 48);
}

}  // namespace anyps2::rt
