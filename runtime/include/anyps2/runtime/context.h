#pragma once

#include <bit>
#include <cstdint>

namespace anyps2::rt {

static_assert(std::endian::native == std::endian::little,
              "o runtime assume host little-endian (x86-64/ARM64)");

class Memory;
class Runtime;

// Registrador de 128 bits do R5900. Instruções comuns usam só os 64 bits
// baixos (ud[0]) e preservam os altos; as MMI operam nas 128 bits.
// O acesso por lanes usa union (suportado por GCC, Clang e MSVC).
union alignas(16) Reg128 {
    std::uint64_t ud[2];
    std::int64_t sd[2];
    std::uint32_t uw[4];
    std::int32_t sw[4];
    std::uint16_t uh[8];
    std::int16_t sh[8];
    std::uint8_t ub[16];
    std::int8_t sb[16];
};
static_assert(sizeof(Reg128) == 16);

// Estado completo do EE visível ao código gerado. Todo estado do guest vive
// aqui ou na memória emulada — nunca em variáveis locais do host entre
// instruções. Isso permite reentrar uma função no meio (retorno de
// longjmp, troca de contexto) a partir do despachante.
struct alignas(16) Context {
    Reg128 r[32];
    Reg128 hi;  // ud[0] = HI, ud[1] = HI1
    Reg128 lo;  // ud[0] = LO, ud[1] = LO1
    std::uint32_t sa = 0;  // shift amount do QFSRV (em bytes)
    std::uint32_t pc = 0;  // alvo corrente de despacho/retorno

    // Orçamento de instruções até o próximo safepoint. O código gerado
    // decrementa em cada desvio para trás (laços); ao ficar negativo chama o
    // runtime, que avança o relógio, dispara timers/VBlank e entrega
    // interrupções. Não pertence a uma thread: o kernel o preserva na troca.
    std::int32_t budget = 0;
    std::int32_t budgetReload = 0;

    // COP1 (FPU de precisão simples). Guardado como bits.
    std::uint32_t f[32];
    std::uint32_t acc = 0;
    std::uint32_t fcr31 = 0;

    // COP0
    std::uint32_t cop0[32];

    // COP2 / VU0: os mesmos registradores no modo macro (EE) e micro.
    // vi[0..15] inteiros de 16 bits; vi[16..31] controle (status, MAC,
    // clip, R, I, Q, TPC, CMSAR0, FBRST, VPU-STAT, CMSAR1).
    Reg128 vf[32];
    std::uint32_t vi[32];
    Reg128 vacc;

    Memory* mem = nullptr;
    Runtime* rt = nullptr;
};

// Índices de registradores do COP0 que o runtime usa.
namespace cop0 {
inline constexpr unsigned Index = 0, Random = 1, EntryLo0 = 2, EntryLo1 = 3, Context = 4,
                          PageMask = 5, Wired = 6, BadVAddr = 8, Count = 9, EntryHi = 10,
                          Compare = 11, Status = 12, Cause = 13, EPC = 14, PRId = 15,
                          Config = 16, BadPAddr = 23, Debug = 24, Perf = 25, TagLo = 28,
                          TagHi = 29, ErrorEPC = 30;
}

}  // namespace anyps2::rt
