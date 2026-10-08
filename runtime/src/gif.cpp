#include "anyps2/runtime/gif.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "anyps2/common/error.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

namespace {
constexpr std::uint64_t bits(std::uint64_t v, unsigned lo, unsigned n) {
    return (v >> lo) & ((1ull << n) - 1);
}
constexpr unsigned kPacked = 0, kReglist = 1;
}  // namespace

Gif::Gif(Runtime* rt, gs::Gs& gs) : rt_(rt), gs_(gs) {}

void Gif::reset() {
    for (auto& p : paths_) p = Path{};
    q_ = 0x3F800000u;
    path3Masked_ = false;
}

void Gif::writePacked(unsigned desc, std::uint64_t lo, std::uint64_t hi, std::uint32_t pc) {
    using namespace gs;
    switch (desc) {
        case 0x0:  // PRIM
            gs_.writeRegister(PRIM, bits(lo, 0, 11), pc);
            return;
        case 0x1: {  // RGBAQ: Q vem do último ST
            const std::uint64_t v = bits(lo, 0, 8) | (bits(lo, 32, 8) << 8) | (bits(hi, 0, 8) << 16) |
                                    (bits(hi, 32, 8) << 24) | (std::uint64_t{q_} << 32);
            gs_.writeRegister(RGBAQ, v, pc);
            return;
        }
        case 0x2:  // ST (+Q interno)
            q_ = static_cast<std::uint32_t>(hi);
            gs_.writeRegister(ST, lo, pc);
            return;
        case 0x3:  // UV
            gs_.writeRegister(UV, bits(lo, 0, 14) | (bits(lo, 32, 14) << 16), pc);
            return;
        case 0x4: case 0xC: {  // XYZF2 / XYZF3
            const bool adc = bits(hi, 47, 1) != 0;
            const std::uint64_t v = bits(lo, 0, 16) | (bits(lo, 32, 16) << 16) | (bits(hi, 4, 24) << 32) |
                                    (bits(hi, 36, 8) << 56);
            gs_.writeRegister((desc == 0xC || adc) ? XYZF3 : XYZF2, v, pc);
            return;
        }
        case 0x5: case 0xD: {  // XYZ2 / XYZ3
            const bool adc = bits(hi, 47, 1) != 0;
            const std::uint64_t v = bits(lo, 0, 16) | (bits(lo, 32, 16) << 16) | (bits(hi, 0, 32) << 32);
            gs_.writeRegister((desc == 0xD || adc) ? XYZ3 : XYZ2, v, pc);
            return;
        }
        case 0x6: case 0x7: case 0x8: case 0x9:  // TEX0_1/2, CLAMP_1/2
            gs_.writeRegister(static_cast<std::uint8_t>(desc), lo, pc);
            return;
        case 0xA:  // FOG
            gs_.writeRegister(FOG, bits(hi, 36, 8) << 56, pc);
            return;
        case 0xE:  // A+D
            gs_.writeRegister(static_cast<std::uint8_t>(hi & 0xFF), lo, pc);
            return;
        case 0xF:  // NOP
            return;
        default:
            throw Unimplemented("GIF: descritor de registrador reservado 0x" + std::to_string(desc) +
                                    " no modo PACKED",
                                pc);
    }
}

void Gif::writeReglist(unsigned desc, std::uint64_t data, std::uint32_t pc) {
    // Em REGLIST o descritor é o próprio endereço do registrador (0x0–0xD);
    // A+D (0xE) e NOP (0xF) não escrevem nada.
    if (desc == 0xB) throw Unimplemented("GIF: descritor reservado 0xB no modo REGLIST", pc);
    if (desc >= 0xE) return;
    gs_.writeRegister(static_cast<std::uint8_t>(desc), data, pc);
}

