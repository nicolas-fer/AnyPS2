#include "anyps2/vu/isa.h"

namespace anyps2::vu {

namespace {

constexpr std::uint32_t bits(std::uint32_t v, unsigned lo, unsigned n) {
    return (v >> lo) & ((1u << n) - 1);
}

std::int32_t sext(std::uint32_t v, unsigned n) {
    const std::uint32_t m = 1u << (n - 1);
    return static_cast<std::int32_t>((v ^ m) - m);
}

Upper decodeUpper(std::uint32_t w) {
    Upper u;
    u.dest = static_cast<std::uint8_t>(bits(w, 21, 4));
    u.ft = static_cast<std::uint8_t>(bits(w, 16, 5));
    u.fs = static_cast<std::uint8_t>(bits(w, 11, 5));
    u.fd = static_cast<std::uint8_t>(bits(w, 6, 5));
    const std::uint32_t f = bits(w, 0, 6);
    u.bc = static_cast<std::uint8_t>(f & 3);
    bool ftUnused = false;
    if (f < 0x1C) {
        static constexpr U kBc[7] = {U::ADDbc, U::SUBbc, U::MADDbc, U::MSUBbc, U::MAXbc, U::MINIbc, U::MULbc};
        u.op = kBc[f >> 2];
    } else if (f < 0x30) {
        static constexpr U kOps[20] = {U::MULq, U::MAXi,  U::MULi, U::MINIi,  U::ADDq, U::MADDq, U::ADDi,
                                       U::MADDi, U::SUBq, U::MSUBq, U::SUBi, U::MSUBi, U::ADD,  U::MADD,
                                       U::MUL,  U::MAX,  U::SUB,  U::MSUB,  U::OPMSUB, U::MINI};
        u.op = kOps[f - 0x1C];
        ftUnused = f < 0x28;
    } else if (f >= 0x3C) {
        const std::uint32_t idx = (std::uint32_t{u.fd} << 2) | (f & 3);
        if (idx < 0x10) {
            static constexpr U kAbc[4] = {U::ADDAbc, U::SUBAbc, U::MADDAbc, U::MSUBAbc};
            u.op = kAbc[idx >> 2];
        } else if (idx < 0x14) {
            static constexpr U kItof[4] = {U::ITOF0, U::ITOF4, U::ITOF12, U::ITOF15};
            u.op = kItof[idx & 3];
        } else if (idx < 0x18) {
            static constexpr U kFtoi[4] = {U::FTOI0, U::FTOI4, U::FTOI12, U::FTOI15};
            u.op = kFtoi[idx & 3];
        } else if (idx < 0x1C) {
            u.op = U::MULAbc;
        } else if (idx < 0x30) {
            static constexpr U kOps[20] = {U::MULAq, U::ABS,    U::MULAi, U::CLIP,    U::ADDAq,  U::MADDAq, U::ADDAi,
                                           U::MADDAi, U::SUBAq, U::MSUBAq, U::SUBAi, U::MSUBAi, U::ADDA,  U::MADDA,
                                           U::MULA,  U::INVALID, U::SUBA, U::MSUBA, U::OPMULA, U::NOP};
            u.op = kOps[idx - 0x1C];
            ftUnused = (idx == 0x1C || idx == 0x1E || (idx >= 0x20 && idx <= 0x27));
        } else {
            u.op = U::INVALID;
        }
    } else {
        u.op = U::INVALID;
    }
    bool canon = bits(w, 25, 2) == 0;
    if (ftUnused && u.ft != 0) canon = false;
    switch (u.op) {
        case U::CLIP:
            if (u.dest != 0xE) canon = false;
            break;
        case U::NOP:
            if (u.dest != 0 || u.ft != 0 || u.fs != 0) canon = false;
            break;
        default:
            break;
    }
    u.canonical = canon;
    return u;
}

Lower decodeLower(std::uint32_t w) {
    Lower l;
    l.dest = static_cast<std::uint8_t>(bits(w, 21, 4));
    l.ft = static_cast<std::uint8_t>(bits(w, 16, 5));
    l.fs = static_cast<std::uint8_t>(bits(w, 11, 5));
    l.fd = static_cast<std::uint8_t>(bits(w, 6, 5));
    l.fsf = static_cast<std::uint8_t>(bits(w, 21, 2));
    l.ftf = static_cast<std::uint8_t>(bits(w, 23, 2));
    const std::uint32_t op = bits(w, 25, 7);
    const std::int32_t imm11 = sext(bits(w, 0, 11), 11);
    // Campos que precisam ser zero na forma canônica.
    std::uint32_t zero = 0;
    constexpr std::uint32_t kDest = 0xFu << 21, kFt = 0x1Fu << 16, kFs = 0x1Fu << 11,
                            kImm11 = 0x7FFu;
    switch (op) {
        case 0x00: l.op = L::LQ; l.imm = imm11; break;
        case 0x01: l.op = L::SQ; l.imm = imm11; break;
        case 0x04: l.op = L::ILW; l.imm = imm11; break;
        case 0x05: l.op = L::ISW; l.imm = imm11; break;
        case 0x08: case 0x09:
            l.op = op == 0x08 ? L::IADDIU : L::ISUBIU;
            l.imm = static_cast<std::int32_t>(bits(w, 0, 11) | (bits(w, 21, 4) << 11));
            break;
        case 0x10: case 0x11: case 0x12: case 0x13: {
            static constexpr L kOps[4] = {L::FCEQ, L::FCSET, L::FCAND, L::FCOR};
            l.op = kOps[op - 0x10];
            l.imm = static_cast<std::int32_t>(bits(w, 0, 24));
            zero = 1u << 24;
            break;
        }
        case 0x14: case 0x15: case 0x16: case 0x17: {
            static constexpr L kOps[4] = {L::FSEQ, L::FSSET, L::FSAND, L::FSOR};
            l.op = kOps[op - 0x14];
            l.imm = static_cast<std::int32_t>(bits(w, 0, 11) | (bits(w, 21, 1) << 11));
            zero = (0x7u << 22) | kFs | (op == 0x15 ? kFt | 0x3Fu : 0);  // FSSET só escreve as flags fixas
            break;
        }
        case 0x18: l.op = L::FMEQ; zero = kDest | kImm11; break;
        case 0x1A: l.op = L::FMAND; zero = kDest | kImm11; break;
        case 0x1B: l.op = L::FMOR; zero = kDest | kImm11; break;
        case 0x1C: l.op = L::FCGET; zero = kDest | kFs | kImm11; break;
        case 0x20: l.op = L::B; l.imm = imm11; zero = kDest | kFt | kFs; break;
        case 0x21: l.op = L::BAL; l.imm = imm11; zero = kDest | kFs; break;
        case 0x24: l.op = L::JR; zero = kDest | kFt | kImm11; break;
        case 0x25: l.op = L::JALR; zero = kDest | kImm11; break;
        case 0x28: l.op = L::IBEQ; l.imm = imm11; zero = kDest; break;
        case 0x29: l.op = L::IBNE; l.imm = imm11; zero = kDest; break;
        case 0x2C: l.op = L::IBLTZ; l.imm = imm11; zero = kDest | kFt; break;
        case 0x2D: l.op = L::IBGTZ; l.imm = imm11; zero = kDest | kFt; break;
        case 0x2E: l.op = L::IBLEZ; l.imm = imm11; zero = kDest | kFt; break;
        case 0x2F: l.op = L::IBGEZ; l.imm = imm11; zero = kDest | kFt; break;
        case 0x40: {
            const std::uint32_t f = bits(w, 0, 6);
            switch (f) {
                case 0x30: l.op = L::IADD; zero = kDest; break;
                case 0x31: l.op = L::ISUB; zero = kDest; break;
                case 0x32: l.op = L::IADDI; l.imm = sext(l.fd, 5); zero = kDest; break;
                case 0x34: l.op = L::IAND; zero = kDest; break;
                case 0x35: l.op = L::IOR; zero = kDest; break;
                case 0x3C: case 0x3D: case 0x3E: case 0x3F: {
                    const std::uint32_t idx = (std::uint32_t{l.fd} << 2) | (f & 3);
                    switch (idx) {
                        case 0x30: l.op = (w == 0x8000033Cu) ? L::NOP : L::MOVE; break;
                        case 0x31: l.op = L::MR32; break;
                        case 0x34: l.op = L::LQI; break;
                        case 0x35: l.op = L::SQI; break;
                        case 0x36: l.op = L::LQD; break;
                        case 0x37: l.op = L::SQD; break;
                        case 0x38: l.op = L::DIV; break;
                        case 0x39: l.op = L::SQRT; zero = (0x3u << 21) | kFs; break;
                        case 0x3A: l.op = L::RSQRT; break;
                        case 0x3B: l.op = L::WAITQ; zero = kDest | kFt | kFs; break;
                        case 0x3C: l.op = L::MTIR; zero = 0x3u << 23; break;
                        case 0x3D: l.op = L::MFIR; break;
                        case 0x3E: l.op = L::ILWR; break;
                        case 0x3F: l.op = L::ISWR; break;
                        case 0x40: l.op = L::RNEXT; zero = kFs; break;
                        case 0x41: l.op = L::RGET; zero = kFs; break;
                        case 0x42: l.op = L::RINIT; zero = (0x3u << 23) | kFt; break;
                        case 0x43: l.op = L::RXOR; zero = (0x3u << 23) | kFt; break;
                        case 0x64: l.op = L::MFP; zero = kFs; break;
                        case 0x68: l.op = L::XTOP; zero = kDest | kFs; break;
                        case 0x69: l.op = L::XITOP; zero = kDest | kFs; break;
                        case 0x6C: l.op = L::XGKICK; zero = kDest | kFt; break;
                        case 0x70: l.op = L::ESADD; zero = kFt; break;
                        case 0x71: l.op = L::ERSADD; zero = kFt; break;
                        case 0x72: l.op = L::ELENG; zero = kFt; break;
                        case 0x73: l.op = L::ERLENG; zero = kFt; break;
                        case 0x74: l.op = L::EATANxy; zero = kFt; break;
                        case 0x75: l.op = L::EATANxz; zero = kFt; break;
                        case 0x76: l.op = L::ESUM; zero = kFt; break;
                        case 0x78: l.op = L::ESQRT; zero = (0x3u << 23) | kFt; break;
                        case 0x79: l.op = L::ERSQRT; zero = (0x3u << 23) | kFt; break;
                        case 0x7A: l.op = L::ERCPR; zero = (0x3u << 23) | kFt; break;
                        case 0x7B: l.op = L::WAITP; zero = kDest | kFt | kFs; break;
                        case 0x7C: l.op = L::ESIN; zero = (0x3u << 23) | kFt; break;
                        case 0x7D: l.op = L::EATAN; zero = (0x3u << 23) | kFt; break;
                        case 0x7E: l.op = L::EEXP; zero = (0x3u << 23) | kFt; break;
                        default: l.op = L::INVALID; break;
                    }
                    break;
                }
                default: l.op = L::INVALID; break;
            }
            break;
        }
        default:
            l.op = L::INVALID;
            break;
    }
    // Instruções de vetor do EFU têm a máscara de destino fixa.
    switch (l.op) {
        case L::ESADD: case L::ERSADD: case L::ELENG: case L::ERLENG:
            if (l.dest != 0xE) l.canonical = false;
            break;
        case L::EATANxy: if (l.dest != 0xC) l.canonical = false; break;
        case L::EATANxz: if (l.dest != 0xA) l.canonical = false; break;
        case L::ESUM: if (l.dest != 0xF) l.canonical = false; break;
        default: break;
    }
    if (w & zero) l.canonical = false;
    return l;
}

}  // namespace

Instr decode(std::uint32_t lowerWord, std::uint32_t upperWord) {
    Instr in;
    in.lowerWord = lowerWord;
    in.upperWord = upperWord;
    in.i = (upperWord >> 31) & 1;
    in.e = (upperWord >> 30) & 1;
    in.m = (upperWord >> 29) & 1;
    in.d = (upperWord >> 28) & 1;
    in.t = (upperWord >> 27) & 1;
    in.upper = decodeUpper(upperWord);
    if (in.i) {
        in.lower.op = L::LOI;
        in.lower.imm = static_cast<std::int32_t>(lowerWord);
    } else {
        in.lower = decodeLower(lowerWord);
    }
    return in;
}

bool isBranch(L op) {
    switch (op) {
        case L::B: case L::BAL: case L::JR: case L::JALR: case L::IBEQ: case L::IBNE: case L::IBLTZ:
        case L::IBGTZ: case L::IBLEZ: case L::IBGEZ:
            return true;
        default:
            return false;
    }
}

bool isConditionalBranch(L op) {
    switch (op) {
        case L::IBEQ: case L::IBNE: case L::IBLTZ: case L::IBGTZ: case L::IBLEZ: case L::IBGEZ: return true;
        default: return false;
    }
}

bool isIndirect(L op) { return op == L::JR || op == L::JALR; }
bool isLink(L op) { return op == L::BAL || op == L::JALR; }

std::uint32_t branchTarget(const Instr& in, std::uint32_t pc) {
    return pc + 8 + static_cast<std::uint32_t>(in.lower.imm * 8);
}

}  // namespace anyps2::vu
