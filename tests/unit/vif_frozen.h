#pragma once
// CÓPIA CONGELADA do Vif de antes da separação em decodificação e execução (commit 81c10bf,
// runtime/src/vif.cpp e vif.h), renomeada FrozenVif e com duas adaptações ao Vu novo (marcadas
// ADAPTAÇÃO): o VU1 lê o TOP/ITOP que quem o inicia entrega. Serve de oráculo do teste
// diferencial (test_vif_split.cpp); não edite a lógica.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "anyps2/common/error.h"
#include "anyps2/runtime/dmac.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/host_profile.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/vif.h"
#include "anyps2/runtime/vu/vu.h"

namespace anyps2::rt::frozen {

class FrozenVif {
public:
    FrozenVif(Runtime* rt, unsigned unit, VuMemory& vu, Gif* gif);

    // Processa dados (múltiplo de 4 bytes) vindos do DMA. Retorna quantos
    // bytes foram aceitos: tudo, ou — se o VIF parou — até o fim do quadword
    // em que parou (o resto dele fica no FIFO); 0 se já estava parado.
    std::size_t transfer(const std::uint8_t* data, std::size_t bytes, std::uint32_t pc);
    // Escrita no VIFn_FIFO pelo EE: com o VIF parado, os dados esperam no FIFO.
    void fifoWrite(const std::uint8_t* data, std::size_t bytes, std::uint32_t pc);
    // DMA com TTE: os 64 bits altos do DMAtag passam pelo VIF.
    void transferTag(std::uint64_t upper, std::uint32_t pc);
    // Parado (bit I, STOP ou ForceBreak) até FBRST.STC.
    bool stalled() const;

    std::uint32_t readRegister(std::uint32_t addr, std::uint32_t pc);
    void writeRegister(std::uint32_t addr, std::uint32_t value, std::uint32_t pc);
    void reset();
    bool idle() const { return state_ == State::Idle; }
    std::uint32_t top() const { return top_; }
    std::uint32_t itop() const { return itop_; }

private:
    enum class State { Idle, Mask, Row, Col, Mpg, Direct, Unpack };

    void word(std::uint32_t w, std::uint32_t pc);
    void command(std::uint32_t w, std::uint32_t pc);
    void unpackWord(std::uint32_t w, std::uint32_t pc);
    void writeUnpackedVector(const std::uint32_t (&v)[4], std::uint32_t pc);
    void writeFillVectors(std::uint32_t pc);
    std::uint8_t* dataMem() const;
    std::uint32_t dataSize() const;
    std::uint8_t* microMem() const;
    std::uint32_t microSize() const;

    Runtime* rt_;
    unsigned unit_;
    VuMemory& vu_;
    Gif* gif_;

    State state_ = State::Idle;
    std::uint32_t code_ = 0;
    // Últimos VIFcodes aceitos, para a mensagem de VIFcode inválido.
    std::uint32_t history_[8] = {};
    unsigned historyPos_ = 0;
    bool irqPending_ = false;           // comando corrente tem o bit I
    std::vector<std::uint8_t> fifo_;    // dados recebidos e ainda não processados (parado)
    void stallOnIrq();
    void resume(std::uint32_t pc);
    std::uint32_t remaining_ = 0;  // palavras (ou quadwords no DIRECT) restantes
    unsigned index_ = 0;
    // MPG
    std::uint32_t mpgAddr_ = 0;
    // DIRECT
    std::uint8_t directBuf_[16] = {};
    unsigned directFill_ = 0;
    // UNPACK
    unsigned upVn_ = 0, upVl_ = 0;
    bool upMask_ = false, upUsn_ = false;
    std::uint32_t upAddr_ = 0;     // quadword de destino base
    std::uint32_t upNum_ = 0;      // vetores restantes a gravar
    std::uint32_t upReadLeft_ = 0;  // vetores restantes a ler do stream
    bool upFill_ = false;          // escrita de preenchimento (CL < WL)
    unsigned upCl_ = 0, upWl_ = 0;
    std::uint32_t upIndex_ = 0;    // vetores já escritos
    std::uint32_t upComp_[4] = {};  // componentes já lidos do vetor corrente
    unsigned upNbits_ = 0;
    std::uint32_t upWordsLeft_ = 0;