void Gif::transfer(unsigned path, const std::uint8_t* data, std::size_t qwords, std::uint32_t pc) {
    Path& p = paths_[path - 1];
    for (std::size_t i = 0; i < qwords; ++i) {
        std::uint64_t lo, hi;
        std::memcpy(&lo, data + i * 16, 8);
        std::memcpy(&hi, data + i * 16 + 8, 8);
        if (!p.active) {
            // GIFtag
            p.tag[0] = lo;
            p.tag[1] = hi;
            p.nloop = static_cast<std::uint32_t>(bits(lo, 0, 15));
            p.eop = bits(lo, 15, 1) != 0;
            p.flg = static_cast<unsigned>(bits(lo, 58, 2));
            p.nreg = static_cast<unsigned>(bits(lo, 60, 4));
            if (p.nreg == 0) p.nreg = 16;
            p.regs = hi;
            p.reg = 0;
            q_ = 0x3F800000u;  // Q volta a 1.0 a cada GIFtag
            if (bits(lo, 46, 1) && p.flg == kPacked) gs_.writeRegister(gs::PRIM, bits(lo, 47, 11), pc);
            if (rt_ && rt_->options().traceGs) {
                std::fprintf(stderr, "[gif] PATH%u tag NLOOP=%u EOP=%d FLG=%u NREG=%u REGS=%016llx\n", path,
                             p.nloop, p.eop ? 1 : 0, p.flg, p.nreg, static_cast<unsigned long long>(p.regs));
            }
            p.active = p.nloop > 0;
            continue;
        }
        switch (p.flg) {
            case kPacked: {
                const unsigned desc = static_cast<unsigned>((p.regs >> (p.reg * 4)) & 0xF);
                writePacked(desc, lo, hi, pc);
                if (++p.reg == p.nreg) {
                    p.reg = 0;
                    if (--p.nloop == 0) p.active = false;
                }
                break;
            }
            case kReglist: {
                const std::uint64_t words[2] = {lo, hi};
                for (std::uint64_t w : words) {
                    if (!p.active) break;  // NREG*NLOOP ímpar: a metade final é preenchimento
                    const unsigned desc = static_cast<unsigned>((p.regs >> (p.reg * 4)) & 0xF);
                    writeReglist(desc, w, pc);
                    if (++p.reg == p.nreg) {
                        p.reg = 0;
                        if (--p.nloop == 0) p.active = false;
                    }
                }
                break;
            }
            default:  // IMAGE (e o modo 3, que o hardware trata igual)
                gs_.writeTransferData(lo, pc);
                gs_.writeTransferData(hi, pc);
                if (--p.nloop == 0) p.active = false;
                break;
        }
    }
}

void Gif::kick(const std::uint8_t* mem, std::uint32_t size, std::uint32_t addr, std::uint32_t pc) {
    const Path& p = paths_[0];
    for (std::uint32_t n = 0; n <= size / 16; ++n) {
        transfer(1, mem + (addr & (size - 16)), 1, pc);
        addr += 16;
        if (!p.active && p.eop) return;
    }
    throw GuestError("XGKICK: a memória do VU1 inteira foi enviada sem um GIFtag com EOP", pc);
}

std::uint32_t Gif::readRegister(std::uint32_t addr, std::uint32_t pc) {
    switch (addr) {
        case 0x10003000: return ctrl_;
        case 0x10003010: return mode_;
        case 0x10003020: {  // GIF_STAT: ocioso; M3R/IMT espelham GIF_MODE; M3P = MSKPATH3
            return (mode_ & 1) | ((mode_ & 4)) | (path3Masked_ ? 2u : 0u);
        }
        case 0x10003040: case 0x10003050: case 0x10003060: case 0x10003070: {
            const Path& p = paths_[2];
            const unsigned w = (addr - 0x10003040) >> 4;
            return static_cast<std::uint32_t>(p.tag[w >> 1] >> ((w & 1) * 32));
        }
        case 0x10003080: case 0x10003090: case 0x100030A0: return 0;
        default: break;
    }
    throw Unimplemented("leitura de registrador do GIF desconhecido " + anyps2::hex(addr), pc);
}

void Gif::writeRegister(std::uint32_t addr, std::uint32_t value, std::uint32_t pc) {
    switch (addr) {
        case 0x10003000:
            if (value & 1) reset();  // RST
            ctrl_ = value & 0x8;     // PSE
            return;
        case 0x10003010:
            mode_ = value & 0x5;
            return;
        default: break;
    }
    throw Unimplemented("escrita em registrador do GIF " + anyps2::hex(addr) + " (somente leitura ou desconhecido)",
                        pc);
}

}  // namespace anyps2::rt
