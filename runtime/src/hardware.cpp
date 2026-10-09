#include "anyps2/runtime/hardware.h"

#include <cstdio>
#include <cstring>

#include "anyps2/runtime/dmac.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/iop/iop.h"
#include "anyps2/runtime/ipu.h"
#include "anyps2/runtime/vif.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/timing.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

namespace {

struct RegName {
    std::uint32_t addr;
    const char* name;
};

constexpr RegName kNames[] = {
    {0x10000000, "T0_COUNT"}, {0x10000010, "T0_MODE"}, {0x10000020, "T0_COMP"}, {0x10000030, "T0_HOLD"},
    {0x10000800, "T1_COUNT"}, {0x10000810, "T1_MODE"}, {0x10000820, "T1_COMP"}, {0x10000830, "T1_HOLD"},
    {0x10001000, "T2_COUNT"}, {0x10001010, "T2_MODE"}, {0x10001020, "T2_COMP"},
    {0x10001800, "T3_COUNT"}, {0x10001810, "T3_MODE"}, {0x10001820, "T3_COMP"},
    {0x10002000, "IPU_CMD"}, {0x10002010, "IPU_CTRL"}, {0x10002020, "IPU_BP"}, {0x10002030, "IPU_TOP"},
    {0x10003000, "GIF_CTRL"}, {0x10003010, "GIF_MODE"}, {0x10003020, "GIF_STAT"},
    {0x10003040, "GIF_TAG0"}, {0x10003050, "GIF_TAG1"}, {0x10003060, "GIF_TAG2"}, {0x10003070, "GIF_TAG3"},
    {0x10003080, "GIF_CNT"}, {0x10003090, "GIF_P3CNT"}, {0x100030A0, "GIF_P3TAG"},
    {0x10003800, "VIF0_STAT"}, {0x10003810, "VIF0_FBRST"}, {0x10003820, "VIF0_ERR"}, {0x10003830, "VIF0_MARK"},
    {0x10003C00, "VIF1_STAT"}, {0x10003C10, "VIF1_FBRST"}, {0x10003C20, "VIF1_ERR"}, {0x10003C30, "VIF1_MARK"},
    {0x10004000, "VIF0_FIFO"}, {0x10005000, "VIF1_FIFO"}, {0x10006000, "GIF_FIFO"},
    {0x10007000, "IPU_out_FIFO"}, {0x10007010, "IPU_in_FIFO"},
    {0x1000E000, "D_CTRL"}, {0x1000E010, "D_STAT"}, {0x1000E020, "D_PCR"}, {0x1000E030, "D_SQWC"},
    {0x1000E040, "D_RBSR"}, {0x1000E050, "D_RBOR"}, {0x1000E060, "D_STADR"},
    {0x1000F000, "INTC_STAT"}, {0x1000F010, "INTC_MASK"},
    {0x1000F100, "SIO_LCR"}, {0x1000F110, "SIO_LSR"}, {0x1000F120, "SIO_IER"}, {0x1000F130, "SIO_ISR"},
    {0x1000F140, "SIO_FCR"}, {0x1000F150, "SIO_BGR"}, {0x1000F180, "SIO_TXFIFO"}, {0x1000F1C0, "SIO_RXFIFO"},
    {0x1000F200, "SB_MSCOM"}, {0x1000F210, "SB_SMCOM"}, {0x1000F220, "SB_MSFLG"}, {0x1000F230, "SB_SMFLG"},
    {0x1000F240, "SB_CTRL"}, {0x1000F260, "SB_BD6"},
    {0x1000F430, "MCH_RICM"}, {0x1000F440, "MCH_DRD"}, {0x1000F520, "D_ENABLER"}, {0x1000F590, "D_ENABLEW"},
    {0x12000000, "GS_PMODE"}, {0x12000010, "GS_SMODE1"}, {0x12000020, "GS_SMODE2"}, {0x12000030, "GS_SRFSH"},
    {0x12000040, "GS_SYNCH1"}, {0x12000050, "GS_SYNCH2"}, {0x12000060, "GS_SYNCV"},
    {0x12000070, "GS_DISPFB1"}, {0x12000080, "GS_DISPLAY1"}, {0x12000090, "GS_DISPFB2"},
    {0x120000A0, "GS_DISPLAY2"}, {0x120000B0, "GS_EXTBUF"}, {0x120000C0, "GS_EXTDATA"},
    {0x120000D0, "GS_EXTWRITE"}, {0x120000E0, "GS_BGCOLOR"}, {0x12001000, "GS_CSR"},
    {0x12001010, "GS_IMR"}, {0x12001040, "GS_BUSDIR"}, {0x12001080, "GS_SIGLBLID"},
};

const char* dmaChannelName(unsigned ch) {
    static const char* kNamesCh[] = {"VIF0", "VIF1", "GIF", "fromIPU", "toIPU", "SIF0", "SIF1", "SIF2", "fromSPR", "toSPR"};
    return ch < 10 ? kNamesCh[ch] : "?";
}

// Endereço base de cada canal de DMA.
constexpr std::uint32_t kDmaBase[10] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                        0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};