    // Registradores
    std::uint32_t stat_ = 0, err_ = 0, mark_ = 0, cycle_ = 0, mode_ = 0, num_ = 0, mask_ = 0;
    std::uint32_t itops_ = 0, base_ = 0, ofst_ = 0, tops_ = 0, itop_ = 0, top_ = 0;
    std::uint32_t row_[4] = {}, col_[4] = {};
};


namespace frozen_detail {
constexpr std::uint32_t kStatMrk = 1u << 6;
constexpr std::uint32_t kStatDbf = 1u << 7;
constexpr std::uint32_t kStatVss = 1u << 8;   // parado por STOP
constexpr std::uint32_t kStatVfs = 1u << 9;   // parado por ForceBreak
constexpr std::uint32_t kStatVis = 1u << 10;  // parado pelo bit I
constexpr std::uint32_t kStatInt = 1u << 11;  // interrupção do bit I
constexpr std::uint32_t kStatEr0 = 1u << 12, kStatEr1 = 1u << 13;
constexpr std::uint32_t kStatFdr = 1u << 23;  // VIF1: sentido VIF → memória (download do GS)
constexpr std::uint32_t kStatStall = kStatVss | kStatVfs | kStatVis;
constexpr std::uint32_t kErrMii = 1;  // ERR.MII: ignora o bit I

std::string cmdName(std::uint32_t cmd) {
    switch (cmd & 0x7F) {
        case 0x00: return "NOP";
        case 0x01: return "STCYCL";
        case 0x02: return "OFFSET";
        case 0x03: return "BASE";
        case 0x04: return "ITOP";
        case 0x05: return "STMOD";
        case 0x06: return "MSKPATH3";
        case 0x07: return "MARK";
        case 0x10: return "FLUSHE";
        case 0x11: return "FLUSH";
        case 0x13: return "FLUSHA";
        case 0x14: return "MSCAL";
        case 0x15: return "MSCALF";
        case 0x17: return "MSCNT";
        case 0x20: return "STMASK";
        case 0x30: return "STROW";
        case 0x31: return "STCOL";
        case 0x4A: return "MPG";
        case 0x50: return "DIRECT";
        case 0x51: return "DIRECTHL";
        default:
            if ((cmd & 0x60) == 0x60) return "UNPACK";
            return "?";
    }
}
}  // namespace frozen_detail
using namespace frozen_detail;

inline FrozenVif::FrozenVif(Runtime* rt, unsigned unit, VuMemory& vu, Gif* gif) : rt_(rt), unit_(unit), vu_(vu), gif_(gif) {}

inline void FrozenVif::reset() {
    state_ = State::Idle;
    irqPending_ = false;
    fifo_.clear();
    remaining_ = 0;
    directFill_ = 0;
    stat_ = err_ = mark_ = cycle_ = mode_ = num_ = mask_ = 0;
    itops_ = base_ = ofst_ = tops_ = itop_ = top_ = 0;
    std::memset(row_, 0, sizeof(row_));
    std::memset(col_, 0, sizeof(col_));
    if (unit_ == 1 && gif_) gif_->setPath3Masked(false);
    if (unit_ == 1 && rt_) rt_->vu1().setVifTop(0, 0);  // ADAPTAÇÃO, ver MSCAL
}

inline std::uint8_t* FrozenVif::dataMem() const { return unit_ == 0 ? vu_.data0.get() : vu_.data1.get(); }
inline std::uint32_t FrozenVif::dataSize() const { return unit_ == 0 ? VuMemory::kVu0Size : VuMemory::kVu1Size; }
inline std::uint8_t* FrozenVif::microMem() const { return unit_ == 0 ? vu_.micro0.get() : vu_.micro1.get(); }
inline std::uint32_t FrozenVif::microSize() const { return unit_ == 0 ? VuMemory::kVu0Size : VuMemory::kVu1Size; }

inline bool FrozenVif::stalled() const { return (stat_ & kStatStall) != 0; }

inline std::size_t FrozenVif::transfer(const std::uint8_t* data, std::size_t bytes, std::uint32_t pc) {
    if (stalled()) return 0;
    // Parse e UNPACK saem do balde do EE; o VU e o GIF chamados daqui abrem escopos mais internos.
    const HostProfile::Scope prof(HostProfile::Vif);
    for (std::size_t off = 0; off + 4 <= bytes; off += 4) {
        std::uint32_t w;
        std::memcpy(&w, data + off, 4);
        word(w, pc);
        if (stalled()) {
            // O quadword em que parou já entrou no FIFO; o resto dele espera.
            const std::size_t end = std::min(bytes, (off + 4 + 15) & ~std::size_t{15});
            fifo_.insert(fifo_.end(), data + off + 4, data + end);
            return end;
        }
    }
    return bytes;
}

inline void FrozenVif::fifoWrite(const std::uint8_t* data, std::size_t bytes, std::uint32_t pc) {
    if (stalled()) {
        fifo_.insert(fifo_.end(), data, data + bytes);
        return;
    }
    transfer(data, bytes, pc);
}

inline void FrozenVif::transferTag(std::uint64_t upper, std::uint32_t pc) {
    std::uint8_t b[8];
    std::memcpy(b, &upper, 8);
    transfer(b, 8, pc);
}

inline void FrozenVif::stallOnIrq() {
    irqPending_ = false;
    stat_ |= kStatVis | kStatInt;
    if (rt_) rt_->kernel().raiseIntc(unit_ == 0 ? 4 : 5);
}

// FBRST.STC: sai da parada, processa o que esperava no FIFO e, se não parar
// de novo, o DMA do canal continua.
inline void FrozenVif::resume(std::uint32_t pc) {
    stat_ &= ~(kStatStall | kStatInt | kStatEr0 | kStatEr1);
    std::vector<std::uint8_t> pending;
    pending.swap(fifo_);
    std::size_t off = 0;
    {
        const HostProfile::Scope prof(HostProfile::Vif);
        for (; off + 4 <= pending.size() && !stalled(); off += 4) {
            std::uint32_t w;
            std::memcpy(&w, pending.data() + off, 4);
            word(w, pc);
        }
    }
    if (stalled()) {
        fifo_.insert(fifo_.begin(), pending.begin() + static_cast<std::ptrdiff_t>(off), pending.end());
        return;
    }
    if (rt_) rt_->dmac().resumeChannel(unit_, pc);
}

inline void FrozenVif::word(std::uint32_t w, std::uint32_t pc) {
    switch (state_) {
        case State::Idle:
            command(w, pc);
            break;
        case State::Mask:
            mask_ = w;
            state_ = State::Idle;
            break;
        case State::Row:
        case State::Col:
            (state_ == State::Row ? row_ : col_)[index_++] = w;
            if (index_ == 4) state_ = State::Idle;
            break;
        case State::Mpg: {
            std::uint8_t* m = microMem();
            std::memcpy(m + (mpgAddr_ & (microSize() - 4)), &w, 4);
            mpgAddr_ += 4;
            if (--remaining_ == 0) state_ = State::Idle;
            break;
        }
        case State::Direct:
            std::memcpy(directBuf_ + directFill_, &w, 4);
            directFill_ += 4;
            if (directFill_ == 16) {
                directFill_ = 0;
                gif_->transfer(2, directBuf_, 1, pc);
                if (--remaining_ == 0) state_ = State::Idle;
            }
            break;
        case State::Unpack:
            unpackWord(w, pc);
            break;
    }
    // Bit I: a interrupção vem ao fim do comando (com os dados dele).
    if (irqPending_ && state_ == State::Idle) stallOnIrq();
}

inline void FrozenVif::command(std::uint32_t w, std::uint32_t pc) {
    code_ = w;
    const std::uint32_t imm = w & 0xFFFF;
    const std::uint32_t num = (w >> 16) & 0xFF;
    const std::uint32_t cmd = w >> 24;
    const std::string unitName = "VIF" + std::to_string(unit_);
    if (rt_ && rt_->options().traceGs) {
        std::fprintf(stderr, "[vif%u] %s (0x%08x)\n", unit_, cmdName(cmd).c_str(), w);
    }
    history_[historyPos_++ % 8] = w;
    if ((cmd & 0x80) && !(err_ & kErrMii)) irqPending_ = true;
    const bool vif1 = unit_ == 1;
    switch (cmd & 0x7F) {
        case 0x00: return;
        case 0x01: cycle_ = imm; return;
        case 0x02:
            if (!vif1) break;
            ofst_ = imm & 0x3FF;
            stat_ &= ~kStatDbf;
            tops_ = base_;
            return;
        case 0x03:
            if (!vif1) break;
            base_ = imm & 0x3FF;
            return;
        case 0x04: itops_ = imm & 0x3FF; return;
        case 0x05: mode_ = imm & 3; return;
        case 0x06:
            if (!vif1) break;
            gif_->setPath3Masked((imm & 0x8000) != 0);
            return;
        case 0x07:
            mark_ = imm;
            stat_ |= kStatMrk;
            return;
        case 0x10: case 0x11: case 0x13:
            if (!vif1 && (cmd & 0x7F) != 0x10) break;
            return;  // VU e caminhos do GIF sempre ociosos (execução síncrona)
        case 0x14: case 0x15: case 0x17: {  // MSCAL, MSCALF, MSCNT
            if (!rt_) throw Unimplemented(unitName + ": " + cmdName(cmd) + " sem runtime", pc);
            itop_ = itops_;
            if (vif1) HostProfile::noteMscal((cmd & 0x7F) == 0x17);
            if (vif1) {
                // Double buffering: o VU1 lê TOP; o próximo UNPACK com FLG vai para o outro buffer.
                top_ = tops_;
                tops_ = (stat_ & kStatDbf) ? base_ : base_ + ofst_;
                stat_ ^= kStatDbf;
            }
            Vu& vu = vif1 ? rt_->vu1() : rt_->vu0();
            const std::uint32_t start = (cmd & 0x7F) == 0x17 ? vu.regs().vi[vucore::reg::TPC] * 8 : imm * 8;
            // ADAPTAÇÃO ao Vu novo (única diferença de comportamento do original): o VU1 não consulta mais
            // o VIF1; o TOP/ITOP vem de quem o inicia.
            if (vif1) vu.startVif(start, pc, top_, itop_, rt_->vblanks());
            else vu.start(start, pc);
            return;
        }
        case 0x20: state_ = State::Mask; return;
        case 0x30: state_ = State::Row; index_ = 0; return;
        case 0x31: state_ = State::Col; index_ = 0; return;
        case 0x4A:
            mpgAddr_ = imm * 8;
            remaining_ = (num == 0 ? 256u : num) * 2;
            state_ = State::Mpg;
            return;
        case 0x50: case 0x51:
            if (!vif1) break;
            remaining_ = imm == 0 ? 65536u : imm;
            directFill_ = 0;
            state_ = State::Direct;
            return;
        default:
            if ((cmd & 0x60) == 0x60) {
                upVn_ = (cmd >> 2) & 3;
                upVl_ = cmd & 3;
                if (upVl_ == 3 && upVn_ != 3) break;  // só V4-5 usa vl=3
                upMask_ = (cmd >> 4) & 1;
                upUsn_ = (imm >> 14) & 1;
                upAddr_ = imm & 0x3FF;
                if (vif1 && (imm & 0x8000)) upAddr_ += tops_;
                upNum_ = num == 0 ? 256 : num;
                upIndex_ = 0;
                upNbits_ = 0;
                // NUM conta os vetores gravados. Escrita de pulo (CL >= WL):
                // lê NUM vetores e pula CL-WL quadwords a cada WL. Escrita de
                // preenchimento (CL < WL): lê só os CL primeiros de cada WL
                // (CL=0: nenhum) e grava os WL em quadwords seguidos.
                // Compara os valores crus: CYCLE=0 (nunca escrito) é escrita
                // normal; CL=0 com WL>0 é preenchimento sem leitura.
                upCl_ = cycle_ & 0xFF;
                upWl_ = (cycle_ >> 8) & 0xFF;
                upFill_ = upWl_ > upCl_;
                if (upFill_) {
                    upReadLeft_ = upCl_ * (upNum_ / upWl_) + std::min<std::uint32_t>(upNum_ % upWl_, upCl_);
                } else {
                    if (upCl_ == 0) upCl_ = 256;
                    if (upWl_ == 0) upWl_ = 256;
                    upReadLeft_ = upNum_;
                }
                const unsigned vbits = (upVn_ == 3 && upVl_ == 3) ? 16 : (upVn_ + 1) * (32u >> upVl_);
                upWordsLeft_ = (upReadLeft_ * vbits + 31) / 32;
                writeFillVectors(pc);
                if (upWordsLeft_ > 0) state_ = State::Unpack;
                return;
            }
            break;
    }
    std::string recent;
    for (unsigned i = 0; i < 8; ++i) {
        const std::uint32_t h = history_[(historyPos_ + i) % 8];
        if (h) recent += " " + cmdName(h >> 24) + "(" + anyps2::hex(h) + ")";
    }
    throw Unimplemented(unitName + ": VIFcode inválido " + anyps2::hex(w) + "; anteriores:" + recent, pc);
}

inline void FrozenVif::unpackWord(std::uint32_t w, std::uint32_t pc) {
    const unsigned ebits = (upVn_ == 3 && upVl_ == 3) ? 16 : (32u >> upVl_);
    const unsigned comps = (upVn_ == 3 && upVl_ == 3) ? 1 : upVn_ + 1;
    for (unsigned off = 0; off < 32 && upReadLeft_ > 0; off += ebits) {
        std::uint32_t e = ebits == 32 ? w : (w >> off) & ((1u << ebits) - 1);
        if (!upUsn_ && ebits == 16 && !(upVn_ == 3 && upVl_ == 3)) {
            e = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(e)));
        } else if (!upUsn_ && ebits == 8) {
            e = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(e)));
        }
        std::uint32_t* comp = upComp_;
        comp[upNbits_++] = e;
        if (upNbits_ == comps) {
            std::uint32_t v[4];
            if (upVn_ == 3 && upVl_ == 3) {
                const std::uint32_t c = comp[0];
                v[0] = (c & 0x1F) << 3;
                v[1] = ((c >> 5) & 0x1F) << 3;
                v[2] = ((c >> 10) & 0x1F) << 3;
                v[3] = ((c >> 15) & 1) << 7;
            } else if (comps == 1) {
                v[0] = v[1] = v[2] = v[3] = comp[0];
            } else if (comps == 2) {
                v[0] = comp[0];
                v[1] = comp[1];
                v[2] = comp[0];
                v[3] = comp[1];
            } else {
                v[0] = comp[0];
                v[1] = comp[1];
                v[2] = comp[2];
                v[3] = comps == 4 ? comp[3] : 0;
            }
            upNbits_ = 0;
            writeUnpackedVector(v, pc);
            --upNum_;
            --upReadLeft_;
            writeFillVectors(pc);
        }
    }
    if (--upWordsLeft_ == 0) state_ = State::Idle;
}

