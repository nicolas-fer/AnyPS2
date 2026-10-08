#pragma once

// Semântica das instruções dos VUs, compartilhada por:
//  * modo macro do VU0 (COP2 executado pelo EE, ops.h),
//  * o interpretador de microcódigo (vu.cpp),
//  * os microprogramas recompilados em C++.
// Tudo é inline e sem estado global: com a instrução constante (código
// recompilado) o compilador elimina os switches.

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "anyps2/runtime/context.h"
#include "anyps2/runtime/ps2float.h"
#include "anyps2/vu/isa.h"

// Inline forçado: o código recompilado chama estas funções com a instrução
// constante e só assim o compilador reduz cada par à sua operação.
#if defined(_MSC_VER)
#define ANYPS2_VU_INLINE __forceinline
#define ANYPS2_VU_LAMBDA_INLINE  // [[msvc::forceinline]] em lambdas não é aceito por todas as versões
#elif defined(__GNUC__)
#define ANYPS2_VU_INLINE inline __attribute__((always_inline))
#define ANYPS2_VU_LAMBDA_INLINE __attribute__((always_inline))
#else
#define ANYPS2_VU_INLINE inline
#define ANYPS2_VU_LAMBDA_INLINE
#endif

namespace anyps2::rt::vucore {

// Registradores de controle (índices de vi[], os mesmos do CFC2/CTC2).
namespace reg {
inline constexpr unsigned Status = 16, Mac = 17, Clip = 18, R = 20, I = 21, Q = 22, P = 23, TPC = 26,
                          CMSAR0 = 27, FBRST = 28, VPUSTAT = 29, CMSAR1 = 31;
}

// Bits do status
namespace st {
inline constexpr std::uint32_t Z = 1u << 0, S = 1u << 1, U = 1u << 2, O = 1u << 3, I = 1u << 4, D = 1u << 5,
                               ZS = 1u << 6, SS = 1u << 7, US = 1u << 8, OS = 1u << 9, IS = 1u << 10,
                               DS = 1u << 11;
}

// Visão dos registradores de um VU.
struct Regs {
    Reg128* vf;         // 32 registradores de 128 bits (vf0 = 0,0,0,1)
    std::uint32_t* vi;  // 0..15 inteiros de 16 bits; 16.. controle (ver reg::)
    Reg128* acc;
};

inline bool hasComp(unsigned dest, unsigned c) { return (dest >> (3 - c)) & 1; }

// Resultado de uma instrução upper antes de ser escrito (no modo micro o
// upper e o lower do mesmo par leem os registradores antigos).
struct UpperResult {
    bool writes = false;
    bool toAcc = false;
    std::uint8_t reg = 0;
    std::uint8_t dest = 0;
    Reg128 value{};
    bool setsFlags = false;
    std::uint16_t mac = 0;
    bool setsClip = false;
    std::uint32_t clip = 0;
    std::uint8_t srcA = 0, srcB = 0;  // registradores lidos (para stalls)
    bool readsAcc = false;
};

// Status (bits ZSUO) derivado do MAC, mais os sticky.
inline std::uint32_t statusFromMac(std::uint32_t oldStatus, std::uint16_t mac) {
    std::uint32_t s = 0;
    if (mac & 0x000F) s |= st::Z;
    if (mac & 0x00F0) s |= st::S;
    if (mac & 0x0F00) s |= st::U;
    if (mac & 0xF000) s |= st::O;
    const std::uint32_t sticky = (oldStatus & 0xFC0u) | (s << 6);
    return (oldStatus & (st::I | st::D)) | s | sticky;
}

// Grava o componente c de um resultado e acumula o MAC.
inline void setComp(UpperResult& r, unsigned c, float host) {
    const ps2f::Out o = ps2f::out(host);
    r.value.uw[c] = o.bits;
    const unsigned bit = 3 - c;  // x → bit 3, w → bit 0
    if ((o.bits & 0x7FFFFFFFu) == 0) r.mac = static_cast<std::uint16_t>(r.mac | (1u << bit));
    if (o.bits & 0x80000000u) r.mac = static_cast<std::uint16_t>(r.mac | (1u << (bit + 4)));
    if (o.underflow) r.mac = static_cast<std::uint16_t>(r.mac | (1u << (bit + 8)));
    if (o.overflow) r.mac = static_cast<std::uint16_t>(r.mac | (1u << (bit + 12)));
}

inline float F(const Reg128& v, unsigned c) { return ps2f::in(v.uw[c]); }

// MAX/MINI comparam como inteiros com sinal-magnitude e não alteram os bits.
inline std::int32_t orderKey(std::uint32_t b) {
    return (b & 0x80000000u) ? -static_cast<std::int32_t>(b & 0x7FFFFFFFu) : static_cast<std::int32_t>(b);
}

inline std::uint32_t ftoi(std::uint32_t b, float scale) {
    const double v = static_cast<double>(ps2f::in(b)) * static_cast<double>(scale);
    if (v >= 2147483647.0) return 0x7FFFFFFFu;
    if (v <= -2147483648.0) return 0x80000000u;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(v));  // trunca
}

