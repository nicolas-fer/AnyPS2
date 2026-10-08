#include <cstdio>
#include <cstring>

#include "anyps2/vu/isa.h"

namespace anyps2::vu {

namespace {

const char* const kUpperNames[] = {
    "add", "sub", "madd", "msub", "max", "mini", "mul", "opmsub",
    "add", "sub", "madd", "msub", "max", "mini", "mul",
    "addq", "subq", "maddq", "msubq", "mulq",
    "addi", "subi", "maddi", "msubi", "maxi", "minii", "muli",
    "adda", "suba", "madda", "msuba", "mula", "opmula",
    "adda", "suba", "madda", "msuba", "mula",
    "addaq", "subaq", "maddaq", "msubaq", "mulaq",
    "addai", "subai", "maddai", "msubai", "mulai",
    "abs", "itof0", "itof4", "itof12", "itof15", "ftoi0", "ftoi4", "ftoi12", "ftoi15", "clip", "nop",
    "*unknown*",
};
static_assert(sizeof(kUpperNames) / sizeof(kUpperNames[0]) == static_cast<std::size_t>(U::COUNT_));

const char* const kLowerNames[] = {
    "lq", "sq", "ilw", "isw", "iaddiu", "isubiu",
    "fceq", "fcset", "fcand", "fcor", "fseq", "fsset", "fsand", "fsor", "fmeq", "fmand", "fmor", "fcget",
    "b", "bal", "jr", "jalr", "ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez",
    "iadd", "isub", "iaddi", "iand", "ior",
    "move", "mr32", "lqi", "sqi", "lqd", "sqd", "div", "sqrt", "rsqrt", "waitq", "mtir", "mfir", "ilwr", "iswr",
    "rnext", "rget", "rinit", "rxor", "mfp", "xtop", "xitop", "xgkick",
    "esadd", "ersadd", "eleng", "erleng", "eatanxy", "eatanxz", "esum", "esqrt", "ersqrt", "ercpr", "waitp",
    "esin", "eatan", "eexp",
    "nop", "loi", "*unknown*",
};
static_assert(sizeof(kLowerNames) / sizeof(kLowerNames[0]) == static_cast<std::size_t>(L::COUNT_));

std::string field(unsigned dest) {
    std::string s;
    if (dest & 8) s += 'x';
    if (dest & 4) s += 'y';
    if (dest & 2) s += 'z';
    if (dest & 1) s += 'w';
    return s;
}

char comp(unsigned c) { return "xyzw"[c & 3]; }

std::string vf(unsigned r, const std::string& f) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "vf%02u", r);
    return buf + f;
}

std::string vfc(unsigned r, unsigned c) {
    return vf(r, std::string(1, comp(c)));
}

std::string vi(unsigned r) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "vi%02u", r);
    return buf;
}

// Imediatos sem sinal como o dvp-objdump: decimal abaixo de 16, senão hexa.
std::string uimm(std::uint32_t v) {
    char buf[16];
    if (v < 16) std::snprintf(buf, sizeof(buf), "%u", v);
    else std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

std::string addr(std::uint32_t a) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", a);
    return buf;
}

std::string flags(const Instr& in) {
    std::string s;
    if (in.i) s += 'i';
    if (in.e) s += 'e';
    if (in.m) s += 'm';
    if (in.d) s += 'd';
    if (in.t) s += 't';
    return s.empty() ? s : "[" + s + "]";
}

bool isBc(U op) {
    switch (op) {
        case U::ADDbc: case U::SUBbc: case U::MADDbc: case U::MSUBbc: case U::MAXbc: case U::MINIbc: case U::MULbc:
        case U::ADDAbc: case U::SUBAbc: case U::MADDAbc: case U::MSUBAbc: case U::MULAbc: case U::CLIP:
            return true;
        default:
            return false;
    }
}

bool writesAcc(U op) {
    switch (op) {
        case U::ADDA: case U::SUBA: case U::MADDA: case U::MSUBA: case U::MULA: case U::OPMULA:
        case U::ADDAbc: case U::SUBAbc: case U::MADDAbc: case U::MSUBAbc: case U::MULAbc:
        case U::ADDAq: case U::SUBAq: case U::MADDAq: case U::MSUBAq: case U::MULAq:
        case U::ADDAi: case U::SUBAi: case U::MADDAi: case U::MSUBAi: case U::MULAi:
            return true;
        default:
            return false;
    }
}

}  // namespace

const char* upperName(U op) { return kUpperNames[static_cast<std::size_t>(op)]; }
const char* lowerName(L op) { return kLowerNames[static_cast<std::size_t>(op)]; }

std::string upperText(const Instr& in) {
    const Upper& u = in.upper;
    if (u.op == U::INVALID) return "*unknown*";
    std::string name = upperName(u.op);
    if (isBc(u.op)) name += comp(u.bc);
    name += flags(in);
    if (u.op == U::NOP) return name;
    const std::string f = field(u.dest);
    name += "." + f + " ";
    const std::string dst = writesAcc(u.op) ? "acc" + f : vf(u.fd, f);
    switch (u.op) {
        case U::ABS: case U::ITOF0: case U::ITOF4: case U::ITOF12: case U::ITOF15:
        case U::FTOI0: case U::FTOI4: case U::FTOI12: case U::FTOI15:
            return name + vf(u.ft, f) + "," + vf(u.fs, f);
        case U::CLIP:
            return name + vf(u.fs, f) + "," + vfc(u.ft, u.bc);
        default:
            break;
    }
    std::string src2;
    switch (u.op) {
        case U::ADDq: case U::SUBq: case U::MADDq: case U::MSUBq: case U::MULq:
        case U::ADDAq: case U::SUBAq: case U::MADDAq: case U::MSUBAq: case U::MULAq:
            src2 = "q";
            break;
        case U::ADDi: case U::SUBi: case U::MADDi: case U::MSUBi: case U::MAXi: case U::MINIi: case U::MULi:
        case U::ADDAi: case U::SUBAi: case U::MADDAi: case U::MSUBAi: case U::MULAi:
            src2 = "i";
            break;
        default:
            src2 = isBc(u.op) ? vfc(u.ft, u.bc) : vf(u.ft, f);
            break;
    }
    return name + dst + "," + vf(u.fs, f) + "," + src2;
}

