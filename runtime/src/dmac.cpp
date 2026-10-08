#include "anyps2/runtime/dmac.h"

#include <cstdio>
#include <cstring>

#include "anyps2/common/error.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/vif.h"

namespace anyps2::rt {

namespace {

constexpr std::uint32_t kBase[Dmac::kChannels] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                                  0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};
constexpr unsigned kVif0 = 0, kVif1 = 1, kGif = 2, kFromSpr = 8, kToSpr = 9;

constexpr std::uint32_t kChcrDir = 1u << 0, kChcrTte = 1u << 6, kChcrTie = 1u << 7, kChcrStr = 1u << 8;
constexpr unsigned kIntcDmac = 1;

int channelOf(std::uint32_t addr) {
    for (unsigned ch = 0; ch < Dmac::kChannels; ++ch) {
        if (addr >= kBase[ch] && addr < kBase[ch] + 0x100) return static_cast<int>(ch);
    }
    return -1;
}

const char* tagName(unsigned id) {
    static const char* kNames[] = {"refe", "cnt", "next", "ref", "refs", "call", "ret", "end"};
    return kNames[id & 7];
}

}  // namespace

const char* Dmac::channelName(unsigned ch) {
    static const char* kNames[] = {"VIF0", "VIF1", "GIF", "fromIPU", "toIPU", "SIF0", "SIF1", "SIF2", "fromSPR", "toSPR"};
    return ch < kChannels ? kNames[ch] : "?";
}

bool Dmac::handles(std::uint32_t addr) {
    return channelOf(addr) >= 0 || (addr >= 0x1000E000u && addr < 0x1000E070u) || addr == 0x1000F520u ||
           addr == 0x1000F590u;
}

Dmac::Dmac(Runtime& rt) : rt_(rt) {}

void Dmac::reset() {
    // Os canais do SIF (5–7) pertencem ao protocolo EE↔IOP, que o HLE do IOP
    // mantém; o reset do DMAC pelo programa não os derruba.
    constexpr std::uint32_t kSif = 0xE0u;
    for (unsigned ch = 0; ch < kChannels; ++ch) {
        if (!(kSif & (1u << ch))) ch_[ch] = Channel{};
    }
    ctrl_ = 1;
    stat_ &= kSif | (kSif << 16);
    pcr_ = sqwc_ = rbsr_ = rbor_ = stadr_ = 0;
    enable_ = 0x1201;
}

bool Dmac::setChannelEnabled(unsigned ch, bool enabled) {
    const std::uint32_t bit = 1u << (16 + ch);
    const bool was = (stat_ & bit) != 0;
    if (enabled) stat_ |= bit;
    else stat_ &= ~bit;
    if (pendingChannels()) rt_.kernel().raiseIntc(kIntcDmac);
    return was;
}

std::uint32_t Dmac::read(std::uint32_t addr, std::uint32_t pc) {
    const int ch = channelOf(addr);
    if (ch >= 0) {
        const Channel& c = ch_[ch];
        switch (addr - kBase[ch]) {
            case 0x00: return c.chcr;
            case 0x10: return c.madr;
            case 0x20: return c.qwc;
            case 0x30: return c.tadr;
            case 0x40: return c.asr0;
            case 0x50: return c.asr1;
            case 0x80: return c.sadr;
            default: break;
        }
    } else {
        switch (addr) {
            case 0x1000E000: return ctrl_;
            case 0x1000E010: return stat_;
            case 0x1000E020: return pcr_;
            case 0x1000E030: return sqwc_;
            case 0x1000E040: return rbsr_;
            case 0x1000E050: return rbor_;
            case 0x1000E060: return stadr_;
            case 0x1000F520: return enable_;
            default: break;
        }
    }
    throw Unimplemented("leitura de registrador do DMAC desconhecido " + anyps2::hex(addr), pc);
}

