#include "anyps2/r5900/decoder.h"

#include <array>

namespace anyps2::r5900 {
namespace {

using enum Op;

// ---------------------------------------------------------------------------
// Tabelas de despacho. Cada posição corresponde a um valor do campo que
// seleciona a operação; Invalid marca posições reservadas no R5900.
// Fonte: "EE Core Instruction Set Manual" (mapas de opcode do apêndice).
// ---------------------------------------------------------------------------

constexpr std::array<Op, 64> kPrimary = {
    // 0x00
    Invalid /*SPECIAL*/, Invalid /*REGIMM*/, J, JAL, BEQ, BNE, BLEZ, BGTZ,
    // 0x08
    ADDI, ADDIU, SLTI, SLTIU, ANDI, ORI, XORI, LUI,
    // 0x10
    Invalid /*COP0*/, Invalid /*COP1*/, Invalid /*COP2*/, Invalid, BEQL, BNEL, BLEZL, BGTZL,
    // 0x18
    DADDI, DADDIU, LDL, LDR, Invalid /*MMI*/, Invalid, LQ, SQ,
    // 0x20
    LB, LH, LWL, LW, LBU, LHU, LWR, LWU,
    // 0x28
    SB, SH, SWL, SW, SDL, SDR, SWR, CACHE,
    // 0x30 (o R5900 não tem LL/LWC2/LLD/LDC1)
    Invalid, LWC1, Invalid, PREF, Invalid, Invalid, LQC2, LD,
    // 0x38 (o R5900 não tem SC/SWC2/SCD/SDC1)
    Invalid, SWC1, Invalid, Invalid, Invalid, Invalid, SQC2, SD,
};

constexpr std::array<Op, 64> kSpecial = {
    // 0x00
    SLL, Invalid, SRL, SRA, SLLV, Invalid, SRLV, SRAV,
    // 0x08
    JR, JALR, MOVZ, MOVN, SYSCALL, BREAK, Invalid, SYNC,
    // 0x10
    MFHI, MTHI, MFLO, MTLO, DSLLV, Invalid, DSRLV, DSRAV,
    // 0x18 (sem DMULT/DDIV no R5900)
    MULT, MULTU, DIV, DIVU, Invalid, Invalid, Invalid, Invalid,
    // 0x20
    ADD, ADDU, SUB, SUBU, AND, OR, XOR, NOR,
    // 0x28
    MFSA, MTSA, SLT, SLTU, DADD, DADDU, DSUB, DSUBU,
    // 0x30
    TGE, TGEU, TLT, TLTU, TEQ, Invalid, TNE, Invalid,
    // 0x38
    DSLL, Invalid, DSRL, DSRA, DSLL32, Invalid, DSRL32, DSRA32,
};

constexpr std::array<Op, 32> kRegimm = {
    BLTZ,   BGEZ,   BLTZL,   BGEZL,   Invalid, Invalid, Invalid, Invalid,
    TGEI,   TGEIU,  TLTI,    TLTIU,   TEQI,    Invalid, TNEI,    Invalid,
    BLTZAL, BGEZAL, BLTZALL, BGEZALL, Invalid, Invalid, Invalid, Invalid,
    MTSAB,  MTSAH,  Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
};

constexpr std::array<Op, 64> kMmi = {
    // 0x00
    MADD, MADDU, Invalid, Invalid, PLZCW, Invalid, Invalid, Invalid,
    // 0x08
    Invalid /*MMI0*/, Invalid /*MMI2*/, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
    // 0x10
    MFHI1, MTHI1, MFLO1, MTLO1, Invalid, Invalid, Invalid, Invalid,
    // 0x18
    MULT1, MULTU1, DIV1, DIVU1, Invalid, Invalid, Invalid, Invalid,
    // 0x20
    MADD1, MADDU1, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
    // 0x28
    Invalid /*MMI1*/, Invalid /*MMI3*/, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
    // 0x30
    Invalid /*PMFHL*/, Invalid /*PMTHL*/, Invalid, Invalid, PSLLH, Invalid, PSRLH, PSRAH,
    // 0x38
    Invalid, Invalid, Invalid, Invalid, PSLLW, Invalid, PSRLW, PSRAW,
};

constexpr std::array<Op, 32> kMmi0 = {
    PADDW,  PSUBW,  PCGTW,  PMAXW,   PADDH,   PSUBH,   PCGTH,  PMAXH,
    PADDB,  PSUBB,  PCGTB,  Invalid, Invalid, Invalid, Invalid, Invalid,
    PADDSW, PSUBSW, PEXTLW, PPACW,   PADDSH,  PSUBSH,  PEXTLH, PPACH,
    PADDSB, PSUBSB, PEXTLB, PPACB,   Invalid, Invalid, PEXT5,  PPAC5,
};

constexpr std::array<Op, 32> kMmi1 = {
    Invalid, PABSW,   PCEQW,   PMINW,   PADSBH,  PABSH,   PCEQH,   PMINH,
    Invalid, Invalid, PCEQB,   Invalid, Invalid, Invalid, Invalid, Invalid,
    PADDUW,  PSUBUW,  PEXTUW,  Invalid, PADDUH,  PSUBUH,  PEXTUH,  Invalid,
    PADDUB,  PSUBUB,  PEXTUB,  QFSRV,   Invalid, Invalid, Invalid, Invalid,
};

constexpr std::array<Op, 32> kMmi2 = {
    PMADDW,  Invalid, PSLLVW,  PSRLVW, PMSUBW,  Invalid, Invalid, Invalid,
    PMFHI,   PMFLO,   PINTH,   Invalid, PMULTW, PDIVW,   PCPYLD,  Invalid,
    PMADDH,  PHMADH,  PAND,    PXOR,   PMSUBH,  PHMSBH,  Invalid, Invalid,
    Invalid, Invalid, PEXEH,   PREVH,  PMULTH,  PDIVBW,  PEXEW,   PROT3W,
};

constexpr std::array<Op, 32> kMmi3 = {
    PMADDUW, Invalid, Invalid, PSRAVW,  Invalid, Invalid, Invalid, Invalid,
    PMTHI,   PMTLO,   PINTEH,  Invalid, PMULTUW, PDIVUW,  PCPYUD,  Invalid,
    Invalid, Invalid, POR,     PNOR,    Invalid, Invalid, Invalid, Invalid,
    Invalid, Invalid, PEXCH,   PCPYH,   Invalid, Invalid, PEXCW,   Invalid,
};

constexpr std::array<Op, 8> kPmfhl = {
    PMFHL_LW, PMFHL_UW, PMFHL_SLW, PMFHL_LH, PMFHL_SH, Invalid, Invalid, Invalid,
};

// COP1, formato S (campo funct).
constexpr std::array<Op, 64> kCop1S = {
    // 0x00
    ADD_S, SUB_S, MUL_S, DIV_S, SQRT_S, ABS_S, MOV_S, NEG_S,
    // 0x08
    Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
    // 0x10
    Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, RSQRT_S, Invalid,
    // 0x18
    ADDA_S, SUBA_S, MULA_S, Invalid, MADD_S, MSUB_S, MADDA_S, MSUBA_S,
    // 0x20
    Invalid, Invalid, Invalid, Invalid, CVT_W_S, Invalid, Invalid, Invalid,
    // 0x28
    MAX_S, MIN_S, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
    // 0x30
    C_F_S, Invalid, C_EQ_S, Invalid, C_LT_S, Invalid, C_LE_S, Invalid,
    // 0x38
    Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
};

// COP2 "Special1": funct 0x00..0x3B (0x3C..0x3F levam à Special2).
constexpr std::array<Op, 64> kCop2Special1 = {
    // 0x00
    VADDbc, VADDbc, VADDbc, VADDbc, VSUBbc, VSUBbc, VSUBbc, VSUBbc,
    // 0x08
    VMADDbc, VMADDbc, VMADDbc, VMADDbc, VMSUBbc, VMSUBbc, VMSUBbc, VMSUBbc,
    // 0x10
    VMAXbc, VMAXbc, VMAXbc, VMAXbc, VMINIbc, VMINIbc, VMINIbc, VMINIbc,
    // 0x18
    VMULbc, VMULbc, VMULbc, VMULbc, VMULq, VMAXi, VMULi, VMINIi,
    // 0x20
    VADDq, VMADDq, VADDi, VMADDi, VSUBq, VMSUBq, VSUBi, VMSUBi,
    // 0x28
    VADD, VMADD, VMUL, VMAX, VSUB, VMSUB, VOPMSUB, VMINI,
    // 0x30
    VIADD, VISUB, VIADDI, Invalid, VIAND, VIOR, Invalid, Invalid,
    // 0x38
    VCALLMS, VCALLMSR, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid,
};

// COP2 "Special2": índice de 7 bits = (bits 10..6 << 2) | bits 1..0.
constexpr std::array<Op, 128> kCop2Special2 = [] {
    std::array<Op, 128> t{};
    for (auto& e : t) e = Invalid;
    const Op first[] = {
        // 0x00
        VADDAbc, VADDAbc, VADDAbc, VADDAbc, VSUBAbc, VSUBAbc, VSUBAbc, VSUBAbc,
        // 0x08
        VMADDAbc, VMADDAbc, VMADDAbc, VMADDAbc, VMSUBAbc, VMSUBAbc, VMSUBAbc, VMSUBAbc,
        // 0x10
        VITOF0, VITOF4, VITOF12, VITOF15, VFTOI0, VFTOI4, VFTOI12, VFTOI15,
        // 0x18
        VMULAbc, VMULAbc, VMULAbc, VMULAbc, VMULAq, VABS, VMULAi, VCLIPw,
        // 0x20
        VADDAq, VMADDAq, VADDAi, VMADDAi, VSUBAq, VMSUBAq, VSUBAi, VMSUBAi,
        // 0x28
        VADDA, VMADDA, VMULA, Invalid, VSUBA, VMSUBA, VOPMULA, VNOP,
        // 0x30
        VMOVE, VMR32, Invalid, Invalid, VLQI, VSQI, VLQD, VSQD,
        // 0x38
        VDIV, VSQRT, VRSQRT, VWAITQ, VMTIR, VMFIR, VILWR, VISWR,
        // 0x40
        VRNEXT, VRGET, VRINIT, VRXOR,
    };
    for (std::size_t i = 0; i < sizeof(first) / sizeof(first[0]); ++i) t[i] = first[i];
    return t;
}();

// ---------------------------------------------------------------------------
// Decodificadores por grupo
// ---------------------------------------------------------------------------

Op decodeCop0(std::uint32_t raw) {
    const std::uint32_t rs = (raw >> 21) & 0x1F;
    const std::uint32_t rt = (raw >> 16) & 0x1F;
    const std::uint32_t rd = (raw >> 11) & 0x1F;
    const std::uint32_t funct = raw & 0x3F;
    switch (rs) {
        case 0x00:  // MF0
        case 0x04:  // MT0
        {
            const bool from = (rs == 0);
            if (rd == 24) {  // registradores de debug (breakpoint)
                static constexpr Op kMf[8] = {MFBPC, Invalid, MFIAB, MFIABM,
                                              MFDAB, MFDABM,  MFDVB, MFDVBM};
                static constexpr Op kMt[8] = {MTBPC, Invalid, MTIAB, MTIABM,
                                              MTDAB, MTDABM,  MTDVB, MTDVBM};
                if (funct >= 8) return Invalid;
                return from ? kMf[funct] : kMt[funct];
            }
            if (rd == 25) {  // contadores de performance
                const bool counter = (raw & 1) != 0;
                if (from) return counter ? MFPC : MFPS;
                return counter ? MTPC : MTPS;
            }
            return from ? MFC0 : MTC0;
        }
        case 0x08: {  // BC0
            static constexpr Op kBc[4] = {BC0F, BC0T, BC0FL, BC0TL};
            return rt < 4 ? kBc[rt] : Invalid;
        }
        case 0x10: {  // C0
            switch (funct) {
                case 0x01: return TLBR;
                case 0x02: return TLBWI;
                case 0x06: return TLBWR;
                case 0x08: return TLBP;
                case 0x18: return ERET;
                case 0x38: return EI;
                case 0x39: return DI;
                default: return Invalid;
            }
        }
        default:
            return Invalid;
    }
}

Op decodeCop1(std::uint32_t raw) {
    const std::uint32_t fmt = (raw >> 21) & 0x1F;
    const std::uint32_t rt = (raw >> 16) & 0x1F;
    const std::uint32_t funct = raw & 0x3F;
    switch (fmt) {
        case 0x00: return MFC1;
        case 0x02: return CFC1;
        case 0x04: return MTC1;
        case 0x06: return CTC1;
        case 0x08: {
            static constexpr Op kBc[4] = {BC1F, BC1T, BC1FL, BC1TL};
            return rt < 4 ? kBc[rt] : Invalid;
        }
        case 0x10: return kCop1S[funct];                 // formato S
        case 0x14: return funct == 0x20 ? CVT_S_W : Invalid;  // formato W
        default: return Invalid;
    }
}

Op decodeCop2(std::uint32_t raw) {
    const std::uint32_t rs = (raw >> 21) & 0x1F;
    const std::uint32_t rt = (raw >> 16) & 0x1F;
    if (rs & 0x10) {  // bit CO: instrução de VU0 em modo macro
        const std::uint32_t funct = raw & 0x3F;
        if (funct < 0x3C) return kCop2Special1[funct];
        const std::uint32_t index = (((raw >> 6) & 0x1F) << 2) | (raw & 0x3);
        return kCop2Special2[index];
    }
    switch (rs) {
        case 0x01: return QMFC2;
        case 0x02: return CFC2;
        case 0x05: return QMTC2;
        case 0x06: return CTC2;
        case 0x08: {
            static constexpr Op kBc[4] = {BC2F, BC2T, BC2FL, BC2TL};
            return rt < 4 ? kBc[rt] : Invalid;
        }
        default: return Invalid;
    }
}

Op decodeMmi(std::uint32_t raw) {
    const std::uint32_t funct = raw & 0x3F;
    const std::uint32_t sa = (raw >> 6) & 0x1F;
    switch (funct) {
        case 0x08: return kMmi0[sa];
        case 0x28: return kMmi1[sa];
        case 0x09: return kMmi2[sa];
        case 0x29: return kMmi3[sa];
        case 0x30: return sa < 8 ? kPmfhl[sa] : Invalid;
        case 0x31: return sa == 0 ? PMTHL_LW : Invalid;
        default: return kMmi[funct];
    }
}

Op decodeOp(std::uint32_t raw) {
    const std::uint32_t opcode = raw >> 26;
    switch (opcode) {
        case 0x00: return kSpecial[raw & 0x3F];
        case 0x01: return kRegimm[(raw >> 16) & 0x1F];
        case 0x10: return decodeCop0(raw);
        case 0x11: return decodeCop1(raw);
        case 0x12: return decodeCop2(raw);
        case 0x1C: return decodeMmi(raw);
        default: return kPrimary[opcode];
    }
}

// Regras de canonicidade que não cabem numa máscara de zeros.
bool extraCanonical(Op op, std::uint32_t raw) {
    const std::uint32_t dest = (raw >> 21) & 0xF;
    switch (op) {
        case VOPMULA:
        case VOPMSUB:
        case VCLIPw:
            return dest == 0xE;  // operam sempre sobre xyz
        case VILWR:
        case VISWR:
            // Exatamente um componente (x, y, z ou w).
            return dest == 0x8 || dest == 0x4 || dest == 0x2 || dest == 0x1;
        default:
            return true;
    }
}

}  // namespace

Instruction decode(std::uint32_t raw, std::uint32_t address) {
    Instruction insn;
    insn.address = address;
    insn.raw = raw;
    insn.op = decodeOp(raw);
    if (insn.op == Invalid) {
        insn.canonical = false;
        return insn;
    }
    const OpInfo& info = opInfo(insn.op);
    insn.canonical = (raw & info.zeroMask) == 0 && extraCanonical(insn.op, raw);
    return insn;
}

bool Instruction::isUnconditionalBranch() const {
    switch (op) {
        case Op::BEQ:
        case Op::BEQL:
            return rs() == rt();
        case Op::BGEZ:
        case Op::BGEZL:
        case Op::BGEZAL:
        case Op::BGEZALL:
        case Op::BLEZ:
        case Op::BLEZL:
            return rs() == 0;
        default:
            return false;
    }
}

}  // namespace anyps2::r5900
