#include "anyps2/r5900/opcodes.h"

namespace anyps2::r5900 {
namespace {

// Abreviações usadas em opcodes.def.
using F = Format;
constexpr F FMT_NONE = F::FMT_NONE, FMT_SYNC = F::FMT_SYNC, FMT_RD_RS_RT = F::FMT_RD_RS_RT, FMT_RDOPT_RS_RT = F::FMT_RDOPT_RS_RT,
            FMT_RD_RT_RS = F::FMT_RD_RT_RS, FMT_RD_RT_SA = F::FMT_RD_RT_SA, FMT_RD_RT = F::FMT_RD_RT,
            FMT_RD_RS = F::FMT_RD_RS, FMT_RD = F::FMT_RD, FMT_RS = F::FMT_RS, FMT_RS_RT = F::FMT_RS_RT,
            FMT_ZERO_RS_RT = F::FMT_ZERO_RS_RT, FMT_JALR = F::FMT_JALR,
            FMT_RT_RS_SIMM = F::FMT_RT_RS_SIMM, FMT_RT_RS_UIMM = F::FMT_RT_RS_UIMM,
            FMT_RT_UIMM = F::FMT_RT_UIMM, FMT_RS_RT_BRANCH = F::FMT_RS_RT_BRANCH,
            FMT_RS_BRANCH = F::FMT_RS_BRANCH, FMT_BRANCH = F::FMT_BRANCH, FMT_JUMP = F::FMT_JUMP,
            FMT_MEM_GPR = F::FMT_MEM_GPR, FMT_MEM_FPR = F::FMT_MEM_FPR, FMT_MEM_VF = F::FMT_MEM_VF,
            FMT_CACHE = F::FMT_CACHE, FMT_SYSCALL = F::FMT_SYSCALL, FMT_BREAK = F::FMT_BREAK,
            FMT_TRAP = F::FMT_TRAP, FMT_RS_SIMM = F::FMT_RS_SIMM, FMT_RT_COP0 = F::FMT_RT_COP0,
            FMT_RT = F::FMT_RT, FMT_RT_PERF = F::FMT_RT_PERF, FMT_RT_FS = F::FMT_RT_FS,
            FMT_RT_FCR = F::FMT_RT_FCR, FMT_RT_VF = F::FMT_RT_VF, FMT_RT_VI = F::FMT_RT_VI,
            FMT_FD_FS_FT = F::FMT_FD_FS_FT, FMT_FD_FS = F::FMT_FD_FS, FMT_FD_FT = F::FMT_FD_FT,
            FMT_FS_FT = F::FMT_FS_FT, FMT_VU_FD_FS_FT = F::FMT_VU_FD_FS_FT,
            FMT_VU_FD_FS_FTBC = F::FMT_VU_FD_FS_FTBC, FMT_VU_FD_FS_Q = F::FMT_VU_FD_FS_Q,
            FMT_VU_FD_FS_I = F::FMT_VU_FD_FS_I, FMT_VU_ACC_FS_FT = F::FMT_VU_ACC_FS_FT,
            FMT_VU_ACC_FS_FTBC = F::FMT_VU_ACC_FS_FTBC, FMT_VU_ACC_FS_Q = F::FMT_VU_ACC_FS_Q,
            FMT_VU_ACC_FS_I = F::FMT_VU_ACC_FS_I, FMT_VU_FT_FS = F::FMT_VU_FT_FS,
            FMT_VU_CLIP = F::FMT_VU_CLIP, FMT_VU_ID_IS_IT = F::FMT_VU_ID_IS_IT,
            FMT_VU_IT_IS_IMM5 = F::FMT_VU_IT_IS_IMM5, FMT_VU_CALLMS = F::FMT_VU_CALLMS,
            FMT_VU_CALLMSR = F::FMT_VU_CALLMSR, FMT_VU_LQI = F::FMT_VU_LQI,
            FMT_VU_SQI = F::FMT_VU_SQI, FMT_VU_LQD = F::FMT_VU_LQD, FMT_VU_SQD = F::FMT_VU_SQD,
            FMT_VU_DIV = F::FMT_VU_DIV, FMT_VU_SQRT = F::FMT_VU_SQRT, FMT_VU_MTIR = F::FMT_VU_MTIR,
            FMT_VU_MFIR = F::FMT_VU_MFIR, FMT_VU_ILWR = F::FMT_VU_ILWR,
            FMT_VU_RNEXT = F::FMT_VU_RNEXT, FMT_VU_RINIT = F::FMT_VU_RINIT;

constexpr std::uint32_t BR = Flag::Branch, JMP = Flag::Jump, JREG = Flag::JumpReg,
                        LNK = Flag::Link, LKL = Flag::Likely, DLY = Flag::DelaySlot,
                        LD = Flag::Load, ST = Flag::Store, UNA = Flag::Unaligned, TRP = Flag::Trap,
                        EXC = Flag::Exception, OVF = Flag::Overflow, PRV = Flag::Privileged,
                        C0 = Flag::Cop0, FPU = Flag::Fpu, VU0 = Flag::Vu0, MMI = Flag::Mmi,
                        VMEM = Flag::VuMemory, ERT = Flag::ExceptionReturn;

constexpr std::uint32_t Z_RS = kFieldRs, Z_RT = kFieldRt, Z_RD = kFieldRd, Z_SA = kFieldSa,
                        Z_DEST = kFieldDest, Z_FTF = kFieldFtf;

constexpr std::array<OpInfo, kOpCount> kTable = {{
#define ANYPS2_OP(id, mnemonic, format, flags, memSize, zeroMask) \
    OpInfo{Op::id, #id, mnemonic, format, (flags), (memSize), (zeroMask)},
#include "anyps2/r5900/opcodes.def"
#undef ANYPS2_OP
}};

// Garante em tempo de compilação que a tabela está na mesma ordem do enum.
constexpr bool tableIsOrdered() {
    for (std::size_t i = 0; i < kTable.size(); ++i) {
        if (static_cast<std::size_t>(kTable[i].op) != i) return false;
    }
    return true;
}
static_assert(tableIsOrdered(), "opcodes.def: tabela fora de ordem");

}  // namespace

const std::array<OpInfo, kOpCount>& opTable() {
    return kTable;
}

}  // namespace anyps2::r5900