void Dmac::write(std::uint32_t addr, std::uint32_t value, std::uint32_t pc) {
    const int ch = channelOf(addr);
    if (ch >= 0) {
        Channel& c = ch_[ch];
        switch (addr - kBase[ch]) {
            case 0x00:
                c.chcr = value;
                if (value & kChcrStr) start(static_cast<unsigned>(ch), pc);
                return;
            case 0x10: c.madr = value & 0xFFFFFFF0u; return;
            case 0x20: c.qwc = value & 0xFFFF; return;
            case 0x30: c.tadr = value & 0xFFFFFFF0u; return;
            case 0x40: c.asr0 = value & 0xFFFFFFF0u; return;
            case 0x50: c.asr1 = value & 0xFFFFFFF0u; return;
            case 0x80: c.sadr = value & 0x3FF0u; return;
            default: break;
        }
    } else {
        switch (addr) {
            case 0x1000E000:
                ctrl_ = value;
                if (value & 0xC) {
                    throw Unimplemented("D_CTRL.MFD (MFIFO) ainda não suportado (valor " + anyps2::hex(value) + ")",
                                        pc);
                }
                startPending(pc);
                return;
            case 0x1000E010: {
                // Bits baixos (CIS, SIS, MEIS, BEIS): 1 limpa; altos (CIM, SIM, MEIM): 1 inverte.
                stat_ &= ~(value & 0x0000E3FFu);
                stat_ ^= value & 0x63FF0000u;
                if (pendingChannels()) rt_.kernel().raiseIntc(kIntcDmac);
                return;
            }
            case 0x1000E020: pcr_ = value; return;
            case 0x1000E030: sqwc_ = value; return;
            case 0x1000E040: rbsr_ = value; return;
            case 0x1000E050: rbor_ = value; return;
            case 0x1000E060: stadr_ = value; return;
            case 0x1000F590:
                enable_ = value;
                startPending(pc);
                return;
            default: break;
        }
    }
    throw Unimplemented("escrita em registrador do DMAC desconhecido " + anyps2::hex(addr) + " (valor " +
                            anyps2::hex(value) + ")",
                        pc);
}

void Dmac::startPending(std::uint32_t pc) {
    for (unsigned ch = 0; ch < kChannels; ++ch) {
        if (ch_[ch].pending) start(ch, pc);
    }
}

std::uint8_t* Dmac::hostAddress(std::uint32_t dmaAddr, std::uint32_t qwc, unsigned ch, std::uint32_t pc) {
    const std::uint32_t bytes = qwc * 16;
    if (dmaAddr & 0x80000000u) {
        const std::uint32_t off = dmaAddr & 0x3FF0u;
        if (off + bytes > Memory::kScratchpadSize) {
            throw GuestError(std::string("DMA ") + channelName(ch) + ": transferência de " + std::to_string(qwc) +
                                 " quadwords passa do fim do scratchpad (" + anyps2::hex(dmaAddr) + ")",
                             pc);
        }
        return rt_.memory().scratchpad() + off;
    }
    const std::uint32_t phys = dmaAddr & 0x7FFFFFF0u;
    if (phys >= Memory::kRamSize || phys + bytes > Memory::kRamSize) {
        throw GuestError(std::string("DMA ") + channelName(ch) + ": endereço " + anyps2::hex(dmaAddr) + " (+" +
                             std::to_string(qwc) + " quadwords) fora da RAM",
                         pc);
    }
    return rt_.memory().ram() + phys;
}