int dmaChannelOf(std::uint32_t addr) {
    for (int ch = 0; ch < 10; ++ch) {
        if (addr >= kDmaBase[ch] && addr < kDmaBase[ch] + 0x100) return ch;
    }
    return -1;
}

}  // namespace

std::string Hardware::registerName(std::uint32_t addr) {
    for (const auto& r : kNames) {
        if (r.addr == (addr & ~0xFu)) return r.name;
    }
    const int ch = dmaChannelOf(addr);
    if (ch >= 0) {
        static const char* kField[] = {"CHCR", "MADR", "QWC", "TADR", "ASR0", "ASR1", "", "", "SADR"};
        const unsigned field = (addr - kDmaBase[ch]) >> 4;
        return std::string("D") + std::to_string(ch) + "_" + (field < 9 ? kField[field] : "?") + " (" +
               dmaChannelName(static_cast<unsigned>(ch)) + ")";
    }
    return "";
}

Hardware::Hardware(Runtime& rt) : rt_(rt) {
    rt.memory().mapDevice(0x10000000u, 0x10000u, this);
    rt.memory().mapDevice(0x12000000u, 0x2000u, this);
}

std::uint64_t Hardware::read64(std::uint32_t addr, unsigned size, std::uint32_t pc) {
    const std::uint32_t reg = addr & ~0xFu;
    if (rt_.options().traceHardware) {
        std::fprintf(stderr, "[hw] leitura %u bits %08x %s\n", size * 8, addr, registerName(addr).c_str());
    }
    if (reg >= 0x1000F200u && reg <= 0x1000F260u) return rt_.iop().sifReadRegister(reg);
    if (reg < 0x10002000u && (reg & 0x7CFu) == 0 && ((reg >> 4) & 0xF) < 4) {
        return rt_.timing().readTimer((reg >> 11) & 3, (reg >> 4) & 3);
    }
    if (reg >= 0x12000000u) {
        if (reg == 0x12001000u) rt_.timing().process(pc);  // VSINT/FIELD em dia
        return rt_.gs().readPrivileged(reg, pc);
    }
    if (Dmac::handles(reg)) return rt_.dmac().read(reg, pc);
    if (reg >= 0x10002000u && reg < 0x10003000u) return rt_.ipu().readRegister(reg, pc);
    if (reg >= 0x10003000u && reg < 0x10003800u) return rt_.gif().readRegister(reg, pc);
    if (reg >= 0x10003800u && reg < 0x10003C00u) return rt_.vif0().readRegister(reg, pc);
    if (reg >= 0x10003C00u && reg < 0x10004000u) return rt_.vif1().readRegister(reg, pc);
    if (reg >= 0x10004000u && reg < 0x10008000u) {
        throw Unimplemented("leitura de FIFO " + registerName(addr) + " (download do GS/IPU) ainda não suportada; em " +
                                rt_.describe(pc),
                            pc);
    }
    switch (reg) {
        case 0x1000F000: return rt_.kernel().intcStat();
        case 0x1000F010: return rt_.kernel().intcMask();
        case 0x1000F110: return 0x60;  // SIO_LSR: transmissor vazio
        default: break;
    }
    if (!registerName(addr).empty()) return regs_[reg];
    throw Unimplemented("leitura de " + std::to_string(size * 8) +
                            " bits de registrador de hardware desconhecido " + anyps2::hex(addr) +
                            " em " + rt_.describe(pc),
                        pc);
}

