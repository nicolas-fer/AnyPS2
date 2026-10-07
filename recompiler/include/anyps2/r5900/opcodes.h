#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace anyps2::r5900 {

// Sintaxe dos operandos de cada instrução (estilo GNU binutils, -M no-aliases).
enum class Format : std::uint8_t {
    FMT_NONE,           // eret, vnop
    FMT_SYNC,           // sync | sync.p
    FMT_RD_RS_RT,       // addu rd,rs,rt
    FMT_RDOPT_RS_RT,    // mult [rd,]rs,rt (rd omitido quando é zero)
    FMT_RD_RT_RS,       // sllv rd,rt,rs
    FMT_RD_RT_SA,       // sll rd,rt,sa
    FMT_RD_RT,          // pabsw rd,rt
    FMT_RD_RS,          // plzcw rd,rs
    FMT_RD,             // mfhi rd
    FMT_RS,             // jr rs
    FMT_RS_RT,          // pdivw rs,rt
    FMT_ZERO_RS_RT,     // div zero,rs,rt
    FMT_JALR,           // jalr rd,rs
    FMT_RT_RS_SIMM,     // addiu rt,rs,simm
    FMT_RT_RS_UIMM,     // ori rt,rs,0xuimm
    FMT_RT_UIMM,        // lui rt,0xuimm
    FMT_RS_RT_BRANCH,   // beq rs,rt,alvo
    FMT_RS_BRANCH,      // bgez rs,alvo
    FMT_BRANCH,         // bc1t alvo
    FMT_JUMP,           // j alvo
    FMT_MEM_GPR,        // lw rt,off(base)
    FMT_MEM_FPR,        // lwc1 $ft,off(base)
    FMT_MEM_VF,         // lqc2 $vfN,off(base)
    FMT_CACHE,          // cache op,off(base)
    FMT_SYSCALL,        // syscall [code]
    FMT_BREAK,          // break [c1[,c2]]
    FMT_TRAP,           // tge rs,rt[,code]
    FMT_RS_SIMM,        // tgei rs,simm
    FMT_RT_COP0,        // mfc0 rt,c0_reg
    FMT_RT,             // mfbpc rt
    FMT_RT_PERF,        // mfpc rt,reg
    FMT_RT_FS,          // mfc1 rt,$fs
    FMT_RT_FCR,         // cfc1 rt,$fcr
    FMT_RT_VF,          // qmfc2[.i] rt,$vfN
    FMT_RT_VI,          // cfc2[.i] rt,$viN
    FMT_FD_FS_FT,       // add.s $fd,$fs,$ft
    FMT_FD_FS,          // mov.s $fd,$fs
    FMT_FD_FT,          // sqrt.s $fd,$ft
    FMT_FS_FT,          // c.eq.s $fs,$ft
    FMT_VU_FD_FS_FT,    // vadd.dest fd,fs,ft
    FMT_VU_FD_FS_FTBC,  // vaddx.dest fd,fs,ftx
    FMT_VU_FD_FS_Q,     // vaddq.dest fd,fs,$Q
    FMT_VU_FD_FS_I,     // vaddi.dest fd,fs,$I
    FMT_VU_ACC_FS_FT,   // vadda.dest $ACC,fs,ft
    FMT_VU_ACC_FS_FTBC, // vaddax.dest $ACC,fs,ftx
    FMT_VU_ACC_FS_Q,    // vaddaq.dest $ACC,fs,$Q
    FMT_VU_ACC_FS_I,    // vaddai.dest $ACC,fs,$I
    FMT_VU_FT_FS,       // vmove.dest ft,fs
    FMT_VU_CLIP,        // vclipw.dest fs,ftw
    FMT_VU_ID_IS_IT,    // viadd $vid,$vis,$vit
    FMT_VU_IT_IS_IMM5,  // viaddi $vit,$vis,imm5
    FMT_VU_CALLMS,      // vcallms 0xaddr
    FMT_VU_CALLMSR,     // vcallmsr $vis
    FMT_VU_LQI,         // vlqi.dest ft,($vis++)
    FMT_VU_SQI,         // vsqi.dest fs,($vit++)
    FMT_VU_LQD,         // vlqd.dest ft,(--$vis)
    FMT_VU_SQD,         // vsqd.dest fs,(--$vit)
    FMT_VU_DIV,         // vdiv $Q,fsfsf,ftftf
    FMT_VU_SQRT,        // vsqrt $Q,ftftf
    FMT_VU_MTIR,        // vmtir $vit,fsfsf
    FMT_VU_MFIR,        // vmfir.dest ft,$vis
    FMT_VU_ILWR,        // vilwr.dest $vit,($vis)
    FMT_VU_RNEXT,       // vrnext.dest ft,$R
    FMT_VU_RINIT,       // vrinit $R,fsfsf
};