void Dmac::sendToDevice(unsigned ch, std::uint32_t addr, std::uint32_t qwc, std::uint32_t pc) {
    if (qwc == 0) return;
    std::uint8_t* src = hostAddress(addr, qwc, ch, pc);
    switch (ch) {
        case kVif0: rt_.vif0().transfer(src, std::size_t{qwc} * 16, pc); return;
        case kVif1: rt_.vif1().transfer(src, std::size_t{qwc} * 16, pc); return;
        case kGif:
            if (rt_.gif().path3Masked()) {
                throw Unimplemented("DMA do GIF com PATH3 mascarado pelo VIF1 (MSKPATH3) — espera não emulada", pc);
            }
            rt_.gif().transfer(3, src, qwc, pc);
            return;
        case kToSpr: {
            Channel& c = ch_[ch];
            if (c.sadr + qwc * 16 > Memory::kScratchpadSize) {
                throw GuestError("DMA toSPR passa do fim do scratchpad (SADR " + anyps2::hex(c.sadr) + ")", pc);
            }
            std::memcpy(rt_.memory().scratchpad() + c.sadr, src, std::size_t{qwc} * 16);
            c.sadr = (c.sadr + qwc * 16) & 0x3FF0u;
            return;
        }
        default:
            break;
    }
    throw Unimplemented(std::string("DMA para o canal ") + channelName(ch) + " ainda não suportado", pc);
}

void Dmac::finish(unsigned ch) {
    Channel& c = ch_[ch];
    c.chcr &= ~kChcrStr;
    c.pending = false;
    stat_ |= 1u << ch;
    if (stat_ & (1u << (16 + ch))) rt_.kernel().raiseIntc(kIntcDmac);
}

void Dmac::start(unsigned ch, std::uint32_t pc) {
    Channel& c = ch_[ch];
    if (!(ctrl_ & 1) || (enable_ & 0x10000)) {
        c.pending = true;  // começa quando o DMAC for (re)habilitado
        return;
    }
    c.pending = false;
    const unsigned mod = (c.chcr >> 2) & 3;
    if (rt_.options().traceGs) {
        std::fprintf(stderr, "[dma] %s CHCR=%08x MADR=%08x QWC=%u TADR=%08x\n", channelName(ch), c.chcr, c.madr,
                     c.qwc, c.tadr);
    }
    switch (ch) {
        case kVif0: case kGif: case kToSpr: break;
        case kVif1:
            if (!(c.chcr & kChcrDir)) {
                throw Unimplemented("DMA VIF1→memória (leitura de dados do GS) ainda não suportado", pc);
            }
            break;
        case kFromSpr:
            if (mod != 0) throw Unimplemented("DMA fromSPR em modo chain/interleave ainda não suportado", pc);
            break;
        default:
            throw Unimplemented(std::string("DMA no canal ") + channelName(ch) +
                                    " (os programas do ps2sdk usam as syscalls de SIF; IPU chega com MPEG)",
                                pc);
    }
    switch (mod) {
        case 0: runNormal(ch, pc); break;
        case 1: runChain(ch, pc); break;
        default:
            throw Unimplemented(std::string("DMA ") + channelName(ch) + " em modo interleave (MOD=" +
                                    std::to_string(mod) + ") ainda não suportado",
                                pc);
    }
    finish(ch);
}

void Dmac::runNormal(unsigned ch, std::uint32_t pc) {
    Channel& c = ch_[ch];
    if (ch == kFromSpr) {
        if (c.sadr + c.qwc * 16 > Memory::kScratchpadSize) {
            throw GuestError("DMA fromSPR passa do fim do scratchpad (SADR " + anyps2::hex(c.sadr) + ")", pc);
        }
        std::uint8_t* dst = hostAddress(c.madr, c.qwc, ch, pc);
        std::memcpy(dst, rt_.memory().scratchpad() + c.sadr, std::size_t{c.qwc} * 16);
        c.sadr = (c.sadr + c.qwc * 16) & 0x3FF0u;
    } else {
        sendToDevice(ch, c.madr, c.qwc, pc);
    }
    c.madr += c.qwc * 16;
    c.qwc = 0;
}

