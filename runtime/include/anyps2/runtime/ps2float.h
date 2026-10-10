#pragma once

// Aritmética de ponto flutuante do PS2 (FPU do EE e FMACs dos VUs).
//
// O formato é o de 32 bits do IEEE 754, mas sem NaN/Inf/denormais:
//  * expoente 255 é um número normal (até ~2^129); aqui ele é aproximado
//    por ±Fmax (0x7F7FFFFF) na entrada;
//  * denormais na entrada valem ±0;
//  * resultados que estouram saturam em ±Fmax (flag O) e os que ficam
//    abaixo do menor normal viram ±0 (flag U);
//  * o arredondamento é sempre em direção a zero (truncamento).
//
// O truncamento é calculado exatamente sem mudar o modo de arredondamento
// do host: faz-se a operação no modo padrão (mais próximo) e recupera-se o
// erro exato (TwoSum para soma; produto em double, que é exato para dois
// floats, para multiplicação/divisão/raiz). Se o resultado arredondado ficou
// mais longe de zero que o valor exato, recua um ulp em direção a zero.
//
// Limitação conhecida: o somador do PS2 descarta bits do operando menor além
// da precisão antes de somar, o que em casos raros difere de "IEEE com
// truncamento" no último bit.

#include <cmath>
#include <cstdint>
#include <cstring>

#if (defined(__SSE2__) || defined(_M_X64)) && !defined(ANYPS2_PS2F_NO_SSE2)
#define ANYPS2_PS2F_SSE2 1
#include <emmintrin.h>
#endif