// Propriedades semânticas usadas pela análise de fluxo e pelo gerador.
namespace Flag {
inline constexpr std::uint32_t Branch = 1u << 0;      // desvio condicional relativo ao PC
inline constexpr std::uint32_t Jump = 1u << 1;        // J/JAL (alvo absoluto na região de 256 MB)
inline constexpr std::uint32_t JumpReg = 1u << 2;     // JR/JALR (alvo em registrador)
inline constexpr std::uint32_t Link = 1u << 3;        // grava endereço de retorno
inline constexpr std::uint32_t Likely = 1u << 4;      // anula o delay slot se não desviar
inline constexpr std::uint32_t DelaySlot = 1u << 5;   // possui delay slot
inline constexpr std::uint32_t Load = 1u << 6;        // lê memória do EE
inline constexpr std::uint32_t Store = 1u << 7;       // escreve memória do EE
inline constexpr std::uint32_t Unaligned = 1u << 8;   // LWL/LWR/LDL/... (acesso parcial)
inline constexpr std::uint32_t Trap = 1u << 9;        // trap condicional
inline constexpr std::uint32_t Exception = 1u << 10;  // SYSCALL/BREAK
inline constexpr std::uint32_t Overflow = 1u << 11;   // pode gerar exceção de overflow
inline constexpr std::uint32_t Privileged = 1u << 12; // requer modo kernel
inline constexpr std::uint32_t Cop0 = 1u << 13;
inline constexpr std::uint32_t Fpu = 1u << 14;        // COP1
inline constexpr std::uint32_t Vu0 = 1u << 15;        // COP2 (VU0 em modo macro)
inline constexpr std::uint32_t Mmi = 1u << 16;        // multimídia 128 bits / pipeline 1
inline constexpr std::uint32_t VuMemory = 1u << 17;   // acessa a memória de dados do VU0
inline constexpr std::uint32_t ExceptionReturn = 1u << 18;  // ERET
}  // namespace Flag

// Máscaras de campos (para MascaraZero em opcodes.def).
inline constexpr std::uint32_t kFieldRs = 0x03E00000u;
inline constexpr std::uint32_t kFieldRt = 0x001F0000u;
inline constexpr std::uint32_t kFieldRd = 0x0000F800u;
inline constexpr std::uint32_t kFieldSa = 0x000007C0u;
inline constexpr std::uint32_t kFieldDest = 0x01E00000u;  // VU: x=bit24 y=23 z=22 w=21
inline constexpr std::uint32_t kFieldFsf = 0x00600000u;   // VU: componente de fs (bits 22..21)
inline constexpr std::uint32_t kFieldFtf = 0x01800000u;   // VU: componente de ft (bits 24..23)

enum class Op : std::uint16_t {
#define ANYPS2_OP(id, mnemonic, format, flags, memSize, zeroMask) id,
#include "anyps2/r5900/opcodes.def"
#undef ANYPS2_OP
    Count_
};

inline constexpr std::size_t kOpCount = static_cast<std::size_t>(Op::Count_);

struct OpInfo {
    Op op;
    std::string_view name;      // identificador C++ (ex.: "ADD_S")
    std::string_view mnemonic;  // mnemônico GNU base (ex.: "add.s")
    Format format;
    std::uint32_t flags;
    std::uint8_t memSize;
    std::uint32_t zeroMask;
};

// Tabela indexada por Op.
const std::array<OpInfo, kOpCount>& opTable();

inline const OpInfo& opInfo(Op op) {
    return opTable()[static_cast<std::size_t>(op)];
}

}  // namespace anyps2::r5900
