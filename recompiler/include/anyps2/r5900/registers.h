#pragma once

#include <cstdint>
#include <string_view>

namespace anyps2::r5900 {

// Nomes ABI o32 dos GPRs, como o GNU objdump imprime (zero, at, v0 ... ra).
std::string_view gprName(std::uint32_t index);

// Nomes dos registradores do COP0 do R5900 no estilo GNU (c0_sr, c0_epc...).
// Registradores sem nome retornam string vazia (imprime-se "$N").
std::string_view cop0Name(std::uint32_t index);

// Índices úteis.
namespace gpr {
inline constexpr std::uint32_t zero = 0, at = 1, v0 = 2, v1 = 3, a0 = 4, a1 = 5, a2 = 6, a3 = 7,
                               t0 = 8, t1 = 9, t2 = 10, t3 = 11, t4 = 12, t5 = 13, t6 = 14,
                               t7 = 15, s0 = 16, s1 = 17, s2 = 18, s3 = 19, s4 = 20, s5 = 21,
                               s6 = 22, s7 = 23, t8 = 24, t9 = 25, k0 = 26, k1 = 27, gp = 28,
                               sp = 29, fp = 30, ra = 31;
}  // namespace gpr

}  // namespace anyps2::r5900