namespace anyps2::rt::ps2f {

inline constexpr std::uint32_t kFmax = 0x7F7FFFFFu;

inline float fromBits(std::uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}
inline std::uint32_t toBits(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// Valor do PS2 → float do host utilizável.
inline float in(std::uint32_t b) {
    const std::uint32_t exp = b & 0x7F800000u;
    if (exp == 0x7F800000u) return fromBits((b & 0x80000000u) | kFmax);
    if (exp == 0) return fromBits(b & 0x80000000u);
    return fromBits(b);
}

struct Out {
    std::uint32_t bits;
    bool overflow;
    bool underflow;
};

// Resultado do host (já truncado) → valor do PS2 com saturação.
inline Out out(float f) {
    const std::uint32_t b = toBits(f);
    const std::uint32_t exp = b & 0x7F800000u;
    if (exp == 0x7F800000u) return {(b & 0x80000000u) | kFmax, true, false};
    if (exp == 0 && (b & 0x007FFFFFu)) return {b & 0x80000000u, false, true};
    return {b, false, false};
}

// Recua um ulp em direção a zero. Com o campo de magnitude em bits (sinal
// fora), "um ulp menor" é só decrementar: vale também nos denormais (o
// menor vira ±0) e no salto de 0x7F800000 (infinito) para ±Fmax. std::nextafter
// faz o mesmo, mas chamada fora de linha num caminho quente do FMAC.
// ±0 e NaN voltam como estão (nunca são chamados assim: o resultado é finito e
// diferente de zero quando há o que recuar).
inline float towardZero(float r) {
    std::uint32_t b = toBits(r);
    const std::uint32_t mag = b & 0x7FFFFFFFu;
    if (mag == 0 || mag > 0x7F800000u) return r;
    b = (b & 0x80000000u) | (mag - 1);
    return fromBits(b);
}

inline float add(float a, float b) {
    const float s = a + b;
    // TwoSum (Knuth): erro exato de s = a + b arredondado.
    const float bb = s - a;
    const float err = (a - (s - bb)) + (b - bb);
    const std::uint32_t bits = toBits(s);
    const std::uint32_t mag = bits & 0x7FFFFFFFu;
    // Sem desvio: o sentido do erro é imprevisível (metade das somas recua um
    // ulp) e um desvio errado custa mais que o cálculo. Infinito, NaN e zero
    // ficam como estão (mag - 1 < 0x7F7FFFFF só vale para finitos não nulos).
    const bool away = (err != 0.0f) & ((err < 0.0f) != (s < 0.0f)) & (mag - 1u < 0x7F7FFFFFu);
    return fromBits(bits - (away ? 1u : 0u));
}

inline float sub(float a, float b) {
    return add(a, -b);
}

inline float mul(float a, float b) {
    const double exact = static_cast<double>(a) * static_cast<double>(b);
    const float r = static_cast<float>(exact);
    const std::uint32_t bits = toBits(r);
    const std::uint32_t mag = bits & 0x7FFFFFFFu;
    // Sem desvio (ver add): recua um ulp se o arredondamento passou do exato.
    const bool away = (std::fabs(static_cast<double>(r)) > std::fabs(exact)) & (mag - 1u < 0x7F7FFFFFu);
    return fromBits(bits - (away ? 1u : 0u));
}

inline float div(float a, float b) {
    float q = a / b;
    if (!std::isfinite(q) || q == 0.0f) return q;
    // q·b em double é exato; se |q·b| > |a|, q passou do valor exato.
    if (std::fabs(static_cast<double>(q) * static_cast<double>(b)) > std::fabs(static_cast<double>(a))) {
        q = towardZero(q);
    }
    return q;
}

inline float sqrt(float a) {
    float s = std::sqrt(a);
    if (s == 0.0f || !std::isfinite(s)) return s;
    if (static_cast<double>(s) * static_cast<double>(s) > static_cast<double>(a)) s = towardZero(s);
    return s;
}

// ---------------------------------------------------------------------------
// Versões vetoriais (4 floats de uma vez) de in/out/add/sub/mul, só com SSE2
// (base do x86-64). Cada componente dá, bit a bit, o mesmo resultado da versão
// escalar acima: mesmas operações do host na mesma ordem (soma e TwoSum em
// float; produto em double, exato, arredondado para float) e o mesmo recuo de
// um ulp por subtração de bits. Sem SSE2 só existe o escalar.
// ---------------------------------------------------------------------------
#ifdef ANYPS2_PS2F_SSE2

namespace v4 {

#if defined(_MSC_VER)
#define ANYPS2_PS2F_V4_INLINE __forceinline
#elif defined(__GNUC__)
#define ANYPS2_PS2F_V4_INLINE inline __attribute__((always_inline))
#else
#define ANYPS2_PS2F_V4_INLINE inline
#endif

ANYPS2_PS2F_V4_INLINE __m128i splat(std::uint32_t v) {
    return _mm_set1_epi32(static_cast<int>(v));
}

// (m & a) | (~m & b), por lane
ANYPS2_PS2F_V4_INLINE __m128i select(__m128i m, __m128i a, __m128i b) {
    return _mm_or_si128(_mm_and_si128(m, a), _mm_andnot_si128(m, b));
}

// Os bits dos 4 valores do PS2 → floats do host utilizáveis (ver ps2f::in).
ANYPS2_PS2F_V4_INLINE __m128 in(__m128i b) {
    const __m128i expMask = splat(0x7F800000u);
    const __m128i sign = _mm_and_si128(b, splat(0x80000000u));
    const __m128i e = _mm_and_si128(b, expMask);
    __m128i r = select(_mm_cmpeq_epi32(e, expMask), _mm_or_si128(sign, splat(kFmax)), b);
    r = select(_mm_cmpeq_epi32(e, _mm_setzero_si128()), sign, r);
    return _mm_castsi128_ps(r);
}

// Recua um ulp onde `away` é verdadeiro e o valor é finito e não nulo (ver add/mul).
ANYPS2_PS2F_V4_INLINE __m128 stepBack(__m128 r, __m128 away) {
    const __m128i bits = _mm_castps_si128(r);
    const __m128i mag = _mm_and_si128(bits, splat(0x7FFFFFFFu));
    // mag está em 0..0x7FFFFFFF, então a comparação com sinal serve: 1 <= mag <= Fmax.
    const __m128i finiteNz = _mm_and_si128(_mm_cmpgt_epi32(mag, _mm_setzero_si128()),
                                           _mm_cmpgt_epi32(splat(0x7F800000u), mag));
    const __m128i m = _mm_and_si128(_mm_castps_si128(away), finiteNz);
    return _mm_castsi128_ps(_mm_add_epi32(bits, m));  // m = -1 onde recua: bits - 1
}

ANYPS2_PS2F_V4_INLINE __m128 add(__m128 a, __m128 b) {
    const __m128 s = _mm_add_ps(a, b);
    const __m128 bb = _mm_sub_ps(s, a);
    const __m128 err = _mm_add_ps(_mm_sub_ps(a, _mm_sub_ps(s, bb)), _mm_sub_ps(b, bb));
    const __m128 zero = _mm_setzero_ps();
    const __m128 away = _mm_and_ps(_mm_cmpneq_ps(err, zero),
                                   _mm_xor_ps(_mm_cmplt_ps(err, zero), _mm_cmplt_ps(s, zero)));
    return stepBack(s, away);
}

ANYPS2_PS2F_V4_INLINE __m128 sub(__m128 a, __m128 b) {
    return add(a, _mm_xor_ps(b, _mm_castsi128_ps(splat(0x80000000u))));
}

ANYPS2_PS2F_V4_INLINE __m128 mul(__m128 a, __m128 b) {
    const __m128d absMask = _mm_castsi128_pd(_mm_set1_epi64x(0x7FFFFFFFFFFFFFFFll));
    const __m128d exLo = _mm_mul_pd(_mm_cvtps_pd(a), _mm_cvtps_pd(b));
    const __m128d exHi = _mm_mul_pd(_mm_cvtps_pd(_mm_movehl_ps(a, a)), _mm_cvtps_pd(_mm_movehl_ps(b, b)));
    const __m128 r = _mm_movelh_ps(_mm_cvtpd_ps(exLo), _mm_cvtpd_ps(exHi));
    const __m128d gtLo = _mm_cmpgt_pd(_mm_and_pd(_mm_cvtps_pd(r), absMask), _mm_and_pd(exLo, absMask));
    const __m128d gtHi = _mm_cmpgt_pd(_mm_and_pd(_mm_cvtps_pd(_mm_movehl_ps(r, r)), absMask),
                                      _mm_and_pd(exHi, absMask));
    // Máscaras de 64 bits → de 32 bits (a metade baixa de cada uma basta).
    const __m128 away = _mm_shuffle_ps(_mm_castpd_ps(gtLo), _mm_castpd_ps(gtHi), _MM_SHUFFLE(2, 0, 2, 0));
    return stepBack(r, away);
}

// Resultado de out() para 4 valores: bits já saturados e as máscaras (tudo 1
// por lane) de overflow e underflow.
struct Out {
    __m128i bits;
    __m128i overflow;
    __m128i underflow;
};

ANYPS2_PS2F_V4_INLINE Out out(__m128 f) {
    const __m128i b = _mm_castps_si128(f);
    const __m128i expMask = splat(0x7F800000u);
    const __m128i sign = _mm_and_si128(b, splat(0x80000000u));
    const __m128i e = _mm_and_si128(b, expMask);
    const __m128i zero = _mm_setzero_si128();
    const __m128i ovf = _mm_cmpeq_epi32(e, expMask);
    const __m128i mantZero = _mm_cmpeq_epi32(_mm_and_si128(b, splat(0x007FFFFFu)), zero);
    const __m128i unf = _mm_andnot_si128(mantZero, _mm_cmpeq_epi32(e, zero));
    __m128i r = select(ovf, _mm_or_si128(sign, splat(kFmax)), b);
    r = select(unf, sign, r);
    return {r, ovf, unf};
}

}  // namespace v4
#endif  // SSE2

}  // namespace anyps2::rt::ps2f