std::string lowerText(const Instr& in, std::uint32_t pc) {
    const Lower& l = in.lower;
    const std::string name = lowerName(l.op);
    const std::string f = field(l.dest);
    const std::string dotted = name + "." + f + " ";
    switch (l.op) {
        case L::INVALID: return "*unknown*";
        case L::NOP: return "nop";
        case L::LOI: {
            float v;
            const auto w = static_cast<std::uint32_t>(l.imm);
            std::memcpy(&v, &w, 4);
            char buf[48];
            std::snprintf(buf, sizeof(buf), "loi %g", static_cast<double>(v));
            return buf;
        }
        case L::LQ: return dotted + vf(l.ft, f) + "," + std::to_string(l.imm) + "(" + vi(l.fs) + ")";
        case L::SQ: return dotted + vf(l.fs, f) + "," + std::to_string(l.imm) + "(" + vi(l.ft) + ")";
        case L::ILW: case L::ISW:
            return dotted + vi(l.ft) + "," + std::to_string(l.imm) + "(" + vi(l.fs) + ")" + f;
        case L::IADDIU: case L::ISUBIU:
            return name + " " + vi(l.ft) + "," + vi(l.fs) + "," + uimm(static_cast<std::uint32_t>(l.imm));
        case L::FCEQ: case L::FCAND: case L::FCOR:
            return name + " vi01," + uimm(static_cast<std::uint32_t>(l.imm));
        case L::FCSET: case L::FSSET:
            return name + " " + uimm(static_cast<std::uint32_t>(l.imm));
        case L::FSEQ: case L::FSAND: case L::FSOR:
            return name + " " + vi(l.ft) + "," + uimm(static_cast<std::uint32_t>(l.imm));
        case L::FMEQ: case L::FMAND: case L::FMOR:
            return name + " " + vi(l.ft) + "," + vi(l.fs);
        case L::FCGET: case L::XTOP: case L::XITOP:
            return name + " " + vi(l.ft);
        case L::B: return name + " " + addr(branchTarget(in, pc));
        case L::BAL: return name + " " + vi(l.ft) + "," + addr(branchTarget(in, pc));
        case L::JR: case L::XGKICK: return name + " " + vi(l.fs);
        case L::JALR: return name + " " + vi(l.ft) + "," + vi(l.fs);
        case L::IBEQ: case L::IBNE:
            return name + " " + vi(l.ft) + "," + vi(l.fs) + "," + addr(branchTarget(in, pc));
        case L::IBLTZ: case L::IBGTZ: case L::IBLEZ: case L::IBGEZ:
            return name + " " + vi(l.fs) + "," + addr(branchTarget(in, pc));
        case L::IADD: case L::ISUB: case L::IAND: case L::IOR:
            return name + " " + vi(l.fd) + "," + vi(l.fs) + "," + vi(l.ft);
        case L::IADDI: return name + " " + vi(l.ft) + "," + vi(l.fs) + "," + std::to_string(l.imm);
        case L::MOVE: case L::MR32: return dotted + vf(l.ft, f) + "," + vf(l.fs, f);
        case L::LQI: return dotted + vf(l.ft, f) + ",(" + vi(l.fs) + "++)";
        case L::SQI: return dotted + vf(l.fs, f) + ",(" + vi(l.ft) + "++)";
        case L::LQD: return dotted + vf(l.ft, f) + ",(--" + vi(l.fs) + ")";
        case L::SQD: return dotted + vf(l.fs, f) + ",(--" + vi(l.ft) + ")";
        case L::DIV: case L::RSQRT: return name + " q," + vfc(l.fs, l.fsf) + "," + vfc(l.ft, l.ftf);
        case L::SQRT: return name + " q," + vfc(l.ft, l.ftf);
        case L::WAITQ: case L::WAITP: return name;
        case L::MTIR: return name + " " + vi(l.ft) + "," + vfc(l.fs, l.fsf);
        case L::MFIR: return dotted + vf(l.ft, f) + "," + vi(l.fs);
        case L::ILWR: case L::ISWR: return dotted + vi(l.ft) + ",(" + vi(l.fs) + ")" + f;
        case L::RNEXT: case L::RGET: return dotted + vf(l.ft, f) + ",r";
        case L::RINIT: case L::RXOR: return name + " r," + vfc(l.fs, l.fsf);
        case L::MFP: return dotted + vf(l.ft, f) + ",p";
        case L::ESADD: case L::ERSADD: case L::ELENG: case L::ERLENG: case L::EATANxy: case L::EATANxz:
        case L::ESUM:
            return name + " p," + vf(l.fs, "");
        case L::ESQRT: case L::ERSQRT: case L::ERCPR: case L::ESIN: case L::EATAN: case L::EEXP:
            return name + " p," + vfc(l.fs, l.fsf);
        default:
            return "*unknown*";
    }
}

std::string disassemble(const Instr& in, std::uint32_t pc) {
    return upperText(in) + " \t" + lowerText(in, pc);
}

}  // namespace anyps2::vu
