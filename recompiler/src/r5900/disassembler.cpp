#include "anyps2/r5900/disassembler.h"

#include <cstdio>

#include "anyps2/r5900/decoder.h"
#include "anyps2/r5900/registers.h"

namespace anyps2::r5900 {
namespace {

std::string hexLower(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

std::string dec(std::int64_t v) {
    return std::to_string(v);
}

std::string gprStr(std::uint32_t r) {
    return std::string(gprName(r));
}

std::string fpr(std::uint32_t r) {
    return "$f" + std::to_string(r);
}

// Registradores de controle do COP1: 0 = FIR (implementação), 31 = FCSR.
std::string fcrStr(std::uint32_t r) {
    if (r == 0) return "c1_fir";
    if (r == 31) return "c1_fcsr";
    return "$" + std::to_string(r);
}

std::string vf(std::uint32_t r, const std::string& suffix) {
    return "$vf" + std::to_string(r) + suffix;
}

std::string vi(std::uint32_t r) {
    return "$vi" + std::to_string(r);
}

constexpr char kComp[4] = {'x', 'y', 'z', 'w'};

std::string comp(std::uint32_t c) {
    return std::string(1, kComp[c & 3]);
}

std::string memOperand(const Instruction& i) {
    return dec(i.simm16()) + "(" + gprStr(i.rs()) + ")";
}

std::string target(std::uint32_t addr, const DisasmOptions& options) {
    std::string s = hexLower(addr);
    if (options.symbolize) {
        std::string sym = options.symbolize(addr);
        if (!sym.empty()) s += " <" + sym + ">";
    }
    return s;
}

}  // namespace

std::string vuDestString(std::uint32_t dest) {
    std::string s;
    if (dest & 8) s += 'x';
    if (dest & 4) s += 'y';
    if (dest & 2) s += 'z';
    if (dest & 1) s += 'w';
    return s;
}

DisasmText disassemble(const Instruction& i, const DisasmOptions& options) {
    DisasmText out;
    if (!i.valid()) {
        out.mnemonic = ".word";
        out.operands = hexLower(i.raw);
        return out;
    }

    const OpInfo& info = i.info();
    out.mnemonic = std::string(info.mnemonic);
    std::string& o = out.operands;

    const std::string dest = vuDestString(i.vuDest());
    const std::string dotDest = "." + dest;

    switch (info.format) {
        case Format::FMT_NONE:
            break;
        case Format::FMT_SYNC:
            if (i.raw & 0x400) out.mnemonic = "sync.p";
            break;
        case Format::FMT_RD_RS_RT:
            o = gprStr(i.rd()) + "," + gprStr(i.rs()) + "," + gprStr(i.rt());
            break;
        case Format::FMT_RDOPT_RS_RT:
            if (i.rd() != 0) o = gprStr(i.rd()) + ",";
            o += gprStr(i.rs()) + "," + gprStr(i.rt());
            break;
        case Format::FMT_RD_RT_RS:
            o = gprStr(i.rd()) + "," + gprStr(i.rt()) + "," + gprStr(i.rs());
            break;
        case Format::FMT_RD_RT_SA:
            o = gprStr(i.rd()) + "," + gprStr(i.rt()) + "," + hexLower(i.sa());
            break;
        case Format::FMT_RD_RT:
            o = gprStr(i.rd()) + "," + gprStr(i.rt());
            break;
        case Format::FMT_RD_RS:
            o = gprStr(i.rd()) + "," + gprStr(i.rs());
            break;
        case Format::FMT_RD:
            o = gprStr(i.rd());
            break;
        case Format::FMT_RS:
            o = gprStr(i.rs());
            break;
        case Format::FMT_RS_RT:
            o = gprStr(i.rs()) + "," + gprStr(i.rt());
            break;
        case Format::FMT_ZERO_RS_RT:
            o = "zero," + gprStr(i.rs()) + "," + gprStr(i.rt());
            break;
        case Format::FMT_JALR:
            // Com rd = ra (o caso usual) o objdump omite o destino.
            o = i.rd() == 31 ? gprStr(i.rs()) : gprStr(i.rd()) + "," + gprStr(i.rs());
            break;
        case Format::FMT_RT_RS_SIMM:
            o = gprStr(i.rt()) + "," + gprStr(i.rs()) + "," + dec(i.simm16());
            break;
        case Format::FMT_RT_RS_UIMM:
            o = gprStr(i.rt()) + "," + gprStr(i.rs()) + "," + hexLower(i.imm16());
            break;
        case Format::FMT_RT_UIMM:
            o = gprStr(i.rt()) + "," + hexLower(i.imm16());
            break;
        case Format::FMT_RS_RT_BRANCH:
            o = gprStr(i.rs()) + "," + gprStr(i.rt()) + "," + target(i.branchTarget(), options);
            break;
        case Format::FMT_RS_BRANCH:
            o = gprStr(i.rs()) + "," + target(i.branchTarget(), options);
            break;
        case Format::FMT_BRANCH:
            o = target(i.branchTarget(), options);
            break;
        case Format::FMT_JUMP:
            o = target(i.jumpTarget(), options);
            break;
        case Format::FMT_MEM_GPR:
            o = gprStr(i.rt()) + "," + memOperand(i);
            break;
        case Format::FMT_MEM_FPR:
            o = fpr(i.ft()) + "," + memOperand(i);
            break;
        case Format::FMT_MEM_VF:
            o = vf(i.rt(), "") + "," + memOperand(i);
            break;
        case Format::FMT_CACHE:
            o = hexLower(i.rt()) + "," + memOperand(i);
            break;
        case Format::FMT_SYSCALL:
            if (i.syscallCode() != 0) o = hexLower(i.syscallCode());
            break;
        case Format::FMT_BREAK: {
            const std::uint32_t c1 = (i.raw >> 16) & 0x3FF;
            const std::uint32_t c2 = (i.raw >> 6) & 0x3FF;
            if (c2 != 0)
                o = hexLower(c1) + "," + hexLower(c2);
            else if (c1 != 0)
                o = hexLower(c1);
            break;
        }
        case Format::FMT_TRAP:
            o = gprStr(i.rs()) + "," + gprStr(i.rt());
            if (i.trapCode() != 0) o += "," + hexLower(i.trapCode());
            break;
        case Format::FMT_RS_SIMM:
            o = gprStr(i.rs()) + "," + dec(i.simm16());
            break;
        case Format::FMT_RT_COP0: {
            std::string_view name = cop0Name(i.rd());
            o = gprStr(i.rt()) + "," + (name.empty() ? "$" + std::to_string(i.rd()) : std::string(name));
            break;
        }
        case Format::FMT_RT:
            o = gprStr(i.rt());
            break;
        case Format::FMT_RT_PERF:
            o = gprStr(i.rt()) + "," + dec((i.raw >> 1) & 0x1F);
            break;
        case Format::FMT_RT_FS:
            o = gprStr(i.rt()) + "," + fpr(i.fs());
            break;
        case Format::FMT_RT_FCR:
            o = gprStr(i.rt()) + "," + fcrStr(i.fs());
            break;
        case Format::FMT_RT_VF:
            if (i.vuInterlock()) out.mnemonic += ".i";
            o = gprStr(i.rt()) + "," + vf(i.rd(), "");
            break;
        case Format::FMT_RT_VI:
            if (i.vuInterlock()) out.mnemonic += ".i";
            o = gprStr(i.rt()) + "," + vi(i.rd());
            break;
        case Format::FMT_FD_FS_FT:
            o = fpr(i.fd()) + "," + fpr(i.fs()) + "," + fpr(i.ft());
            break;
        case Format::FMT_FD_FS:
            o = fpr(i.fd()) + "," + fpr(i.fs());
            break;
        case Format::FMT_FD_FT:
            o = fpr(i.fd()) + "," + fpr(i.ft());
            break;
        case Format::FMT_FS_FT:
            o = fpr(i.fs()) + "," + fpr(i.ft());
            break;

        // ---- VU0 macro ----------------------------------------------------
        case Format::FMT_VU_FD_FS_FT:
            out.mnemonic += dotDest;
            o = vf(i.vuFd(), dest) + "," + vf(i.vuFs(), dest) + "," + vf(i.vuFt(), dest);
            break;
        case Format::FMT_VU_FD_FS_FTBC:
            out.mnemonic += comp(i.vuBc()) + dotDest;
            o = vf(i.vuFd(), dest) + "," + vf(i.vuFs(), dest) + "," + vf(i.vuFt(), comp(i.vuBc()));
            break;
        case Format::FMT_VU_FD_FS_Q:
            out.mnemonic += dotDest;
            o = vf(i.vuFd(), dest) + "," + vf(i.vuFs(), dest) + ",$Q";
            break;
        case Format::FMT_VU_FD_FS_I:
            out.mnemonic += dotDest;
            o = vf(i.vuFd(), dest) + "," + vf(i.vuFs(), dest) + ",$I";
            break;
        case Format::FMT_VU_ACC_FS_FT:
            out.mnemonic += dotDest;
            o = "$ACC" + dest + "," + vf(i.vuFs(), dest) + "," + vf(i.vuFt(), dest);
            break;
        case Format::FMT_VU_ACC_FS_FTBC:
            out.mnemonic += comp(i.vuBc()) + dotDest;
            o = "$ACC" + dest + "," + vf(i.vuFs(), dest) + "," + vf(i.vuFt(), comp(i.vuBc()));
            break;
        case Format::FMT_VU_ACC_FS_Q:
            out.mnemonic += dotDest;
            o = "$ACC" + dest + "," + vf(i.vuFs(), dest) + ",$Q";
            break;
        case Format::FMT_VU_ACC_FS_I:
            out.mnemonic += dotDest;
            o = "$ACC" + dest + "," + vf(i.vuFs(), dest) + ",$I";
            break;
        case Format::FMT_VU_FT_FS:
            out.mnemonic += dotDest;
            o = vf(i.vuFt(), dest) + "," + vf(i.vuFs(), dest);
            break;
        case Format::FMT_VU_CLIP:
            out.mnemonic += dotDest;
            o = vf(i.vuFs(), dest) + "," + vf(i.vuFt(), "w");
            break;
        case Format::FMT_VU_ID_IS_IT:
            o = vi(i.vuFd()) + "," + vi(i.vuFs()) + "," + vi(i.vuFt());
            break;
        case Format::FMT_VU_IT_IS_IMM5:
            o = vi(i.vuFt()) + "," + vi(i.vuFs()) + "," + dec(i.vuImm5());
            break;
        case Format::FMT_VU_CALLMS:
            o = hexLower(i.vuImm15() * 8);
            break;
        case Format::FMT_VU_CALLMSR:
            o = vi(i.vuFs());
            break;
        case Format::FMT_VU_LQI:
            out.mnemonic += dotDest;
            o = vf(i.vuFt(), dest) + ",(" + vi(i.vuFs()) + "++)";
            break;
        case Format::FMT_VU_SQI:
            out.mnemonic += dotDest;
            o = vf(i.vuFs(), dest) + ",(" + vi(i.vuFt()) + "++)";
            break;
        case Format::FMT_VU_LQD:
            out.mnemonic += dotDest;
            o = vf(i.vuFt(), dest) + ",(--" + vi(i.vuFs()) + ")";
            break;
        case Format::FMT_VU_SQD:
            out.mnemonic += dotDest;
            o = vf(i.vuFs(), dest) + ",(--" + vi(i.vuFt()) + ")";
            break;
        case Format::FMT_VU_DIV:
            o = "$Q," + vf(i.vuFs(), comp(i.vuFsf())) + "," + vf(i.vuFt(), comp(i.vuFtf()));
            break;
        case Format::FMT_VU_SQRT:
            o = "$Q," + vf(i.vuFt(), comp(i.vuFtf()));
            break;
        case Format::FMT_VU_MTIR:
            o = vi(i.vuFt()) + "," + vf(i.vuFs(), comp(i.vuFsf()));
            break;
        case Format::FMT_VU_MFIR:
            out.mnemonic += dotDest;
            o = vf(i.vuFt(), dest) + "," + vi(i.vuFs());
            break;
        case Format::FMT_VU_ILWR:
            out.mnemonic += dotDest;
            o = vi(i.vuFt()) + ",(" + vi(i.vuFs()) + ")";
            break;
        case Format::FMT_VU_RNEXT:
            out.mnemonic += dotDest;
            o = vf(i.vuFt(), dest) + ",$R";
            break;
        case Format::FMT_VU_RINIT:
            o = "$R," + vf(i.vuFs(), comp(i.vuFsf()));
            break;
    }
    return out;
}

std::string disassembleWord(std::uint32_t raw, std::uint32_t address) {
    return disassemble(decode(raw, address)).str();
}

}  // namespace anyps2::r5900
