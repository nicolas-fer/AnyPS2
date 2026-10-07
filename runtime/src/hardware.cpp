#include "anyps2/runtime/hardware.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/iop.h"
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
    {0x10003800, "VIF0_STAT"}, {0x10003C00, "VIF1_STAT"},
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

std::uint64_t ticks(std::uint64_t hz) {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    const auto ns = duration_cast<nanoseconds>(steady_clock::now() - t0).count();
    return static_cast<std::uint64_t>(ns) * hz / 1000000000ull;
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
    regs_[0x12001000] = 0;  // GS_CSR
    regs_[0x12001010] = 0x7F00;
}

void Hardware::setGsCrt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field) {
    regs_[0x12000010] = (std::uint64_t{interlace} << 32) | (mode << 8) | field;
}

std::uint32_t Hardware::timerCount(unsigned timer) const {
    // Relógio de barramento: 147,456 MHz; MODE.CLKS: 0=BUSCLK, 1=/16, 2=/256, 3=HBLANK (~15,7 kHz)
    const auto it = regs_.find(0x10000010u + timer * 0x800u);
    const std::uint32_t mode = it == regs_.end() ? 0 : static_cast<std::uint32_t>(it->second);
    static constexpr std::uint64_t kHz[4] = {147456000ull, 9216000ull, 576000ull, 15734ull};
    return static_cast<std::uint32_t>(ticks(kHz[mode & 3]) & 0xFFFF);
}

std::uint64_t Hardware::read64(std::uint32_t addr, unsigned size, std::uint32_t pc) {
    const std::uint32_t reg = addr & ~0xFu;
    if (rt_.options().traceHardware) {
        std::fprintf(stderr, "[hw] leitura %u bits %08x %s\n", size * 8, addr, registerName(addr).c_str());
    }
    if (reg >= 0x1000F200u && reg <= 0x1000F260u) return rt_.iop().sifReadRegister(reg);
    switch (reg) {
        case 0x10000000: return timerCount(0);
        case 0x10000800: return timerCount(1);
        case 0x10001000: return timerCount(2);
        case 0x10001800: return timerCount(3);
        case 0x1000F000: return intcStat_;
        case 0x1000F010: return intcMask_;
        case 0x1000F110: return 0x60;  // SIO_LSR: transmissor vazio
        case 0x1000F590: case 0x1000F520: return regs_[0x1000F590];
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
    switch (reg) {
        case 0x1000F000: intcStat_ &= ~static_cast<std::uint32_t>(value); return;  // escreve 1 para limpar
        case 0x1000F010: intcMask_ ^= static_cast<std::uint32_t>(value); return;   // escreve 1 para inverter
        case 0x1000F180: {  // SIO_TXFIFO: saída de debug do kernel/EE
            const char ch = static_cast<char>(value & 0xFF);
            std::fputc(ch, stdout);
            if (ch == '\n') std::fflush(stdout);
            return;
        }
        case 0x1000F520: case 0x1000F590: regs_[0x1000F590] = value; return;
        default: break;
    }
    const int ch = dmaChannelOf(addr);
    if (ch >= 0 && (addr - kDmaBase[ch]) == 0 && (value & 0x100)) {
        throw Unimplemented(std::string("início de transferência DMA no canal ") + dmaChannelName(static_cast<unsigned>(ch)) +
                                " (CHCR.STR) — DMAC/GIF/VIF chegam na Fase 4; em " + rt_.describe(pc),
                            pc);
    }
    if (reg >= 0x10004000u && reg < 0x10008000u) {
        throw Unimplemented("escrita em FIFO " + registerName(addr) + " — Fase 4; em " + rt_.describe(pc), pc);
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
    std::uint64_t v = 0;
    std::memcpy(&v, in, size > 8 ? 8 : size);
    write64(addr, v, size > 8 ? 8 : size, pc);
}

}  // namespace anyps2::rt