// Preenchimento: nas posições CL..WL-1 de cada ciclo nada é lido; o valor
// vem da máscara (ROW/COL). Com máscara 0 o dado é indefinido no hardware;
// aqui é zero (somado a ROW nos modos de offset).
inline void FrozenVif::writeFillVectors(std::uint32_t pc) {
    if (!upFill_) return;
    static constexpr std::uint32_t zero[4] = {0, 0, 0, 0};
    while (upNum_ > 0 && upIndex_ % upWl_ >= upCl_) {
        writeUnpackedVector(zero, pc);
        --upNum_;
    }
}

inline void FrozenVif::writeUnpackedVector(const std::uint32_t (&v)[4], std::uint32_t) {
    const std::uint32_t i = upIndex_++;
    const std::uint32_t cyclePos = i % upWl_;
    const std::uint32_t qw = upFill_ ? upAddr_ + i : upAddr_ + (i / upWl_) * upCl_ + cyclePos;
    const std::uint32_t qwords = dataSize() / 16;
    std::uint8_t* dst = dataMem() + (qw % qwords) * 16;
    const unsigned maskRow = std::min<std::uint32_t>(cyclePos, 3);
    const bool v3 = upVn_ == 2;
    for (unsigned c = 0; c < 4; ++c) {
        const unsigned m = upMask_ ? (mask_ >> ((maskRow * 4 + c) * 2)) & 3 : 0;
        std::uint32_t value;
        switch (m) {
            case 0:
                if (v3 && c == 3) continue;  // V3: W indeterminado no hardware; preservado aqui
                value = v[c];
                if (mode_ == 1) value += row_[c];
                else if (mode_ == 2) {
                    value += row_[c];
                    row_[c] = value;
                }
                break;
            case 1: value = row_[c]; break;
            case 2: value = col_[maskRow]; break;
            default: continue;  // protegido contra escrita
        }
        std::memcpy(dst + c * 4, &value, 4);
    }
}

