#pragma once

#include <cstdint>
#include <string>

// Conjunto de instruções dos Vector Units (VU0/VU1) em modo micro.
//
// Cada instrução tem 64 bits: a palavra baixa é o "lower" (inteiros, load/
// store, desvios, FDIV, EFU...) e a alta é o "upper" (FMAC), que carrega
// também os bits I/E/M/D/T. Com o bit I, a palavra lower é um float que vai
// para o registrador I (LOI).
namespace anyps2::vu {

enum class U : std::uint8_t {
    // fd = fs op ft
    ADD, SUB, MADD, MSUB, MAX, MINI, MUL, OPMSUB,
    // fd = fs op ft.bc
    ADDbc, SUBbc, MADDbc, MSUBbc, MAXbc, MINIbc, MULbc,
    // fd = fs op Q / I
    ADDq, SUBq, MADDq, MSUBq, MULq,
    ADDi, SUBi, MADDi, MSUBi, MAXi, MINIi, MULi,
    // ACC = fs op ...
    ADDA, SUBA, MADDA, MSUBA, MULA, OPMULA,
    ADDAbc, SUBAbc, MADDAbc, MSUBAbc, MULAbc,
    ADDAq, SUBAq, MADDAq, MSUBAq, MULAq,
    ADDAi, SUBAi, MADDAi, MSUBAi, MULAi,
    ABS, ITOF0, ITOF4, ITOF12, ITOF15, FTOI0, FTOI4, FTOI12, FTOI15, CLIP, NOP,
    INVALID,
    COUNT_
};

enum class L : std::uint8_t {
    LQ, SQ, ILW, ISW, IADDIU, ISUBIU,
    FCEQ, FCSET, FCAND, FCOR, FSEQ, FSSET, FSAND, FSOR, FMEQ, FMAND, FMOR, FCGET,
    B, BAL, JR, JALR, IBEQ, IBNE, IBLTZ, IBGTZ, IBLEZ, IBGEZ,
    IADD, ISUB, IADDI, IAND, IOR,
    MOVE, MR32, LQI, SQI, LQD, SQD, DIV, SQRT, RSQRT, WAITQ, MTIR, MFIR, ILWR, ISWR,
    RNEXT, RGET, RINIT, RXOR, MFP, XTOP, XITOP, XGKICK,
    ESADD, ERSADD, ELENG, ERLENG, EATANxy, EATANxz, ESUM, ESQRT, ERSQRT, ERCPR, WAITP, ESIN, EATAN, EEXP,
    NOP,  // move com todos os campos zerados (0x8000033C)
    LOI,  // palavra lower com o bit I: float para o registrador I
    INVALID,
    COUNT_
};

const char* upperName(U op);
const char* lowerName(L op);

struct Upper {
    U op = U::INVALID;
    std::uint8_t dest = 0;  // máscara xyzw: bit 3 = x, bit 0 = w
    std::uint8_t ft = 0, fs = 0, fd = 0;
    std::uint8_t bc = 0;     // 0..3 = x..w
    bool canonical = true;   // campos não usados zerados
};

struct Lower {
    L op = L::INVALID;
    std::uint8_t dest = 0;
    std::uint8_t ft = 0, fs = 0, fd = 0;  // também it/is/id
    std::uint8_t fsf = 0, ftf = 0;        // componentes (0..3 = x..w)
    std::int32_t imm = 0;                 // imediato já estendido/montado
    bool canonical = true;
};

struct Instr {
    std::uint32_t lowerWord = 0;
    std::uint32_t upperWord = 0;
    Upper upper;
    Lower lower;
    bool i = false, e = false, m = false, d = false, t = false;
};

Instr decode(std::uint32_t lowerWord, std::uint32_t upperWord);

// Propriedades do lower usadas pelo interpretador e pelo recompilador.
bool isBranch(L op);              // B, BAL, IBxx, JR, JALR (todos com delay slot)
bool isConditionalBranch(L op);   // IBxx
bool isIndirect(L op);            // JR, JALR
bool isLink(L op);                // BAL, JALR
// Alvo de B/BAL/IBxx em bytes (pc = endereço do par, em bytes).
std::uint32_t branchTarget(const Instr& in, std::uint32_t pc);

// Texto no formato do dvp-objdump (-m dvp:vu).
std::string upperText(const Instr& in);
std::string lowerText(const Instr& in, std::uint32_t pc);
std::string disassemble(const Instr& in, std::uint32_t pc);  // "upper <TAB> lower"

}  // namespace anyps2::vu