void Hardware::write64(std::uint32_t addr, std::uint64_t value, unsigned size, std::uint32_t pc) {
    const std::uint32_t reg = addr & ~0xFu;
    if (rt_.options().traceHardware) {
        std::fprintf(stderr, "[hw] escrita %u bits %08x %s = 0x%llx\n", size * 8, addr,
                     registerName(addr).c_str(), static_cast<unsigned long long>(value));
    }
    if (reg >= 0x1000F200u && reg <= 0x1000F260u) {
        rt_.iop().sifWriteRegister(reg, static_cast<std::uint32_t>(value));
        return;
    }
    if (reg < 0x10002000u && (reg & 0x7CFu) == 0 && ((reg >> 4) & 0xF) < 4) {
        rt_.timing().writeTimer((reg >> 11) & 3, (reg >> 4) & 3, static_cast<std::uint32_t>(value));
        return;
    }
    if (reg >= 0x12000000u) {
        rt_.gs().writePrivileged(reg, value, pc);
        return;
    }
    if (Dmac::handles(reg)) {
        rt_.dmac().write(reg, static_cast<std::uint32_t>(value), pc);
        return;
    }
    if (reg >= 0x10002000u && reg < 0x10003000u) {
        rt_.ipu().writeRegister(reg, static_cast<std::uint32_t>(value), pc);
        return;
    }
    if (reg >= 0x10003000u && reg < 0x10003800u) {
        rt_.gif().writeRegister(reg, static_cast<std::uint32_t>(value), pc);
        return;
    }
    if (reg >= 0x10003800u && reg < 0x10004000u) {
        (reg < 0x10003C00u ? rt_.vif0() : rt_.vif1()).writeRegister(reg, static_cast<std::uint32_t>(value), pc);
        return;
    }
    switch (reg) {
        case 0x1000F000: rt_.kernel().clearIntcStat(static_cast<std::uint32_t>(value)); return;  // 1 limpa
        case 0x1000F010: rt_.kernel().toggleIntcMask(static_cast<std::uint32_t>(value)); return;  // 1 inverte
        case 0x1000F180: {  // SIO_TXFIFO: saída de debug do kernel/EE
            const char ch = static_cast<char>(value & 0xFF);
            std::fputc(ch, stdout);
            if (ch == '\n') std::fflush(stdout);
            return;
        }
        default: break;
    }
    if (reg >= 0x10004000u && reg < 0x10008000u) {
        throw Unimplemented("escrita de " + std::to_string(size * 8) + " bits no FIFO " + registerName(addr) +
                                " (o hardware só aceita 128 bits); em " + rt_.describe(pc),
                            pc);
    }
    if (!registerName(addr).empty()) {
        regs_[reg] = value;
        return;
    }
    throw Unimplemented("escrita de " + std::to_string(size * 8) + " bits em registrador de hardware desconhecido " +
                            anyps2::hex(addr) + " (valor " + anyps2::hex(value) + ") em " + rt_.describe(pc),
                        pc);
}

void Hardware::read(std::uint32_t addr, void* out, unsigned size, std::uint32_t pc) {
    if (size == 16 && (addr & ~0xFu) == 0x10007000u) {  // IPU_out_FIFO
        std::uint8_t qw[16];
        rt_.ipu().fifoRead(qw, pc);
        std::memcpy(out, qw, 16);
        return;
    }
    if (size == 16 && (addr & ~0xFFFu) == 0x10005000u) {  // VIF1_FIFO: download do GS
        if (!rt_.gs().busDirToHost()) {
            throw GuestError("leitura do VIF1_FIFO com BUSDIR = 0 (o GS não está mandando dados)", pc);
        }
        std::uint8_t qw[16];
        rt_.gs().readDownload(qw, 1);
        std::memcpy(out, qw, 16);
        return;
    }
    if (size == 16) {
        const std::uint64_t lo = read64(addr, 8, pc);
        std::memcpy(out, &lo, 8);
        std::memset(static_cast<std::uint8_t*>(out) + 8, 0, 8);
        return;
    }
    const std::uint64_t v = read64(addr, size, pc) >> (8 * (addr & 7));
    std::memcpy(out, &v, size);
}

void Hardware::write(std::uint32_t addr, const void* in, unsigned size, std::uint32_t pc) {
    if (size == 16 && addr >= 0x10004000u && addr < 0x10008000u) {
        const auto* data = static_cast<const std::uint8_t*>(in);
        if (rt_.options().traceHardware) {
            std::fprintf(stderr, "[hw] escrita 128 bits %08x %s\n", addr, registerName(addr).c_str());
        }
        switch (addr & ~0xFFFu) {
            case 0x10004000: rt_.vif0().fifoWrite(data, 16, pc); return;
            case 0x10005000: rt_.vif1().fifoWrite(data, 16, pc); return;
            case 0x10006000:
                rt_.gif().transfer(3, data, 1, pc);
                return;
            default:
                if ((addr & ~0xFu) == 0x10007010u) {  // IPU_in_FIFO
                    std::uint8_t qw[16];
                    std::memcpy(qw, data, 16);
                    rt_.ipu().fifoWrite(qw, pc);
                    return;
                }
                throw Unimplemented("escrita no FIFO de saída do IPU (" + anyps2::hex(addr) + ") em " +
                                        rt_.describe(pc),
                                    pc);
        }
    }
    std::uint64_t v = 0;
    std::memcpy(&v, in, size > 8 ? 8 : size);
    write64(addr, v, size > 8 ? 8 : size, pc);
}

}  // namespace anyps2::rt
