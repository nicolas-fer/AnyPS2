#pragma once

#include <cstdint>

// Peças de MPEG-2 (ISO/IEC 13818-2) usadas pelo IPU: códigos de comprimento
// variável do anexo B, varreduras, escala de quantização não linear e a IDCT.
namespace anyps2::rt::mpeg {

// Resultado de um código: valor e quantos bits ele ocupa (0 = código
// inválido).
struct Vlc {
    int value = 0;
    unsigned len = 0;
};

enum class Table {
    Mbai,        // B-1: 1–33; 34 = macroblock_stuffing, 35 = macroblock_escape
    MbTypeI,     // B-2/B-3/B-4: bits 1 intra, 2 pattern, 4 backward, 8 forward,
    MbTypeP,     // 16 quant
    MbTypeB,
    Cbp,         // B-9 (inclui o cbp 0 do MPEG-2)
    MotionCode,  // B-10: valor absoluto 0–16, sem o bit de sinal
    DmVector,    // B-11: 0, 1, -1
    DcSizeLuma,  // B-12
    DcSizeChroma,  // B-13
};
inline constexpr int kMbIntra = 1, kMbPattern = 2, kMbBackward = 4, kMbForward = 8, kMbQuant = 16;
inline constexpr int kMbaiStuffing = 34, kMbaiEscape = 35;

// Decodifica o código no começo de `bits` (os próximos 32 bits do fluxo,
// o primeiro no bit 31).
Vlc decode(Table table, std::uint32_t bits);

// Coeficiente DCT (B-14 com tableOne = false, B-15 com true), sem o bit de
// sinal, que vem depois de len bits.
struct Dct {
    enum Kind : std::uint8_t { Invalid, Coef, Eob, Escape };
    Kind kind = Invalid;
    std::uint8_t run = 0;
    std::uint8_t level = 0;
    std::uint8_t len = 0;
};
// first: primeiro coeficiente de um bloco não intra, em que "1s" é (0,1).
Dct decodeDct(bool tableOne, bool first, std::uint32_t bits);

extern const std::uint8_t kZigzag[64];     // posição na varredura → posição no bloco
extern const std::uint8_t kAlternate[64];
extern const std::uint8_t kNonLinearQuantizerScale[32];  // tabela 7-6

// IDCT 8×8 de referência (precisão dupla, arredondamento ao mais próximo),
// em ordem de linhas: in[v*8+u] → out[y*8+x].
void idct(const std::int32_t (&in)[64], std::int32_t (&out)[64]);

}  // namespace anyps2::rt::mpeg
