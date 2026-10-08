#pragma once

// Semântica das instruções do EE (R5900), usada pelo código gerado.
//
// Cada instrução vira uma chamada a uma função inline com o MESMO nome do
// enumerador em r5900::Op (ADDIU, PEXTLW, ADD_S...). Os argumentos são os
// campos já decodificados, todos constantes em tempo de compilação, então o
// compilador reduz cada chamada a poucas instruções do host.
//
// Convenções:
//  * Escritas em $zero são descartadas (if (rd) ...), o que o compilador
//    elimina quando rd é constante.
//  * Instruções não-MMI alteram só os 64 bits baixos do GPR; os 64 altos
//    são preservados, como no R5900.
//  * Valores de 32 bits gravados em GPR são estendidos com sinal para 64.
//  * Sem UB de inteiros: aritmética feita em unsigned e convertida.
//
// Fontes da semântica: EE Core Instruction Set Manual; casos de borda (divisão
// por zero, saturações, FPU sem IEEE) conforme o comportamento validado em
// hardware documentado pelo projeto PCSX2.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "anyps2/runtime/context.h"
#include "anyps2/runtime/ps2float.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/memory.h"

namespace anyps2::rt::ops {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// ---------------------------------------------------------------------------
// Utilitários
// ---------------------------------------------------------------------------

inline s64 sx32(u32 v) { return static_cast<s32>(v); }
inline void set64(Context* c, unsigned r, u64 v) {
    if (r) c->r[r].ud[0] = v;
}
inline void set32(Context* c, unsigned r, u32 v) {
    if (r) c->r[r].sd[0] = static_cast<s32>(v);
}
inline void set128(Context* c, unsigned r, const Reg128& v) {
    if (r) c->r[r] = v;
}
inline u64 G(Context* c, unsigned r) { return c->r[r].ud[0]; }
inline u32 G32(Context* c, unsigned r) { return c->r[r].uw[0]; }
inline u32 addr(Context* c, unsigned base, s32 off) { return c->r[base].uw[0] + static_cast<u32>(off); }

[[noreturn]] void overflowException(u32 pc);
[[noreturn]] void trapException(u32 pc);
[[noreturn]] void breakException(Context* c, u32 pc);
[[noreturn]] void unsupported(Context* c, u32 pc, const char* text);
[[noreturn]] void unimplementedCop0(Context* c, u32 pc, const char* what);

// ---------------------------------------------------------------------------
// Aritmética e lógica com imediato
// ---------------------------------------------------------------------------

inline void ADDI(Context* c, unsigned rt, unsigned rs, s32 imm, u32 pc) {
    const s64 r = sx32(G32(c, rs)) + imm;
    if (r != static_cast<s32>(r)) overflowException(pc);
    set32(c, rt, static_cast<u32>(r));
}
inline void ADDIU(Context* c, unsigned rt, unsigned rs, s32 imm) {
    set32(c, rt, G32(c, rs) + static_cast<u32>(imm));
}
inline void DADDI(Context* c, unsigned rt, unsigned rs, s32 imm, u32 pc) {
    const u64 a = G(c, rs), b = static_cast<u64>(static_cast<s64>(imm)), r = a + b;
    if (((a ^ r) & (b ^ r)) >> 63) overflowException(pc);
    set64(c, rt, r);
}
inline void DADDIU(Context* c, unsigned rt, unsigned rs, s32 imm) {
    set64(c, rt, G(c, rs) + static_cast<u64>(static_cast<s64>(imm)));
}
inline void SLTI(Context* c, unsigned rt, unsigned rs, s32 imm) {
    set64(c, rt, c->r[rs].sd[0] < imm ? 1 : 0);
}
inline void SLTIU(Context* c, unsigned rt, unsigned rs, s32 imm) {
    set64(c, rt, G(c, rs) < static_cast<u64>(static_cast<s64>(imm)) ? 1 : 0);
}
inline void ANDI(Context* c, unsigned rt, unsigned rs, u32 imm) { set64(c, rt, G(c, rs) & imm); }
inline void ORI(Context* c, unsigned rt, unsigned rs, u32 imm) { set64(c, rt, G(c, rs) | imm); }
inline void XORI(Context* c, unsigned rt, unsigned rs, u32 imm) { set64(c, rt, G(c, rs) ^ imm); }
inline void LUI(Context* c, unsigned rt, u32 imm) { set32(c, rt, imm << 16); }

// ---------------------------------------------------------------------------
// SPECIAL: aritmética de registradores
// ---------------------------------------------------------------------------

inline void ADD(Context* c, unsigned rd, unsigned rs, unsigned rt, u32 pc) {
    const s64 r = sx32(G32(c, rs)) + sx32(G32(c, rt));
    if (r != static_cast<s32>(r)) overflowException(pc);
    set32(c, rd, static_cast<u32>(r));
}
inline void ADDU(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    set32(c, rd, G32(c, rs) + G32(c, rt));
}
inline void SUB(Context* c, unsigned rd, unsigned rs, unsigned rt, u32 pc) {
    const s64 r = sx32(G32(c, rs)) - sx32(G32(c, rt));
    if (r != static_cast<s32>(r)) overflowException(pc);
    set32(c, rd, static_cast<u32>(r));
}
inline void SUBU(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    set32(c, rd, G32(c, rs) - G32(c, rt));
}
inline void DADD(Context* c, unsigned rd, unsigned rs, unsigned rt, u32 pc) {
    const u64 a = G(c, rs), b = G(c, rt), r = a + b;
    if (((a ^ r) & (b ^ r)) >> 63) overflowException(pc);
    set64(c, rd, r);
}
inline void DADDU(Context* c, unsigned rd, unsigned rs, unsigned rt) { set64(c, rd, G(c, rs) + G(c, rt)); }
inline void DSUB(Context* c, unsigned rd, unsigned rs, unsigned rt, u32 pc) {
    const u64 a = G(c, rs), b = G(c, rt), r = a - b;
    if (((a ^ b) & (a ^ r)) >> 63) overflowException(pc);
    set64(c, rd, r);
}
inline void DSUBU(Context* c, unsigned rd, unsigned rs, unsigned rt) { set64(c, rd, G(c, rs) - G(c, rt)); }
inline void AND(Context* c, unsigned rd, unsigned rs, unsigned rt) { set64(c, rd, G(c, rs) & G(c, rt)); }
inline void OR(Context* c, unsigned rd, unsigned rs, unsigned rt) { set64(c, rd, G(c, rs) | G(c, rt)); }
inline void XOR(Context* c, unsigned rd, unsigned rs, unsigned rt) { set64(c, rd, G(c, rs) ^ G(c, rt)); }
inline void NOR(Context* c, unsigned rd, unsigned rs, unsigned rt) { set64(c, rd, ~(G(c, rs) | G(c, rt))); }
inline void SLT(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    set64(c, rd, c->r[rs].sd[0] < c->r[rt].sd[0] ? 1 : 0);
}
inline void SLTU(Context* c, unsigned rd, unsigned rs, unsigned rt) { set64(c, rd, G(c, rs) < G(c, rt) ? 1 : 0); }
inline void MOVZ(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    if (G(c, rt) == 0) set64(c, rd, G(c, rs));
}
inline void MOVN(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    if (G(c, rt) != 0) set64(c, rd, G(c, rs));
}

// Shifts
inline void SLL(Context* c, unsigned rd, unsigned rt, unsigned sa) { set32(c, rd, G32(c, rt) << sa); }
inline void SRL(Context* c, unsigned rd, unsigned rt, unsigned sa) { set32(c, rd, G32(c, rt) >> sa); }
inline void SRA(Context* c, unsigned rd, unsigned rt, unsigned sa) {
    set32(c, rd, static_cast<u32>(c->r[rt].sw[0] >> sa));
}
inline void SLLV(Context* c, unsigned rd, unsigned rt, unsigned rs) { set32(c, rd, G32(c, rt) << (G32(c, rs) & 31)); }
inline void SRLV(Context* c, unsigned rd, unsigned rt, unsigned rs) { set32(c, rd, G32(c, rt) >> (G32(c, rs) & 31)); }
inline void SRAV(Context* c, unsigned rd, unsigned rt, unsigned rs) {
    set32(c, rd, static_cast<u32>(c->r[rt].sw[0] >> (G32(c, rs) & 31)));
}
inline void DSLL(Context* c, unsigned rd, unsigned rt, unsigned sa) { set64(c, rd, G(c, rt) << sa); }
inline void DSRL(Context* c, unsigned rd, unsigned rt, unsigned sa) { set64(c, rd, G(c, rt) >> sa); }
inline void DSRA(Context* c, unsigned rd, unsigned rt, unsigned sa) {
    set64(c, rd, static_cast<u64>(c->r[rt].sd[0] >> sa));
}
inline void DSLL32(Context* c, unsigned rd, unsigned rt, unsigned sa) { set64(c, rd, G(c, rt) << (sa + 32)); }
inline void DSRL32(Context* c, unsigned rd, unsigned rt, unsigned sa) { set64(c, rd, G(c, rt) >> (sa + 32)); }
inline void DSRA32(Context* c, unsigned rd, unsigned rt, unsigned sa) {
    set64(c, rd, static_cast<u64>(c->r[rt].sd[0] >> (sa + 32)));
}
inline void DSLLV(Context* c, unsigned rd, unsigned rt, unsigned rs) { set64(c, rd, G(c, rt) << (G(c, rs) & 63)); }
inline void DSRLV(Context* c, unsigned rd, unsigned rt, unsigned rs) { set64(c, rd, G(c, rt) >> (G(c, rs) & 63)); }
inline void DSRAV(Context* c, unsigned rd, unsigned rt, unsigned rs) {
    set64(c, rd, static_cast<u64>(c->r[rt].sd[0] >> (G(c, rs) & 63)));
}

// ---------------------------------------------------------------------------
// HI/LO, multiplicação e divisão (pipelines 0 e 1)
// ---------------------------------------------------------------------------

// lane 0 = HI/LO, lane 1 = HI1/LO1
inline void setHiLo(Context* c, unsigned lane, u64 product, unsigned rd) {
    c->lo.sd[lane] = sx32(static_cast<u32>(product));
    c->hi.sd[lane] = sx32(static_cast<u32>(product >> 32));
    set64(c, rd, c->lo.ud[lane]);
}
inline void multS(Context* c, unsigned lane, unsigned rd, unsigned rs, unsigned rt) {
    const s64 p = sx32(G32(c, rs)) * sx32(G32(c, rt));
    setHiLo(c, lane, static_cast<u64>(p), rd);
}
inline void multU(Context* c, unsigned lane, unsigned rd, unsigned rs, unsigned rt) {
    const u64 p = u64{G32(c, rs)} * u64{G32(c, rt)};
    setHiLo(c, lane, p, rd);
}
inline u64 hiLo64(Context* c, unsigned lane) {
    return (u64{c->hi.uw[lane * 2]} << 32) | c->lo.uw[lane * 2];
}
inline void maddS(Context* c, unsigned lane, unsigned rd, unsigned rs, unsigned rt) {
    const s64 p = sx32(G32(c, rs)) * sx32(G32(c, rt));
    setHiLo(c, lane, hiLo64(c, lane) + static_cast<u64>(p), rd);
}
inline void maddU(Context* c, unsigned lane, unsigned rd, unsigned rs, unsigned rt) {
    const u64 p = u64{G32(c, rs)} * u64{G32(c, rt)};
    setHiLo(c, lane, hiLo64(c, lane) + p, rd);
}
inline void divS(Context* c, unsigned lane, unsigned rs, unsigned rt) {
    const s32 n = c->r[rs].sw[0], d = c->r[rt].sw[0];
    if (n == std::numeric_limits<s32>::min() && d == -1) {
        c->lo.sd[lane] = n;
        c->hi.sd[lane] = 0;
    } else if (d != 0) {
        c->lo.sd[lane] = n / d;
        c->hi.sd[lane] = n % d;
    } else {
        c->lo.sd[lane] = n < 0 ? 1 : -1;
        c->hi.sd[lane] = n;
    }
}
inline void divU(Context* c, unsigned lane, unsigned rs, unsigned rt) {
    const u32 n = G32(c, rs), d = G32(c, rt);
    if (d != 0) {
        c->lo.sd[lane] = sx32(n / d);
        c->hi.sd[lane] = sx32(n % d);
    } else {
        c->lo.sd[lane] = -1;
        c->hi.sd[lane] = sx32(n);
    }
}

inline void MULT(Context* c, unsigned rd, unsigned rs, unsigned rt) { multS(c, 0, rd, rs, rt); }
inline void MULTU(Context* c, unsigned rd, unsigned rs, unsigned rt) { multU(c, 0, rd, rs, rt); }
inline void MULT1(Context* c, unsigned rd, unsigned rs, unsigned rt) { multS(c, 1, rd, rs, rt); }
inline void MULTU1(Context* c, unsigned rd, unsigned rs, unsigned rt) { multU(c, 1, rd, rs, rt); }
inline void MADD(Context* c, unsigned rd, unsigned rs, unsigned rt) { maddS(c, 0, rd, rs, rt); }
inline void MADDU(Context* c, unsigned rd, unsigned rs, unsigned rt) { maddU(c, 0, rd, rs, rt); }
inline void MADD1(Context* c, unsigned rd, unsigned rs, unsigned rt) { maddS(c, 1, rd, rs, rt); }
inline void MADDU1(Context* c, unsigned rd, unsigned rs, unsigned rt) { maddU(c, 1, rd, rs, rt); }
inline void DIV(Context* c, unsigned rs, unsigned rt) { divS(c, 0, rs, rt); }
inline void DIVU(Context* c, unsigned rs, unsigned rt) { divU(c, 0, rs, rt); }
inline void DIV1(Context* c, unsigned rs, unsigned rt) { divS(c, 1, rs, rt); }
inline void DIVU1(Context* c, unsigned rs, unsigned rt) { divU(c, 1, rs, rt); }

inline void MFHI(Context* c, unsigned rd) { set64(c, rd, c->hi.ud[0]); }
inline void MFLO(Context* c, unsigned rd) { set64(c, rd, c->lo.ud[0]); }
inline void MTHI(Context* c, unsigned rs) { c->hi.ud[0] = G(c, rs); }
inline void MTLO(Context* c, unsigned rs) { c->lo.ud[0] = G(c, rs); }
inline void MFHI1(Context* c, unsigned rd) { set64(c, rd, c->hi.ud[1]); }
inline void MFLO1(Context* c, unsigned rd) { set64(c, rd, c->lo.ud[1]); }
inline void MTHI1(Context* c, unsigned rs) { c->hi.ud[1] = G(c, rs); }
inline void MTLO1(Context* c, unsigned rs) { c->lo.ud[1] = G(c, rs); }

// SA (QFSRV). Guardado em bytes.
inline void MFSA(Context* c, unsigned rd) { set64(c, rd, c->sa); }
inline void MTSA(Context* c, unsigned rs) { c->sa = G32(c, rs); }
inline void MTSAB(Context* c, unsigned rs, s32 imm) {
    c->sa = (G32(c, rs) & 0xF) ^ (static_cast<u32>(imm) & 0xF);
}
inline void MTSAH(Context* c, unsigned rs, s32 imm) {
    c->sa = ((G32(c, rs) & 0x7) ^ (static_cast<u32>(imm) & 0x7)) << 1;
}

// ---------------------------------------------------------------------------
// Traps e exceções
// ---------------------------------------------------------------------------

inline void TGE(Context* c, unsigned rs, unsigned rt, u32 pc) { if (c->r[rs].sd[0] >= c->r[rt].sd[0]) trapException(pc); }
inline void TGEU(Context* c, unsigned rs, unsigned rt, u32 pc) { if (G(c, rs) >= G(c, rt)) trapException(pc); }
inline void TLT(Context* c, unsigned rs, unsigned rt, u32 pc) { if (c->r[rs].sd[0] < c->r[rt].sd[0]) trapException(pc); }
inline void TLTU(Context* c, unsigned rs, unsigned rt, u32 pc) { if (G(c, rs) < G(c, rt)) trapException(pc); }
inline void TEQ(Context* c, unsigned rs, unsigned rt, u32 pc) { if (G(c, rs) == G(c, rt)) trapException(pc); }
inline void TNE(Context* c, unsigned rs, unsigned rt, u32 pc) { if (G(c, rs) != G(c, rt)) trapException(pc); }
inline void TGEI(Context* c, unsigned rs, s32 imm, u32 pc) { if (c->r[rs].sd[0] >= imm) trapException(pc); }
inline void TGEIU(Context* c, unsigned rs, s32 imm, u32 pc) { if (G(c, rs) >= static_cast<u64>(static_cast<s64>(imm))) trapException(pc); }
inline void TLTI(Context* c, unsigned rs, s32 imm, u32 pc) { if (c->r[rs].sd[0] < imm) trapException(pc); }
inline void TLTIU(Context* c, unsigned rs, s32 imm, u32 pc) { if (G(c, rs) < static_cast<u64>(static_cast<s64>(imm))) trapException(pc); }
inline void TEQI(Context* c, unsigned rs, s32 imm, u32 pc) { if (c->r[rs].sd[0] == imm) trapException(pc); }
inline void TNEI(Context* c, unsigned rs, s32 imm, u32 pc) { if (c->r[rs].sd[0] != imm) trapException(pc); }
inline void BREAK(Context* c, u32 pc) { breakException(c, pc); }
inline void SYNC(Context*) {}

// ---------------------------------------------------------------------------
// Loads e stores
// ---------------------------------------------------------------------------

inline void LB(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    const s8 v = static_cast<s8>(c->mem->read<u8>(addr(c, base, off), pc));
    set64(c, rt, static_cast<u64>(static_cast<s64>(v)));
}
inline void LBU(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    set64(c, rt, c->mem->read<u8>(addr(c, base, off), pc));
}
inline void LH(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    const s16 v = static_cast<s16>(c->mem->read<u16>(addr(c, base, off), pc));
    set64(c, rt, static_cast<u64>(static_cast<s64>(v)));
}
inline void LHU(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    set64(c, rt, c->mem->read<u16>(addr(c, base, off), pc));
}
inline void LW(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    set32(c, rt, c->mem->read<u32>(addr(c, base, off), pc));
}
inline void LWU(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    set64(c, rt, c->mem->read<u32>(addr(c, base, off), pc));
}
inline void LD(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    set64(c, rt, c->mem->read<u64>(addr(c, base, off), pc));
}
inline void LQ(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    Reg128 v;
    c->mem->read128(addr(c, base, off) & ~15u, v, pc);  // LQ ignora os 4 bits baixos
    set128(c, rt, v);
}
inline void SB(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    c->mem->write<u8>(addr(c, base, off), static_cast<u8>(G(c, rt)), pc);
}
inline void SH(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    c->mem->write<u16>(addr(c, base, off), static_cast<u16>(G(c, rt)), pc);
}
inline void SW(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    c->mem->write<u32>(addr(c, base, off), G32(c, rt), pc);
}
inline void SD(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    c->mem->write<u64>(addr(c, base, off), G(c, rt), pc);
}
inline void SQ(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    c->mem->write128(addr(c, base, off) & ~15u, c->r[rt], pc);
}

// Acessos parciais (little-endian).
inline void LWL(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    static constexpr u32 kMask[4] = {0x00FFFFFFu, 0x0000FFFFu, 0x000000FFu, 0x00000000u};
    static constexpr unsigned kShift[4] = {24, 16, 8, 0};
    const u32 a = addr(c, base, off), s = a & 3;
    const u32 mem = c->mem->read<u32>(a & ~3u, pc);
    set32(c, rt, (G32(c, rt) & kMask[s]) | (mem << kShift[s]));
}
inline void LWR(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    static constexpr u32 kMask[4] = {0x00000000u, 0xFF000000u, 0xFFFF0000u, 0xFFFFFF00u};
    static constexpr unsigned kShift[4] = {0, 8, 16, 24};
    const u32 a = addr(c, base, off), s = a & 3;
    const u32 mem = c->mem->read<u32>(a & ~3u, pc);
    const u32 v = (G32(c, rt) & kMask[s]) | (mem >> kShift[s]);
    if (!rt) return;
    if (s == 0) c->r[rt].sd[0] = static_cast<s32>(v);  // palavra inteira: estende o sinal
    else c->r[rt].uw[0] = v;                            // parcial: só os 32 bits baixos
}
inline void LDL(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    const u32 a = addr(c, base, off), s = a & 7;
    const u64 mem = c->mem->read<u64>(a & ~7u, pc);
    const unsigned shift = 56 - 8 * s;
    const u64 mask = s == 7 ? 0 : (~u64{0} >> (8 * (s + 1)));
    set64(c, rt, (G(c, rt) & mask) | (mem << shift));
}
inline void LDR(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    const u32 a = addr(c, base, off), s = a & 7;
    const u64 mem = c->mem->read<u64>(a & ~7u, pc);
    const u64 mask = s == 0 ? 0 : (~u64{0} << (64 - 8 * s));
    set64(c, rt, (G(c, rt) & mask) | (mem >> (8 * s)));
}
inline void SWL(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    static constexpr u32 kMask[4] = {0xFFFFFF00u, 0xFFFF0000u, 0xFF000000u, 0x00000000u};
    static constexpr unsigned kShift[4] = {24, 16, 8, 0};
    const u32 a = addr(c, base, off), s = a & 3;
    const u32 mem = c->mem->read<u32>(a & ~3u, pc);
    c->mem->write<u32>(a & ~3u, (G32(c, rt) >> kShift[s]) | (mem & kMask[s]), pc);
}
inline void SWR(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    static constexpr u32 kMask[4] = {0x00000000u, 0x000000FFu, 0x0000FFFFu, 0x00FFFFFFu};
    static constexpr unsigned kShift[4] = {0, 8, 16, 24};
    const u32 a = addr(c, base, off), s = a & 3;
    const u32 mem = c->mem->read<u32>(a & ~3u, pc);
    c->mem->write<u32>(a & ~3u, (G32(c, rt) << kShift[s]) | (mem & kMask[s]), pc);
}
inline void SDL(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    const u32 a = addr(c, base, off), s = a & 7;
    const u64 mem = c->mem->read<u64>(a & ~7u, pc);
    const u64 mask = s == 7 ? 0 : (~u64{0} << (8 * (s + 1)));
    c->mem->write<u64>(a & ~7u, (G(c, rt) >> (56 - 8 * s)) | (mem & mask), pc);
}
inline void SDR(Context* c, unsigned rt, unsigned base, s32 off, u32 pc) {
    const u32 a = addr(c, base, off), s = a & 7;
    const u64 mem = c->mem->read<u64>(a & ~7u, pc);
    const u64 mask = s == 0 ? 0 : (~u64{0} >> (64 - 8 * s));
    c->mem->write<u64>(a & ~7u, (G(c, rt) << (8 * s)) | (mem & mask), pc);
}

// ---------------------------------------------------------------------------
// MMI: helpers de lane
// ---------------------------------------------------------------------------

template <typename T>
inline T satAdd(T a, T b) {
    using W = std::conditional_t<std::is_signed_v<T>, s64, u64>;
    const W r = static_cast<W>(a) + static_cast<W>(b);
    if (r > static_cast<W>(std::numeric_limits<T>::max())) return std::numeric_limits<T>::max();
    if constexpr (std::is_signed_v<T>) {
        if (r < static_cast<W>(std::numeric_limits<T>::min())) return std::numeric_limits<T>::min();
    }
    return static_cast<T>(r);
}
template <typename T>
inline T satSub(T a, T b) {
    if constexpr (std::is_signed_v<T>) {
        const s64 r = static_cast<s64>(a) - static_cast<s64>(b);
        if (r > std::numeric_limits<T>::max()) return std::numeric_limits<T>::max();
        if (r < std::numeric_limits<T>::min()) return std::numeric_limits<T>::min();
        return static_cast<T>(r);
    } else {
        return a > b ? static_cast<T>(a - b) : T{0};
    }
}

#define ANYPS2_MMI_LANES(NAME, FIELD, COUNT, EXPR)                         \
    inline void NAME(Context* c, unsigned rd, unsigned rs, unsigned rt) {  \
        const Reg128 a = c->r[rs], b = c->r[rt];                           \
        Reg128 d;                                                          \
        for (int i = 0; i < COUNT; ++i) {                                  \
            const auto x = a.FIELD[i];                                     \
            const auto y = b.FIELD[i];                                     \
            d.FIELD[i] = static_cast<std::remove_reference_t<decltype(d.FIELD[0])>>(EXPR); \
        }                                                                  \
        set128(c, rd, d);                                                  \
    }

// Soma/subtração com wraparound.
ANYPS2_MMI_LANES(PADDW, uw, 4, x + y)
ANYPS2_MMI_LANES(PSUBW, uw, 4, x - y)
ANYPS2_MMI_LANES(PADDH, uh, 8, x + y)
ANYPS2_MMI_LANES(PSUBH, uh, 8, x - y)
ANYPS2_MMI_LANES(PADDB, ub, 16, x + y)
ANYPS2_MMI_LANES(PSUBB, ub, 16, x - y)
// Saturação com sinal / sem sinal.
ANYPS2_MMI_LANES(PADDSW, sw, 4, satAdd<s32>(x, y))
ANYPS2_MMI_LANES(PSUBSW, sw, 4, satSub<s32>(x, y))
ANYPS2_MMI_LANES(PADDSH, sh, 8, satAdd<s16>(x, y))
ANYPS2_MMI_LANES(PSUBSH, sh, 8, satSub<s16>(x, y))
ANYPS2_MMI_LANES(PADDSB, sb, 16, satAdd<s8>(x, y))
ANYPS2_MMI_LANES(PSUBSB, sb, 16, satSub<s8>(x, y))
ANYPS2_MMI_LANES(PADDUW, uw, 4, satAdd<u32>(x, y))
ANYPS2_MMI_LANES(PSUBUW, uw, 4, satSub<u32>(x, y))
ANYPS2_MMI_LANES(PADDUH, uh, 8, satAdd<u16>(x, y))
ANYPS2_MMI_LANES(PSUBUH, uh, 8, satSub<u16>(x, y))
ANYPS2_MMI_LANES(PADDUB, ub, 16, satAdd<u8>(x, y))
ANYPS2_MMI_LANES(PSUBUB, ub, 16, satSub<u8>(x, y))
// Comparações (resultado: todos os bits 1 ou 0).
ANYPS2_MMI_LANES(PCGTW, sw, 4, x > y ? -1 : 0)
ANYPS2_MMI_LANES(PCGTH, sh, 8, x > y ? -1 : 0)
ANYPS2_MMI_LANES(PCGTB, sb, 16, x > y ? -1 : 0)
ANYPS2_MMI_LANES(PCEQW, uw, 4, x == y ? 0xFFFFFFFFu : 0u)
ANYPS2_MMI_LANES(PCEQH, uh, 8, x == y ? 0xFFFFu : 0u)
ANYPS2_MMI_LANES(PCEQB, ub, 16, x == y ? 0xFFu : 0u)
ANYPS2_MMI_LANES(PMAXW, sw, 4, x > y ? x : y)
ANYPS2_MMI_LANES(PMINW, sw, 4, x < y ? x : y)
ANYPS2_MMI_LANES(PMAXH, sh, 8, x > y ? x : y)
ANYPS2_MMI_LANES(PMINH, sh, 8, x < y ? x : y)
// Lógicas de 128 bits.
ANYPS2_MMI_LANES(PAND, ud, 2, x & y)
ANYPS2_MMI_LANES(POR, ud, 2, x | y)
ANYPS2_MMI_LANES(PXOR, ud, 2, x ^ y)
ANYPS2_MMI_LANES(PNOR, ud, 2, ~(x | y))
#undef ANYPS2_MMI_LANES

inline void PADSBH(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    const Reg128 a = c->r[rs], b = c->r[rt];
    Reg128 d;
    for (int i = 0; i < 4; ++i) d.uh[i] = static_cast<u16>(a.uh[i] - b.uh[i]);
    for (int i = 4; i < 8; ++i) d.uh[i] = static_cast<u16>(a.uh[i] + b.uh[i]);
    set128(c, rd, d);
}

// Unárias sobre rt.
inline void PABSW(Context* c, unsigned rd, unsigned rt) {
    Reg128 d = c->r[rt];
    for (auto& v : d.sw) v = v == std::numeric_limits<s32>::min() ? std::numeric_limits<s32>::max() : (v < 0 ? -v : v);
    set128(c, rd, d);
}
inline void PABSH(Context* c, unsigned rd, unsigned rt) {
    Reg128 d = c->r[rt];
    for (auto& v : d.sh) v = static_cast<s16>(v == std::numeric_limits<s16>::min() ? std::numeric_limits<s16>::max() : (v < 0 ? -v : v));
    set128(c, rd, d);
}

// Extend / pack / interleave
inline void PEXTLW(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    const Reg128 a = c->r[rs], b = c->r[rt];
    Reg128 d;
    d.uw[0] = b.uw[0]; d.uw[1] = a.uw[0]; d.uw[2] = b.uw[1]; d.uw[3] = a.uw[1];
    set128(c, rd, d);
}
inline void PEXTUW(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    const Reg128 a = c->r[rs], b = c->r[rt];
    Reg128 d;
    d.uw[0] = b.uw[2]; d.uw[1] = a.uw[2]; d.uw[2] = b.uw[3]; d.uw[3] = a.uw[3];
    set128(c, rd, d);
}
inline void PEXTLH(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    const Reg128 a = c->r[rs], b = c->r[rt];
    Reg128 d;
    for (int i = 0; i < 4; ++i) { d.uh[2 * i] = b.uh[i]; d.uh[2 * i + 1] = a.uh[i]; }
    set128(c, rd, d);
}
inline void PEXTUH(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    const Reg128 a = c->r[rs], b = c->r[rt];
    Reg128 d;
    for (int i = 0; i < 4; ++i) { d.uh[2 * i] = b.uh[i + 4]; d.uh[2 * i + 1] = a.uh[i + 4]; }
    set128(c, rd, d);
}
inline void PEXTLB(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    const Reg128 a = c->r[rs], b = c->r[rt];
    Reg128 d;
    for (int i = 0; i < 8; ++i) { d.ub[2 * i] = b.ub[i]; d.ub[2 * i + 1] = a.ub[i]; }
    set128(c, rd, d);
}
inline void PEXTUB(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    const Reg128 a = c->r[rs], b = c->r[rt];
    Reg128 d;
    for (int i = 0; i < 8; ++i) { d.ub[2 * i] = b.ub[i + 8]; d.ub[2 * i + 1] = a.ub[i + 8]; }
    set128(c, rd, d);
}
inline void PPACW(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    const Reg128 a = c->r[rs], b = c->r[rt];
    Reg128 d;
    d.uw[0] = b.uw[0]; d.uw[1] = b.uw[2]; d.uw[2] = a.uw[0]; d.uw[3] = a.uw[2];
    set128(c, rd, d);
}
inline void PPACH(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    const Reg128 a = c->r[rs], b = c->r[rt];
    Reg128 d;
    for (int i = 0; i < 4; ++i) { d.uh[i] = b.uh[2 * i]; d.uh[i + 4] = a.uh[2 * i]; }
    set128(c, rd, d);
}
inline void PPACB(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    const Reg128 a = c->r[rs], b = c->r[rt];
    Reg128 d;
    for (int i = 0; i < 8; ++i) { d.ub[i] = b.ub[2 * i]; d.ub[i + 8] = a.ub[2 * i]; }
    set128(c, rd, d);
}
inline void PEXT5(Context* c, unsigned rd, unsigned rt) {
    Reg128 d;
    for (int i = 0; i < 4; ++i) {
        const u32 v = c->r[rt].uw[i];
        d.uw[i] = ((v & 0x001Fu) << 3) | ((v & 0x03E0u) << 6) | ((v & 0x7C00u) << 9) | ((v & 0x8000u) << 16);
    }
    set128(c, rd, d);
}
inline void PPAC5(Context* c, unsigned rd, unsigned rt) {
    Reg128 d;
    for (int i = 0; i < 4; ++i) {
        const u32 v = c->r[rt].uw[i];
        d.uw[i] = ((v >> 3) & 0x001Fu) | ((v >> 6) & 0x03E0u) | ((v >> 9) & 0x7C00u) | ((v >> 16) & 0x8000u);
    }
    set128(c, rd, d);
}
inline void PINTH(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    const Reg128 a = c->r[rs], b = c->r[rt];
    Reg128 d;
    for (int i = 0; i < 4; ++i) { d.uh[2 * i] = b.uh[i]; d.uh[2 * i + 1] = a.uh[i + 4]; }
    set128(c, rd, d);
}
inline void PINTEH(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    const Reg128 a = c->r[rs], b = c->r[rt];
    Reg128 d;
    for (int i = 0; i < 4; ++i) { d.uh[2 * i] = b.uh[2 * i]; d.uh[2 * i + 1] = a.uh[2 * i]; }
    set128(c, rd, d);
}
inline void PCPYLD(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    Reg128 d;
    d.ud[0] = c->r[rt].ud[0];
    d.ud[1] = c->r[rs].ud[0];
    set128(c, rd, d);
}
inline void PCPYUD(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    Reg128 d;
    d.ud[0] = c->r[rs].ud[1];
    d.ud[1] = c->r[rt].ud[1];
    set128(c, rd, d);
}
inline void PCPYH(Context* c, unsigned rd, unsigned rt) {
    const Reg128 b = c->r[rt];
    Reg128 d;
    for (int i = 0; i < 4; ++i) { d.uh[i] = b.uh[0]; d.uh[i + 4] = b.uh[4]; }
    set128(c, rd, d);
}
inline void permuteH(Context* c, unsigned rd, unsigned rt, const int (&idx)[8]) {
    const Reg128 b = c->r[rt];
    Reg128 d;
    for (int i = 0; i < 8; ++i) d.uh[i] = b.uh[idx[i]];
    set128(c, rd, d);
}
inline void permuteW(Context* c, unsigned rd, unsigned rt, const int (&idx)[4]) {
    const Reg128 b = c->r[rt];
    Reg128 d;
    for (int i = 0; i < 4; ++i) d.uw[i] = b.uw[idx[i]];
    set128(c, rd, d);
}
inline void PEXEH(Context* c, unsigned rd, unsigned rt) { permuteH(c, rd, rt, {2, 1, 0, 3, 6, 5, 4, 7}); }
inline void PREVH(Context* c, unsigned rd, unsigned rt) { permuteH(c, rd, rt, {3, 2, 1, 0, 7, 6, 5, 4}); }
inline void PEXCH(Context* c, unsigned rd, unsigned rt) { permuteH(c, rd, rt, {0, 2, 1, 3, 4, 6, 5, 7}); }
inline void PEXEW(Context* c, unsigned rd, unsigned rt) { permuteW(c, rd, rt, {2, 1, 0, 3}); }
inline void PROT3W(Context* c, unsigned rd, unsigned rt) { permuteW(c, rd, rt, {1, 2, 0, 3}); }
inline void PEXCW(Context* c, unsigned rd, unsigned rt) { permuteW(c, rd, rt, {0, 2, 1, 3}); }

// Shifts paralelos
inline void PSLLH(Context* c, unsigned rd, unsigned rt, unsigned sa) {
    Reg128 d = c->r[rt];
    for (auto& v : d.uh) v = static_cast<u16>(v << (sa & 15));
    set128(c, rd, d);
}
inline void PSRLH(Context* c, unsigned rd, unsigned rt, unsigned sa) {
    Reg128 d = c->r[rt];
    for (auto& v : d.uh) v = static_cast<u16>(v >> (sa & 15));
    set128(c, rd, d);
}
inline void PSRAH(Context* c, unsigned rd, unsigned rt, unsigned sa) {
    Reg128 d = c->r[rt];
    for (auto& v : d.sh) v = static_cast<s16>(v >> (sa & 15));
    set128(c, rd, d);
}
inline void PSLLW(Context* c, unsigned rd, unsigned rt, unsigned sa) {
    Reg128 d = c->r[rt];
    for (auto& v : d.uw) v <<= sa;
    set128(c, rd, d);
}
inline void PSRLW(Context* c, unsigned rd, unsigned rt, unsigned sa) {
    Reg128 d = c->r[rt];
    for (auto& v : d.uw) v >>= sa;
    set128(c, rd, d);
}
inline void PSRAW(Context* c, unsigned rd, unsigned rt, unsigned sa) {
    Reg128 d = c->r[rt];
    for (auto& v : d.sw) v >>= sa;
    set128(c, rd, d);
}
// Shifts variáveis nas palavras 0 e 2, resultado estendido para 64 bits.
inline void PSLLVW(Context* c, unsigned rd, unsigned rt, unsigned rs) {
    Reg128 d;
    d.sd[0] = sx32(c->r[rt].uw[0] << (c->r[rs].uw[0] & 31));
    d.sd[1] = sx32(c->r[rt].uw[2] << (c->r[rs].uw[2] & 31));
    set128(c, rd, d);
}
inline void PSRLVW(Context* c, unsigned rd, unsigned rt, unsigned rs) {
    Reg128 d;
    d.sd[0] = sx32(c->r[rt].uw[0] >> (c->r[rs].uw[0] & 31));
    d.sd[1] = sx32(c->r[rt].uw[2] >> (c->r[rs].uw[2] & 31));
    set128(c, rd, d);
}
inline void PSRAVW(Context* c, unsigned rd, unsigned rt, unsigned rs) {
    Reg128 d;
    d.sd[0] = c->r[rt].sw[0] >> (c->r[rs].uw[0] & 31);
    d.sd[1] = c->r[rt].sw[2] >> (c->r[rs].uw[2] & 31);
    set128(c, rd, d);
}

// Contagem de bits iguais ao sinal (menos 1), nas duas palavras baixas.
inline u32 leadingSignBits(u32 v) {
    if (v & 0x80000000u) v = ~v;
    if (v == 0) return 31;
    unsigned n = 0;
    while (!(v & 0x80000000u)) {
        v <<= 1;
        ++n;
    }
    return n - 1;
}
inline void PLZCW(Context* c, unsigned rd, unsigned rs) {
    if (!rd) return;
    const u32 a = c->r[rs].uw[0], b = c->r[rs].uw[1];
    c->r[rd].uw[0] = leadingSignBits(a);
    c->r[rd].uw[1] = leadingSignBits(b);
}

// QFSRV: concatena rs:rt (256 bits) e desloca à direita SA bytes.
inline void QFSRV(Context* c, unsigned rd, unsigned rs, unsigned rt) {
    u8 buf[32];
    std::memcpy(buf, &c->r[rt], 16);
    std::memcpy(buf + 16, &c->r[rs], 16);
    Reg128 d;
    std::memcpy(&d, buf + (c->sa & 15), 16);
    set128(c, rd, d);
}

// HI/LO de 128 bits
inline void PMFHI(Context* c, unsigned rd) { set128(c, rd, c->hi); }
inline void PMFLO(Context* c, unsigned rd) { set128(c, rd, c->lo); }
inline void PMTHI(Context* c, unsigned rs) { c->hi = c->r[rs]; }
inline void PMTLO(Context* c, unsigned rs) { c->lo = c->r[rs]; }
inline void PMFHL_LW(Context* c, unsigned rd) {
    Reg128 d;
    d.uw[0] = c->lo.uw[0]; d.uw[1] = c->hi.uw[0]; d.uw[2] = c->lo.uw[2]; d.uw[3] = c->hi.uw[2];
    set128(c, rd, d);
}
inline void PMFHL_UW(Context* c, unsigned rd) {
    Reg128 d;
    d.uw[0] = c->lo.uw[1]; d.uw[1] = c->hi.uw[1]; d.uw[2] = c->lo.uw[3]; d.uw[3] = c->hi.uw[3];
    set128(c, rd, d);
}
inline s64 sat32(s64 v) {
    if (v > std::numeric_limits<s32>::max()) return std::numeric_limits<s32>::max();
    if (v < std::numeric_limits<s32>::min()) return std::numeric_limits<s32>::min();
    return v;
}
inline s16 sat16(s32 v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<s16>(v);
}
inline void PMFHL_SLW(Context* c, unsigned rd) {
    Reg128 d;
    for (int i = 0; i < 2; ++i) {
        const u64 v = (u64{c->hi.uw[2 * i]} << 32) | c->lo.uw[2 * i];
        d.sd[i] = sat32(static_cast<s64>(v));
    }
    set128(c, rd, d);
}
inline void PMFHL_LH(Context* c, unsigned rd) {
    Reg128 d;
    d.uh[0] = c->lo.uh[0]; d.uh[1] = c->lo.uh[2]; d.uh[2] = c->hi.uh[0]; d.uh[3] = c->hi.uh[2];
    d.uh[4] = c->lo.uh[4]; d.uh[5] = c->lo.uh[6]; d.uh[6] = c->hi.uh[4]; d.uh[7] = c->hi.uh[6];
    set128(c, rd, d);
}
inline void PMFHL_SH(Context* c, unsigned rd) {
    Reg128 d;
    d.sh[0] = sat16(c->lo.sw[0]); d.sh[1] = sat16(c->lo.sw[1]);
    d.sh[2] = sat16(c->hi.sw[0]); d.sh[3] = sat16(c->hi.sw[1]);
    d.sh[4] = sat16(c->lo.sw[2]); d.sh[5] = sat16(c->lo.sw[3]);
    d.sh[6] = sat16(c->hi.sw[2]); d.sh[7] = sat16(c->hi.sw[3]);
    set128(c, rd, d);
}
inline void PMTHL_LW(Context* c, unsigned rs) {
    const Reg128 a = c->r[rs];
    c->lo.uw[0] = a.uw[0]; c->hi.uw[0] = a.uw[1]; c->lo.uw[2] = a.uw[2]; c->hi.uw[2] = a.uw[3];
}

// Multiplicação/divisão paralelas de palavras (palavras 0 e 2).
inline void pmulW(Context* c, unsigned rd, unsigned rs, unsigned rt, bool isSigned, int acc) {
    Reg128 d = rd ? c->r[rd] : Reg128{};
    for (int dd = 0; dd < 2; ++dd) {
        const int ss = dd * 2;
        u64 base = (u64{c->hi.uw[ss]} << 32) | c->lo.uw[ss];
        u64 prod = isSigned ? static_cast<u64>(sx32(c->r[rs].uw[ss]) * sx32(c->r[rt].uw[ss]))
                            : u64{c->r[rs].uw[ss]} * u64{c->r[rt].uw[ss]};
        const u64 r = acc > 0 ? base + prod : acc < 0 ? base - prod : prod;
        c->lo.sd[dd] = sx32(static_cast<u32>(r));
        c->hi.sd[dd] = sx32(static_cast<u32>(r >> 32));
        d.ud[dd] = r;
    }
    set128(c, rd, d);
}
inline void PMULTW(Context* c, unsigned rd, unsigned rs, unsigned rt) { pmulW(c, rd, rs, rt, true, 0); }
inline void PMULTUW(Context* c, unsigned rd, unsigned rs, unsigned rt) { pmulW(c, rd, rs, rt, false, 0); }
inline void PMADDW(Context* c, unsigned rd, unsigned rs, unsigned rt) { pmulW(c, rd, rs, rt, true, 1); }
inline void PMADDUW(Context* c, unsigned rd, unsigned rs, unsigned rt) { pmulW(c, rd, rs, rt, false, 1); }
inline void PMSUBW(Context* c, unsigned rd, unsigned rs, unsigned rt) { pmulW(c, rd, rs, rt, true, -1); }

inline void PDIVW(Context* c, unsigned rs, unsigned rt) {
    for (int dd = 0; dd < 2; ++dd) {
        const int ss = dd * 2;
        const s32 n = c->r[rs].sw[ss], d = c->r[rt].sw[ss];
        if (n == std::numeric_limits<s32>::min() && d == -1) {
            c->lo.sd[dd] = n;
            c->hi.sd[dd] = 0;
        } else if (d != 0) {
            c->lo.sd[dd] = n / d;
            c->hi.sd[dd] = n % d;
        } else {
            c->lo.sd[dd] = n < 0 ? 1 : -1;
            c->hi.sd[dd] = n;
        }
    }
}
inline void PDIVUW(Context* c, unsigned rs, unsigned rt) {
    for (int dd = 0; dd < 2; ++dd) {
        const int ss = dd * 2;
        const u32 n = c->r[rs].uw[ss], d = c->r[rt].uw[ss];
        if (d != 0) {
            c->lo.sd[dd] = sx32(n / d);
            c->hi.sd[dd] = sx32(n % d);
        } else {
            c->lo.sd[dd] = -1;
            c->hi.sd[dd] = sx32(n);
        }
    }
}
inline void PDIVBW(Context* c, unsigned rs, unsigned rt) {
    const s16 d = c->r[rt].sh[0];
    for (int i = 0; i < 4; ++i) {
        const s32 n = c->r[rs].sw[i];
        if (n == std::numeric_limits<s32>::min() && d == -1) {
            c->lo.sw[i] = n;
            c->hi.sw[i] = 0;
        } else if (d != 0) {
            c->lo.sw[i] = n / d;
            c->hi.sw[i] = static_cast<s16>(n % d);
        } else {
            c->lo.sw[i] = n < 0 ? 1 : -1;
            c->hi.sw[i] = n;
        }
    }
}

// Multiplicação de halfwords. Organização do resultado (manual do EE):
// produtos 0,1 -> LO[0,1]; 2,3 -> HI[0,1]; 4,5 -> LO[2,3]; 6,7 -> HI[2,3];
// rd = {LO0, HI0, LO2, HI2}.
inline void pmulH(Context* c, unsigned rd, unsigned rs, unsigned rt, int acc) {
    const Reg128 a = c->r[rs], b = c->r[rt];
    u32* dst[8] = {&c->lo.uw[0], &c->lo.uw[1], &c->hi.uw[0], &c->hi.uw[1],
                   &c->lo.uw[2], &c->lo.uw[3], &c->hi.uw[2], &c->hi.uw[3]};
    for (int i = 0; i < 8; ++i) {
        const u32 p = static_cast<u32>(s32{a.sh[i]} * s32{b.sh[i]});
        *dst[i] = acc > 0 ? *dst[i] + p : acc < 0 ? *dst[i] - p : p;
    }
    if (rd) {
        c->r[rd].uw[0] = c->lo.uw[0];
        c->r[rd].uw[1] = c->hi.uw[0];
        c->r[rd].uw[2] = c->lo.uw[2];
        c->r[rd].uw[3] = c->hi.uw[2];
    }
}
inline void PMULTH(Context* c, unsigned rd, unsigned rs, unsigned rt) { pmulH(c, rd, rs, rt, 0); }
inline void PMADDH(Context* c, unsigned rd, unsigned rs, unsigned rt) { pmulH(c, rd, rs, rt, 1); }
inline void PMSUBH(Context* c, unsigned rd, unsigned rs, unsigned rt) { pmulH(c, rd, rs, rt, -1); }

// Soma/subtração horizontal de produtos. As palavras ímpares de LO/HI recebem
// o produto do par alto (PHMADH) ou seu complemento (PHMSBH), conforme
// testes em hardware documentados pelo PCSX2.
inline void phmH(Context* c, unsigned rd, unsigned rs, unsigned rt, bool sub) {
    const Reg128 a = c->r[rs], b = c->r[rt];
    u32* even[4] = {&c->lo.uw[0], &c->hi.uw[0], &c->lo.uw[2], &c->hi.uw[2]};
    u32* odd[4] = {&c->lo.uw[1], &c->hi.uw[1], &c->lo.uw[3], &c->hi.uw[3]};
    for (int i = 0; i < 4; ++i) {
        const u32 hiP = static_cast<u32>(s32{a.sh[2 * i + 1]} * s32{b.sh[2 * i + 1]});
        const u32 loP = static_cast<u32>(s32{a.sh[2 * i]} * s32{b.sh[2 * i]});
        *even[i] = sub ? hiP - loP : hiP + loP;
        *odd[i] = sub ? ~hiP : hiP;
    }
    if (rd) {
        c->r[rd].uw[0] = c->lo.uw[0];
        c->r[rd].uw[1] = c->hi.uw[0];
        c->r[rd].uw[2] = c->lo.uw[2];
        c->r[rd].uw[3] = c->hi.uw[2];
    }
}
inline void PHMADH(Context* c, unsigned rd, unsigned rs, unsigned rt) { phmH(c, rd, rs, rt, false); }
inline void PHMSBH(Context* c, unsigned rd, unsigned rs, unsigned rt) { phmH(c, rd, rs, rt, true); }

// ---------------------------------------------------------------------------
// COP1 (FPU). O PS2 não segue IEEE 754: não há NaN/Inf nem denormais;
// expoente 255 é um número normal e resultados que estourariam saturam em
// ±FLT_MAX ("Fmax"), com flags O/U/I/D no FCR31. O arredondamento é em
// direção a zero (ps2float.h).
// ---------------------------------------------------------------------------

namespace fcr {
inline constexpr u32 C = 0x00800000u, I = 0x00020000u, D = 0x00010000u, O = 0x00008000u,
                     U = 0x00004000u, SI = 0x00000040u, SD = 0x00000020u, SO = 0x00000010u,
                     SU = 0x00000008u;
}
inline constexpr u32 kPosFmax = 0x7F7FFFFFu;

inline float bitsToFloat(u32 b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
inline u32 floatToBits(float f) {
    u32 b;
    std::memcpy(&b, &f, 4);
    return b;
}
// Converte um valor do PS2 para um float do host utilizável.
inline float ps2ToHost(u32 b) {
    const u32 exp = b & 0x7F800000u;
    if (exp == 0x7F800000u) return bitsToFloat((b & 0x80000000u) | kPosFmax);  // "Inf/NaN" -> Fmax
    if (exp == 0) return bitsToFloat(b & 0x80000000u);                            // denormal -> ±0
    return bitsToFloat(b);
}
// Converte o resultado do host para o PS2, saturando e marcando flags.
inline u32 hostToPs2(Context* c, float f) {
    u32 b = floatToBits(f);
    const u32 exp = b & 0x7F800000u;
    if (exp == 0x7F800000u) {
        c->fcr31 |= fcr::O | fcr::SO;
        return (b & 0x80000000u) | kPosFmax;
    }
    if (exp == 0 && (b & 0x007FFFFFu)) {
        c->fcr31 |= fcr::U | fcr::SU;
        return b & 0x80000000u;
    }
    return b;
}
inline void fpuClearOU(Context* c) { c->fcr31 &= ~(fcr::O | fcr::U); }
inline float F(Context* c, unsigned r) { return ps2ToHost(c->f[r]); }
inline void setF(Context* c, unsigned r, float v) {
    fpuClearOU(c);
    c->f[r] = hostToPs2(c, v);
}

inline void MFC1(Context* c, unsigned rt, unsigned fs) { set32(c, rt, c->f[fs]); }
inline void MTC1(Context* c, unsigned rt, unsigned fs) { c->f[fs] = G32(c, rt); }
inline void CFC1(Context* c, unsigned rt, unsigned fs) {
    set32(c, rt, fs == 0 ? 0x00002E00u : fs == 31 ? c->fcr31 : 0u);
}
inline void CTC1(Context* c, unsigned rt, unsigned fs) {
    if (fs == 31) c->fcr31 = (G32(c, rt) & 0x0083C078u) | 0x01000001u;
}
inline void LWC1(Context* c, unsigned ft, unsigned base, s32 off, u32 pc) {
    c->f[ft] = c->mem->read<u32>(addr(c, base, off), pc);
}
inline void SWC1(Context* c, unsigned ft, unsigned base, s32 off, u32 pc) {
    c->mem->write<u32>(addr(c, base, off), c->f[ft], pc);
}

inline void ADD_S(Context* c, unsigned fd, unsigned fs, unsigned ft) { setF(c, fd, ps2f::add(F(c, fs), F(c, ft))); }
inline void SUB_S(Context* c, unsigned fd, unsigned fs, unsigned ft) { setF(c, fd, ps2f::sub(F(c, fs), F(c, ft))); }
inline void MUL_S(Context* c, unsigned fd, unsigned fs, unsigned ft) { setF(c, fd, ps2f::mul(F(c, fs), F(c, ft))); }
inline void DIV_S(Context* c, unsigned fd, unsigned fs, unsigned ft) {
    c->fcr31 &= ~(fcr::I | fcr::D);
    const u32 a = c->f[fs], b = c->f[ft];
    if ((b & 0x7F800000u) == 0) {  // divisor zero (ou denormal)
        c->fcr31 |= (a & 0x7F800000u) == 0 ? (fcr::I | fcr::SI) : (fcr::D | fcr::SD);
        c->f[fd] = ((a ^ b) & 0x80000000u) | kPosFmax;
        return;
    }
    setF(c, fd, ps2f::div(F(c, fs), F(c, ft)));
}
inline void SQRT_S(Context* c, unsigned fd, unsigned ft) {
    c->fcr31 &= ~(fcr::I | fcr::D);
    const u32 b = c->f[ft];
    if ((b & 0x7F800000u) == 0) {
        c->f[fd] = b & 0x80000000u;
        return;
    }
    if (b & 0x80000000u) c->fcr31 |= fcr::I | fcr::SI;
    c->f[fd] = hostToPs2(c, ps2f::sqrt(std::fabs(ps2ToHost(b))));
}
inline void RSQRT_S(Context* c, unsigned fd, unsigned fs, unsigned ft) {
    c->fcr31 &= ~(fcr::I | fcr::D);
    const u32 a = c->f[fs], b = c->f[ft];
    if ((b & 0x7F800000u) == 0) {
        c->fcr31 |= fcr::D | fcr::SD;
        c->f[fd] = ((a ^ b) & 0x80000000u) | kPosFmax;
        return;
    }
    if (b & 0x80000000u) c->fcr31 |= fcr::I | fcr::SI;
    setF(c, fd, ps2f::div(ps2ToHost(a), ps2f::sqrt(std::fabs(ps2ToHost(b)))));
}
inline void ABS_S(Context* c, unsigned fd, unsigned fs) { c->f[fd] = c->f[fs] & 0x7FFFFFFFu; fpuClearOU(c); }
inline void NEG_S(Context* c, unsigned fd, unsigned fs) { c->f[fd] = c->f[fs] ^ 0x80000000u; fpuClearOU(c); }
inline void MOV_S(Context* c, unsigned fd, unsigned fs) { c->f[fd] = c->f[fs]; }
inline void ADDA_S(Context* c, unsigned fs, unsigned ft) { fpuClearOU(c); c->acc = hostToPs2(c, ps2f::add(F(c, fs), F(c, ft))); }
inline void SUBA_S(Context* c, unsigned fs, unsigned ft) { fpuClearOU(c); c->acc = hostToPs2(c, ps2f::sub(F(c, fs), F(c, ft))); }
inline void MULA_S(Context* c, unsigned fs, unsigned ft) { fpuClearOU(c); c->acc = hostToPs2(c, ps2f::mul(F(c, fs), F(c, ft))); }
inline void MADD_S(Context* c, unsigned fd, unsigned fs, unsigned ft) { setF(c, fd, ps2f::add(ps2ToHost(c->acc), ps2f::mul(F(c, fs), F(c, ft)))); }
inline void MSUB_S(Context* c, unsigned fd, unsigned fs, unsigned ft) { setF(c, fd, ps2f::sub(ps2ToHost(c->acc), ps2f::mul(F(c, fs), F(c, ft)))); }
inline void MADDA_S(Context* c, unsigned fs, unsigned ft) { fpuClearOU(c); c->acc = hostToPs2(c, ps2f::add(ps2ToHost(c->acc), ps2f::mul(F(c, fs), F(c, ft)))); }
inline void MSUBA_S(Context* c, unsigned fs, unsigned ft) { fpuClearOU(c); c->acc = hostToPs2(c, ps2f::sub(ps2ToHost(c->acc), ps2f::mul(F(c, fs), F(c, ft)))); }
// MAX/MIN comparam como inteiros em sinal-magnitude (ordem total do PS2).
inline s64 fpuOrderKey(u32 b) { return (b & 0x80000000u) ? -static_cast<s64>(b & 0x7FFFFFFFu) : static_cast<s64>(b); }
inline void MAX_S(Context* c, unsigned fd, unsigned fs, unsigned ft) {
    c->f[fd] = fpuOrderKey(c->f[fs]) >= fpuOrderKey(c->f[ft]) ? c->f[fs] : c->f[ft];
    fpuClearOU(c);
}
inline void MIN_S(Context* c, unsigned fd, unsigned fs, unsigned ft) {
    c->f[fd] = fpuOrderKey(c->f[fs]) <= fpuOrderKey(c->f[ft]) ? c->f[fs] : c->f[ft];
    fpuClearOU(c);
}
inline void CVT_S_W(Context* c, unsigned fd, unsigned fs) {
    c->f[fd] = floatToBits(static_cast<float>(static_cast<s32>(c->f[fs])));
}
inline void CVT_W_S(Context* c, unsigned fd, unsigned fs) {
    const u32 b = c->f[fs];
    if ((b & 0x7F800000u) >= 0x4F000000u) {  // |x| >= 2^31: satura
        c->f[fd] = (b & 0x80000000u) ? 0x80000000u : 0x7FFFFFFFu;
    } else {
        c->f[fd] = static_cast<u32>(static_cast<s32>(ps2ToHost(b)));  // trunca
    }
}
inline void setC(Context* c, bool v) {
    if (v) c->fcr31 |= fcr::C;
    else c->fcr31 &= ~fcr::C;
}
inline void C_F_S(Context* c, unsigned, unsigned) { setC(c, false); }
inline void C_EQ_S(Context* c, unsigned fs, unsigned ft) { setC(c, F(c, fs) == F(c, ft)); }
inline void C_LT_S(Context* c, unsigned fs, unsigned ft) { setC(c, F(c, fs) < F(c, ft)); }
inline void C_LE_S(Context* c, unsigned fs, unsigned ft) { setC(c, F(c, fs) <= F(c, ft)); }
inline bool fpuCondition(Context* c) { return (c->fcr31 & fcr::C) != 0; }
// CPCOND0 (BC0T/BC0F): no EE é ligado ao DMAC — verdadeiro quando todos os
// canais marcados em D_PCR.CPC terminaram (D_STAT.CIS).
bool cop0Condition(Context* c);

// ---------------------------------------------------------------------------
// COP0
// ---------------------------------------------------------------------------

std::uint32_t readCop0(Context* c, unsigned reg, u32 pc);
void writeCop0(Context* c, unsigned reg, u32 value, u32 pc);
std::uint32_t readPerfCounter(Context* c, unsigned reg, bool counter);

inline void MFC0(Context* c, unsigned rt, unsigned rd, u32 pc) { set32(c, rt, readCop0(c, rd, pc)); }
inline void MTC0(Context* c, unsigned rt, unsigned rd, u32 pc) { writeCop0(c, rd, G32(c, rt), pc); }
inline void MFPS(Context* c, unsigned rt, unsigned reg) { set32(c, rt, readPerfCounter(c, reg, false)); }
inline void MFPC(Context* c, unsigned rt, unsigned reg) { set32(c, rt, readPerfCounter(c, reg, true)); }
inline void MTPS(Context*, unsigned, unsigned) {}
inline void MTPC(Context*, unsigned, unsigned) {}
inline constexpr u32 kStatusEIE = 1u << 16;
void onInterruptsEnabled(Context* c);
inline void EI(Context* c) {
    c->cop0[cop0::Status] |= kStatusEIE;
    onInterruptsEnabled(c);
}
inline void DI(Context* c) { c->cop0[cop0::Status] &= ~kStatusEIE; }
inline constexpr u32 kStatusEXL = 1u << 1;
inline constexpr u32 kStatusERL = 1u << 2;
// ERET (retorno de exceção, sem delay slot): com ERL ligado volta para
// ErrorEPC e limpa ERL; senão volta para EPC e limpa EXL. Devolve o destino.
// Jogos usam isso para sair de trechos que rodam em modo kernel (ex.: a
// libkernel da Sony escreve nos timers do kernel com ERL ligado e faz "eret"
// com ErrorEPC = ra).
inline u32 ERET(Context* c) {
    u32& sr = c->cop0[cop0::Status];
    if (sr & kStatusERL) {
        sr &= ~kStatusERL;
        return c->cop0[cop0::ErrorEPC];
    }
    sr &= ~kStatusEXL;
    return c->cop0[cop0::EPC];
}

// ---------------------------------------------------------------------------
// COP2 / VU0 em modo macro. As instruções vetoriais são executadas pelo
// mesmo núcleo do microcódigo (vu/vu_core.h), com o pipeline concluído a
// cada instrução.
// ---------------------------------------------------------------------------

inline void QMFC2(Context* c, unsigned rt, unsigned fs) { set128(c, rt, c->vf[fs]); }
inline void QMTC2(Context* c, unsigned rt, unsigned fs) {
    if (fs) c->vf[fs] = c->r[rt];  // vf0 é constante (0,0,0,1)
}
u32 cfc2(Context* c, unsigned id);
inline void CFC2(Context* c, unsigned rt, unsigned id) { set32(c, rt, cfc2(c, id)); }
// Macroinstrução vetorial/inteira: (lower, upper) no formato do microcódigo.
void vu0Macro(Context* c, u32 lowerWord, u32 upperWord, u32 pc);
// VCALLMS/VCALLMSR: executa um microprograma do VU0 (endereço em bytes).
void vu0Call(Context* c, u32 addr, u32 pc);
inline void VCALLMS(Context* c, u32 imm, u32 pc) { vu0Call(c, imm * 8, pc); }
inline void VCALLMSR(Context* c, u32 pc) { vu0Call(c, (c->vi[27] & 0xFFFF) * 8, pc); }
// CPCOND2 (BC2F/BC2T): o VU0 nunca está ocupado do ponto de vista do EE
// (microprogramas rodam até o fim quando chamados).
inline bool cop2Condition(Context*) { return false; }
void ctc2(Context* c, unsigned id, u32 value, u32 pc);
inline void CTC2(Context* c, unsigned rt, unsigned id, u32 pc) { ctc2(c, id, G32(c, rt), pc); }
inline void LQC2(Context* c, unsigned ft, unsigned base, s32 off, u32 pc) {
    Reg128 v;
    c->mem->read128(addr(c, base, off) & ~15u, v, pc);
    if (ft) c->vf[ft] = v;
}
inline void SQC2(Context* c, unsigned ft, unsigned base, s32 off, u32 pc) {
    c->mem->write128(addr(c, base, off) & ~15u, c->vf[ft], pc);
}

}  // namespace anyps2::rt::ops