inline std::uint32_t itof(std::uint32_t b, float scale) {
    const float v = static_cast<float>(static_cast<std::int32_t>(b)) * scale;
    return ps2f::toBits(v);
}

// Calcula uma instrução upper. Q e I são os valores vistos pela instrução.
template <vu::U Op>
ANYPS2_VU_INLINE UpperResult computeUpperOp(const Regs& R, const vu::Upper& u, std::uint32_t qBits, std::uint32_t iBits) {
    using vu::U;
    UpperResult r;
    r.dest = u.dest;
    r.srcA = u.fs;
    r.srcB = u.ft;
    const Reg128& s = R.vf[u.fs];
    const Reg128& t = R.vf[u.ft];
    const Reg128& a = *R.acc;
    const float q = ps2f::in(qBits), ii = ps2f::in(iBits);
    const float tb = F(t, u.bc);

    // Operando "t" por componente conforme o tipo (vetor, broadcast, Q, I).
    [[maybe_unused]] auto opT = [&](unsigned c, int kind) ANYPS2_VU_LAMBDA_INLINE -> float {
        switch (kind) {
            case 0: return F(t, c);
            case 1: return tb;
            case 2: return q;
            default: return ii;
        }
    };
    enum Kind { Vec = 0, Bc = 1, Qk = 2, Ik = 3 };
    enum Fn { Add, Sub, Mul, Madd, Msub, Max, Min };
    [[maybe_unused]] auto arith = [&](Fn fn, Kind k, bool toAcc, unsigned dst) ANYPS2_VU_LAMBDA_INLINE {
        r.writes = true;
        r.toAcc = toAcc;
        r.reg = static_cast<std::uint8_t>(dst);
        r.setsFlags = fn != Max && fn != Min;
        r.readsAcc = fn == Madd || fn == Msub;
        if (k != Vec && k != Bc) r.srcB = 0;
        for (unsigned c = 0; c < 4; ++c) {
            if (!hasComp(u.dest, c)) continue;
            const float x = F(s, c), y = opT(c, k);
            switch (fn) {
                case Add: setComp(r, c, ps2f::add(x, y)); break;
                case Sub: setComp(r, c, ps2f::sub(x, y)); break;
                case Mul: setComp(r, c, ps2f::mul(x, y)); break;
                case Madd: {
                    const ps2f::Out p = ps2f::out(ps2f::mul(x, y));
                    setComp(r, c, ps2f::add(F(a, c), ps2f::in(p.bits)));
                    break;
                }
                case Msub: {
                    const ps2f::Out p = ps2f::out(ps2f::mul(x, y));
                    setComp(r, c, ps2f::sub(F(a, c), ps2f::in(p.bits)));
                    break;
                }
                case Max: case Min: {
                    const std::uint32_t xb = s.uw[c];
                    const std::uint32_t yb = k == Vec ? t.uw[c] : k == Bc ? t.uw[u.bc] : k == Qk ? qBits : iBits;
                    const bool xBig = orderKey(xb) > orderKey(yb);
                    r.value.uw[c] = (fn == Max) == xBig ? xb : yb;
                    break;
                }
            }
        }
    };
    if constexpr (Op == U::ADD) {
        arith(Add, Vec, false, u.fd);
    } else if constexpr (Op == U::SUB) {
        arith(Sub, Vec, false, u.fd);
    } else if constexpr (Op == U::MUL) {
        arith(Mul, Vec, false, u.fd);
    } else if constexpr (Op == U::MADD) {
        arith(Madd, Vec, false, u.fd);
    } else if constexpr (Op == U::MSUB) {
        arith(Msub, Vec, false, u.fd);
    } else if constexpr (Op == U::MAX) {
        arith(Max, Vec, false, u.fd);
    } else if constexpr (Op == U::MINI) {
        arith(Min, Vec, false, u.fd);
    } else if constexpr (Op == U::ADDbc) {
        arith(Add, Bc, false, u.fd);
    } else if constexpr (Op == U::SUBbc) {
        arith(Sub, Bc, false, u.fd);
    } else if constexpr (Op == U::MULbc) {
        arith(Mul, Bc, false, u.fd);
    } else if constexpr (Op == U::MADDbc) {
        arith(Madd, Bc, false, u.fd);
    } else if constexpr (Op == U::MSUBbc) {
        arith(Msub, Bc, false, u.fd);
    } else if constexpr (Op == U::MAXbc) {
        arith(Max, Bc, false, u.fd);
    } else if constexpr (Op == U::MINIbc) {
        arith(Min, Bc, false, u.fd);
    } else if constexpr (Op == U::ADDq) {
        arith(Add, Qk, false, u.fd);
    } else if constexpr (Op == U::SUBq) {
        arith(Sub, Qk, false, u.fd);
    } else if constexpr (Op == U::MULq) {
        arith(Mul, Qk, false, u.fd);
    } else if constexpr (Op == U::MADDq) {
        arith(Madd, Qk, false, u.fd);
    } else if constexpr (Op == U::MSUBq) {
        arith(Msub, Qk, false, u.fd);
    } else if constexpr (Op == U::ADDi) {
        arith(Add, Ik, false, u.fd);
    } else if constexpr (Op == U::SUBi) {
        arith(Sub, Ik, false, u.fd);
    } else if constexpr (Op == U::MULi) {
        arith(Mul, Ik, false, u.fd);
    } else if constexpr (Op == U::MADDi) {
        arith(Madd, Ik, false, u.fd);
    } else if constexpr (Op == U::MSUBi) {
        arith(Msub, Ik, false, u.fd);
    } else if constexpr (Op == U::MAXi) {
        arith(Max, Ik, false, u.fd);
    } else if constexpr (Op == U::MINIi) {
        arith(Min, Ik, false, u.fd);
    } else if constexpr (Op == U::ADDA) {
        arith(Add, Vec, true, 0);
    } else if constexpr (Op == U::SUBA) {
        arith(Sub, Vec, true, 0);
    } else if constexpr (Op == U::MULA) {
        arith(Mul, Vec, true, 0);
    } else if constexpr (Op == U::MADDA) {
        arith(Madd, Vec, true, 0);
    } else if constexpr (Op == U::MSUBA) {
        arith(Msub, Vec, true, 0);
    } else if constexpr (Op == U::ADDAbc) {
        arith(Add, Bc, true, 0);
    } else if constexpr (Op == U::SUBAbc) {
        arith(Sub, Bc, true, 0);
    } else if constexpr (Op == U::MULAbc) {
        arith(Mul, Bc, true, 0);
    } else if constexpr (Op == U::MADDAbc) {
        arith(Madd, Bc, true, 0);
    } else if constexpr (Op == U::MSUBAbc) {
        arith(Msub, Bc, true, 0);
    } else if constexpr (Op == U::ADDAq) {
        arith(Add, Qk, true, 0);
    } else if constexpr (Op == U::SUBAq) {
        arith(Sub, Qk, true, 0);
    } else if constexpr (Op == U::MULAq) {
        arith(Mul, Qk, true, 0);
    } else if constexpr (Op == U::MADDAq) {
        arith(Madd, Qk, true, 0);
    } else if constexpr (Op == U::MSUBAq) {
        arith(Msub, Qk, true, 0);
    } else if constexpr (Op == U::ADDAi) {
        arith(Add, Ik, true, 0);
    } else if constexpr (Op == U::SUBAi) {
        arith(Sub, Ik, true, 0);
    } else if constexpr (Op == U::MULAi) {
        arith(Mul, Ik, true, 0);
    } else if constexpr (Op == U::MADDAi) {
        arith(Madd, Ik, true, 0);
    } else if constexpr (Op == U::MSUBAi) {
        arith(Msub, Ik, true, 0);
    } else if constexpr (Op == U::OPMULA || Op == U::OPMSUB) {
        // Produto vetorial: ACC = fs.yzx * ft.zxy ; fd = ACC − fs.yzx * ft.zxy
        static constexpr unsigned kA[3] = {1, 2, 0}, kB[3] = {2, 0, 1};
        constexpr bool sub = Op == U::OPMSUB;
        r.writes = true;
        r.toAcc = !sub;
        r.reg = sub ? u.fd : 0;
        r.setsFlags = true;
        r.readsAcc = sub;
        for (unsigned c = 0; c < 3; ++c) {
            if (!hasComp(u.dest, c)) continue;
            const float p = ps2f::mul(F(s, kA[c]), F(t, kB[c]));
            if (sub) setComp(r, c, ps2f::sub(F(a, c), ps2f::in(ps2f::out(p).bits)));
            else setComp(r, c, p);
        }
    } else if constexpr (Op == U::ABS) {
        r.writes = true;
        r.reg = u.ft;
        r.srcB = 0;
        for (unsigned c = 0; c < 4; ++c) {
            if (hasComp(u.dest, c)) r.value.uw[c] = s.uw[c] & 0x7FFFFFFFu;
        }
    } else if constexpr (Op == U::ITOF0 || Op == U::ITOF4 || Op == U::ITOF12 || Op == U::ITOF15 || Op == U::FTOI0 || Op == U::FTOI4 || Op == U::FTOI12 || Op == U::FTOI15) {
        static constexpr float kScale[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
        constexpr bool toInt = Op >= U::FTOI0;
        constexpr unsigned n = static_cast<unsigned>(Op) - static_cast<unsigned>(toInt ? U::FTOI0 : U::ITOF0);
        r.writes = true;
        r.reg = u.ft;
        r.srcB = 0;
        for (unsigned c = 0; c < 4; ++c) {
            if (!hasComp(u.dest, c)) continue;
            r.value.uw[c] = toInt ? ftoi(s.uw[c], kScale[n]) : itof(s.uw[c], 1.0f / kScale[n]);
        }
    } else if constexpr (Op == U::CLIP) {
        const float w = std::fabs(F(t, 3));
        std::uint32_t j = 0;
        const float x = F(s, 0), y = F(s, 1), z = F(s, 2);
        if (x > w) j |= 0x01;
        if (x < -w) j |= 0x02;
        if (y > w) j |= 0x04;
        if (y < -w) j |= 0x08;
        if (z > w) j |= 0x10;
        if (z < -w) j |= 0x20;
        r.setsClip = true;
        r.clip = j;
    } else {
        r.srcA = r.srcB = 0;
    }
    return r;
}

// Mesma operação com o código escolhido em tempo de execução (interpretador,
// modo macro).
inline UpperResult computeUpper(const Regs& R, const vu::Upper& u, std::uint32_t qBits, std::uint32_t iBits) {
    switch (u.op) {
        case vu::U::ADD: return computeUpperOp<vu::U::ADD>(R, u, qBits, iBits);
        case vu::U::SUB: return computeUpperOp<vu::U::SUB>(R, u, qBits, iBits);
        case vu::U::MADD: return computeUpperOp<vu::U::MADD>(R, u, qBits, iBits);
        case vu::U::MSUB: return computeUpperOp<vu::U::MSUB>(R, u, qBits, iBits);
        case vu::U::MAX: return computeUpperOp<vu::U::MAX>(R, u, qBits, iBits);
        case vu::U::MINI: return computeUpperOp<vu::U::MINI>(R, u, qBits, iBits);
        case vu::U::MUL: return computeUpperOp<vu::U::MUL>(R, u, qBits, iBits);
        case vu::U::OPMSUB: return computeUpperOp<vu::U::OPMSUB>(R, u, qBits, iBits);
        case vu::U::ADDbc: return computeUpperOp<vu::U::ADDbc>(R, u, qBits, iBits);
        case vu::U::SUBbc: return computeUpperOp<vu::U::SUBbc>(R, u, qBits, iBits);
        case vu::U::MADDbc: return computeUpperOp<vu::U::MADDbc>(R, u, qBits, iBits);
        case vu::U::MSUBbc: return computeUpperOp<vu::U::MSUBbc>(R, u, qBits, iBits);
        case vu::U::MAXbc: return computeUpperOp<vu::U::MAXbc>(R, u, qBits, iBits);
        case vu::U::MINIbc: return computeUpperOp<vu::U::MINIbc>(R, u, qBits, iBits);
        case vu::U::MULbc: return computeUpperOp<vu::U::MULbc>(R, u, qBits, iBits);
        case vu::U::ADDq: return computeUpperOp<vu::U::ADDq>(R, u, qBits, iBits);
        case vu::U::SUBq: return computeUpperOp<vu::U::SUBq>(R, u, qBits, iBits);
        case vu::U::MADDq: return computeUpperOp<vu::U::MADDq>(R, u, qBits, iBits);
        case vu::U::MSUBq: return computeUpperOp<vu::U::MSUBq>(R, u, qBits, iBits);
        case vu::U::MULq: return computeUpperOp<vu::U::MULq>(R, u, qBits, iBits);
        case vu::U::ADDi: return computeUpperOp<vu::U::ADDi>(R, u, qBits, iBits);
        case vu::U::SUBi: return computeUpperOp<vu::U::SUBi>(R, u, qBits, iBits);
        case vu::U::MADDi: return computeUpperOp<vu::U::MADDi>(R, u, qBits, iBits);
        case vu::U::MSUBi: return computeUpperOp<vu::U::MSUBi>(R, u, qBits, iBits);
        case vu::U::MAXi: return computeUpperOp<vu::U::MAXi>(R, u, qBits, iBits);
        case vu::U::MINIi: return computeUpperOp<vu::U::MINIi>(R, u, qBits, iBits);
        case vu::U::MULi: return computeUpperOp<vu::U::MULi>(R, u, qBits, iBits);
        case vu::U::ADDA: return computeUpperOp<vu::U::ADDA>(R, u, qBits, iBits);
        case vu::U::SUBA: return computeUpperOp<vu::U::SUBA>(R, u, qBits, iBits);
        case vu::U::MADDA: return computeUpperOp<vu::U::MADDA>(R, u, qBits, iBits);
        case vu::U::MSUBA: return computeUpperOp<vu::U::MSUBA>(R, u, qBits, iBits);
        case vu::U::MULA: return computeUpperOp<vu::U::MULA>(R, u, qBits, iBits);
        case vu::U::OPMULA: return computeUpperOp<vu::U::OPMULA>(R, u, qBits, iBits);
        case vu::U::ADDAbc: return computeUpperOp<vu::U::ADDAbc>(R, u, qBits, iBits);
        case vu::U::SUBAbc: return computeUpperOp<vu::U::SUBAbc>(R, u, qBits, iBits);
        case vu::U::MADDAbc: return computeUpperOp<vu::U::MADDAbc>(R, u, qBits, iBits);
        case vu::U::MSUBAbc: return computeUpperOp<vu::U::MSUBAbc>(R, u, qBits, iBits);
        case vu::U::MULAbc: return computeUpperOp<vu::U::MULAbc>(R, u, qBits, iBits);
        case vu::U::ADDAq: return computeUpperOp<vu::U::ADDAq>(R, u, qBits, iBits);
        case vu::U::SUBAq: return computeUpperOp<vu::U::SUBAq>(R, u, qBits, iBits);
        case vu::U::MADDAq: return computeUpperOp<vu::U::MADDAq>(R, u, qBits, iBits);
        case vu::U::MSUBAq: return computeUpperOp<vu::U::MSUBAq>(R, u, qBits, iBits);
        case vu::U::MULAq: return computeUpperOp<vu::U::MULAq>(R, u, qBits, iBits);
        case vu::U::ADDAi: return computeUpperOp<vu::U::ADDAi>(R, u, qBits, iBits);
        case vu::U::SUBAi: return computeUpperOp<vu::U::SUBAi>(R, u, qBits, iBits);
        case vu::U::MADDAi: return computeUpperOp<vu::U::MADDAi>(R, u, qBits, iBits);
        case vu::U::MSUBAi: return computeUpperOp<vu::U::MSUBAi>(R, u, qBits, iBits);
        case vu::U::MULAi: return computeUpperOp<vu::U::MULAi>(R, u, qBits, iBits);
        case vu::U::ABS: return computeUpperOp<vu::U::ABS>(R, u, qBits, iBits);
        case vu::U::ITOF0: return computeUpperOp<vu::U::ITOF0>(R, u, qBits, iBits);
        case vu::U::ITOF4: return computeUpperOp<vu::U::ITOF4>(R, u, qBits, iBits);
        case vu::U::ITOF12: return computeUpperOp<vu::U::ITOF12>(R, u, qBits, iBits);
        case vu::U::ITOF15: return computeUpperOp<vu::U::ITOF15>(R, u, qBits, iBits);
        case vu::U::FTOI0: return computeUpperOp<vu::U::FTOI0>(R, u, qBits, iBits);
        case vu::U::FTOI4: return computeUpperOp<vu::U::FTOI4>(R, u, qBits, iBits);
        case vu::U::FTOI12: return computeUpperOp<vu::U::FTOI12>(R, u, qBits, iBits);
        case vu::U::FTOI15: return computeUpperOp<vu::U::FTOI15>(R, u, qBits, iBits);
        case vu::U::CLIP: return computeUpperOp<vu::U::CLIP>(R, u, qBits, iBits);
        case vu::U::NOP: return computeUpperOp<vu::U::NOP>(R, u, qBits, iBits);
        case vu::U::INVALID: return computeUpperOp<vu::U::INVALID>(R, u, qBits, iBits);
        default: return computeUpperOp<vu::U::INVALID>(R, u, qBits, iBits);
    }
}

// Escreve o resultado nos registradores (vf0 é constante).
inline void writeUpper(const Regs& R, const UpperResult& r) {
    if (!r.writes) return;
    Reg128& dst = r.toAcc ? *R.acc : R.vf[r.reg];
    if (!r.toAcc && r.reg == 0) return;
    for (unsigned c = 0; c < 4; ++c) {
        if (hasComp(r.dest, c)) dst.uw[c] = r.value.uw[c];
    }
}

// Flags do upper aplicadas na hora (modo macro).
inline void flagsNow(const Regs& R, const UpperResult& r) {
    if (r.setsFlags) {
        R.vi[reg::Mac] = r.mac;
        R.vi[reg::Status] = statusFromMac(R.vi[reg::Status], r.mac);
    }
    if (r.setsClip) R.vi[reg::Clip] = ((R.vi[reg::Clip] << 6) | r.clip) & 0xFFFFFFu;
}

// ---- FDIV ------------------------------------------------------------------

struct DivResult {
    std::uint32_t q;
    std::uint32_t flags;  // st::I / st::D
};

inline DivResult fdiv(vu::L op, std::uint32_t fsBits, std::uint32_t ftBits) {
    const float s = ps2f::in(fsBits), t = ps2f::in(ftBits);
    const std::uint32_t sign = (fsBits ^ ftBits) & 0x80000000u;
    switch (op) {
        case vu::L::DIV:
            if (t == 0.0f) {
                if (s == 0.0f) return {sign | ps2f::kFmax, st::I};
                return {sign | ps2f::kFmax, st::D};
            }
            return {ps2f::out(ps2f::div(s, t)).bits, 0};
        case vu::L::SQRT: {
            const std::uint32_t f = t < 0.0f ? st::I : 0;
            return {ps2f::out(ps2f::sqrt(std::fabs(t))).bits & 0x7FFFFFFFu, f};
        }
        default: {  // RSQRT
            std::uint32_t f = t < 0.0f ? st::I : 0;
            const float root = ps2f::sqrt(std::fabs(t));
            if (root == 0.0f) return {(fsBits & 0x80000000u) | ps2f::kFmax, f | (s == 0.0f ? st::I : st::D)};
            return {ps2f::out(ps2f::div(s, root)).bits, f};
        }
    }
}

// Aplica as flags D/I do FDIV ao status (com sticky).
inline std::uint32_t statusWithDiv(std::uint32_t status, std::uint32_t flags) {
    status &= ~(st::I | st::D);
    status |= flags;
    if (flags & st::I) status |= st::IS;
    if (flags & st::D) status |= st::DS;
    return status;
}

// ---- Registrador R (LFSR) ----------------------------------------------------

inline std::uint32_t advanceR(std::uint32_t r) {
    const std::uint32_t x = (r >> 4) & 1, y = (r >> 22) & 1;
    r = (r << 1) ^ x ^ y;
    return (r & 0x007FFFFFu) | 0x3F800000u;
}

}  // namespace anyps2::rt::vucore