inline std::uint32_t FrozenVif::readRegister(std::uint32_t addr, std::uint32_t pc) {
    const std::uint32_t off = addr & 0x3FF;
    switch (off) {
        case 0x00: {
            // VPS (esperando dados) e FQC (quadwords no FIFO).
            const std::uint32_t fqc = std::min<std::uint32_t>(static_cast<std::uint32_t>((fifo_.size() + 15) / 16), 16);
            return stat_ | (state_ != State::Idle ? 1u : 0u) | (fqc << 24);
        }
        case 0x10: return 0;
        case 0x20: return err_;
        case 0x30: return mark_;
        case 0x40: return cycle_;
        case 0x50: return mode_;
        case 0x60: return num_;
        case 0x70: return mask_;
        case 0x80: return code_;
        case 0x90: return itops_;
        case 0xA0: return base_;
        case 0xB0: return ofst_;
        case 0xC0: return tops_;
        case 0xD0: return itop_;
        case 0xE0: return top_;
        case 0x100: case 0x110: case 0x120: case 0x130:
            // ROW só muda com MODE 1/2: uma thread dona do VU teria de esperar para ler uma soma em dia.
            if (unit_ == 1 && HostProfile::enabled()) {
                HostProfile::count(&SyncStats::rowReads);
                if (mode_ != 0) HostProfile::count(&SyncStats::rowReadsMode);
            }
            return row_[(off - 0x100) >> 4];
        case 0x140: case 0x150: case 0x160: case 0x170: return col_[(off - 0x140) >> 4];
        default: break;
    }
    throw Unimplemented("leitura de registrador do VIF" + std::to_string(unit_) + " desconhecido " +
                            anyps2::hex(addr),
                        pc);
}