void Dmac::runChain(unsigned ch, std::uint32_t pc) {
    Channel& c = ch_[ch];
    if ((c.chcr & kChcrTte) && ch == kGif) {
        throw Unimplemented("DMA do GIF em chain com TTE=1 (tag transferido para o GIF) não suportado", pc);
    }
    // Se QWC já tem dados, o DMAC os transfere antes de ler o próximo tag.
    if (c.qwc > 0) {
        sendToDevice(ch, c.madr, c.qwc, pc);
        c.madr += c.qwc * 16;
        c.qwc = 0;
        const unsigned lastId = (c.chcr >> 28) & 7;
        if (lastId == 0 || lastId == 7) return;  // refe/end: a cadeia já tinha terminado
    }
    for (unsigned guard = 0;; ++guard) {
        if (guard > 1000000) {
            throw GuestError(std::string("DMA ") + channelName(ch) + ": cadeia de tags com mais de 1 milhão de "
                                 "elementos (laço infinito?), TADR " + anyps2::hex(c.tadr),
                             pc);
        }
        std::uint8_t* tp = hostAddress(c.tadr, 1, ch, pc);
        std::uint64_t tag, upper;
        std::memcpy(&tag, tp, 8);
        std::memcpy(&upper, tp + 8, 8);
        const std::uint32_t qwc = static_cast<std::uint32_t>(tag & 0xFFFF);
        const unsigned id = static_cast<unsigned>((tag >> 28) & 7);
        const bool irq = (tag >> 31) & 1;
        const std::uint32_t addr = static_cast<std::uint32_t>((tag >> 32) & 0x7FFFFFF0u) |
                                   static_cast<std::uint32_t>((tag >> 32) & 0x80000000u);
        c.chcr = (c.chcr & 0xFFFFu) | (static_cast<std::uint32_t>(tag) & 0xFFFF0000u);
        if (rt_.options().traceGs) {
            std::fprintf(stderr, "[dma] %s tag %s QWC=%u ADDR=%08x em %08x\n", channelName(ch), tagName(id), qwc,
                         addr, c.tadr);
        }
        if ((c.chcr & kChcrTte) && (ch == kVif0 || ch == kVif1)) {
            (ch == kVif0 ? rt_.vif0() : rt_.vif1()).transferTag(upper, pc);
        }
        const std::uint32_t after = c.tadr + 16;
        bool done = false;
        unsigned asp = (c.chcr >> 4) & 3;
        switch (id) {
            case 0:  // refe
                c.madr = addr;
                sendToDevice(ch, addr, qwc, pc);
                c.tadr = after;
                done = true;
                break;
            case 1:  // cnt
                c.madr = after;
                sendToDevice(ch, after, qwc, pc);
                c.tadr = after + qwc * 16;
                break;
            case 2:  // next
                c.madr = after;
                sendToDevice(ch, after, qwc, pc);
                c.tadr = addr;
                break;
            case 3: case 4:  // ref, refs
                c.madr = addr;
                sendToDevice(ch, addr, qwc, pc);
                c.tadr = after;
                break;
            case 5: {  // call
                c.madr = after;
                sendToDevice(ch, after, qwc, pc);
                const std::uint32_t ret = after + qwc * 16;
                if (asp == 0) c.asr0 = ret;
                else if (asp == 1) c.asr1 = ret;
                else {
                    throw GuestError(std::string("DMA ") + channelName(ch) +
                                         ": tag call com a pilha de endereços cheia (3 níveis)",
                                     pc);
                }
                ++asp;
                c.tadr = addr;
                break;
            }
            case 6:  // ret
                c.madr = after;
                sendToDevice(ch, after, qwc, pc);
                if (asp == 0) {
                    done = true;
                } else {
                    c.tadr = asp == 2 ? c.asr1 : c.asr0;
                    --asp;
                }
                break;
            default:  // end
                c.madr = after;
                sendToDevice(ch, after, qwc, pc);
                c.tadr = after;
                done = true;
                break;
        }
        c.madr += qwc * 16;
        c.qwc = 0;
        c.chcr = (c.chcr & ~0x30u) | (asp << 4);
        if (irq && (c.chcr & kChcrTie)) done = true;
        if (done) return;
    }
}

}  // namespace anyps2::rt
