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
    if (!std::isfinite(s)) return s;
    // TwoSum (Knuth): erro exato de s = a + b arredondado.
    const float bb = s - a;
    const float err = (a - (s - bb)) + (b - bb);
    if (err != 0.0f && ((err < 0.0f) != (s < 0.0f))) return towardZero(s);
    return s;
}

inline float sub(float a, float b) {
    return add(a, -b);
}

inline float mul(float a, float b) {
    const double exact = static_cast<double>(a) * static_cast<double>(b);
    float r = static_cast<float>(exact);
    if (!std::isfinite(r)) return r;
    if (std::fabs(static_cast<double>(r)) > std::fabs(exact)) r = towardZero(r);
    return r;
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

}  // namespace anyps2::rt::ps2f