inline void FrozenVif::writeRegister(std::uint32_t addr, std::uint32_t value, std::uint32_t pc) {
    const std::uint32_t off = addr & 0x3FF;
    switch (off) {
        case 0x00:  // STAT: só o FDR (sentido VIF1 → memória) é gravável
            if (unit_ == 1) stat_ = (stat_ & ~kStatFdr) | (value & kStatFdr);
            return;
        case 0x10:  // FBRST: RST, FBK, STP, STC
            if (value & 1) {
                reset();
                return;
            }
            if (value & 6) {
                throw Unimplemented("VIF" + std::to_string(unit_) + ": FBRST " + anyps2::hex(value) +
                                        " (ForceBreak/STOP) ainda não suportado",
                                    pc);
            }
            if ((value & 8) && stalled()) resume(pc);
            else if (value & 8) stat_ &= ~(kStatInt | kStatEr0 | kStatEr1);
            return;
        case 0x20: err_ = value & 7; return;
        case 0x30:
            mark_ = value & 0xFFFF;
            stat_ &= ~kStatMrk;
            return;
        default: break;
    }
    throw Unimplemented("escrita em registrador do VIF" + std::to_string(unit_) + " " + anyps2::hex(addr) +
                            " (somente leitura ou desconhecido)",
                        pc);
}


}  // namespace anyps2::rt::frozen
