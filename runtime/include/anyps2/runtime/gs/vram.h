#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace anyps2::rt::gs {

// Formatos de pixel do GS (campo PSM dos registradores).
enum Psm : std::uint32_t {
    PSMCT32 = 0x00,
    PSMCT24 = 0x01,
    PSMCT16 = 0x02,
    PSMCT16S = 0x0A,
    PSMT8 = 0x13,
    PSMT4 = 0x14,
    PSMT8H = 0x1B,
    PSMT4HL = 0x24,
    PSMT4HH = 0x2C,
    PSMZ32 = 0x30,
    PSMZ24 = 0x31,
    PSMZ16 = 0x32,
    PSMZ16S = 0x3A,
};

bool isValidPsm(std::uint32_t psm);
std::string psmName(std::uint32_t psm);
// Bits por pixel na memória (32, 16, 8 ou 4). PSMCT24/PSMZ24/PSMT8H/PSMT4Hx
// ocupam o layout de 32 bits.
unsigned psmStorageBits(std::uint32_t psm);
// Bits por pixel nos dados de transferência HOST→LOCAL (24 para PSMCT24/Z24).
unsigned psmTransferBits(std::uint32_t psm);

// Memória local do GS: 4 MB, organizada em páginas de 8 KB, blocos de 256
// bytes e colunas de 64 bytes, com um "swizzle" diferente por formato. Todo
// acesso por (x, y) passa pelas mesmas tabelas que o hardware usa, então
// ler um buffer com um formato diferente do que foi escrito (truques comuns
// em jogos, como reinterpretar PSMCT32 como PSMT8) dá o mesmo resultado que
// no console.
//
// bp é o endereço base em blocos (unidades de 256 bytes, como TBP/CBP/SBP/
// DBP); FBP/ZBP (páginas) devem ser multiplicados por 32. bw é a largura do
// buffer em unidades de 64 pixels (como TBW/FBW).
class Vram {
public:
    static constexpr std::uint32_t kSize = 4u * 1024 * 1024;
    static constexpr std::uint32_t kBlocks = kSize / 256;

    Vram();

    std::uint8_t* data() { return mem_.get(); }
    const std::uint8_t* data() const { return mem_.get(); }

    // Endereço em bytes (32/16/8 bits) do pixel; para 4 bits, endereço em
    // nibbles. Sempre dentro de [0, 4 MB) (o endereçamento dá a volta).
    static std::uint32_t byteAddress32(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y,
                                       bool z = false);
    static std::uint32_t byteAddress16(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y,
                                       std::uint32_t psm);
    static std::uint32_t byteAddress8(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y);
    static std::uint32_t nibbleAddress4(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y);

    // Valor bruto do pixel no formato `psm`: 32 bits (24 bits para os
    // formatos de 24), 16 bits, índice de 8 ou 4 bits.
    std::uint32_t readPixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x,
                            std::uint32_t y) const;
    // Escreve só os bits que o formato ocupa (PSMCT24 preserva o byte alto,
    // PSMT8H escreve só os bits 24–31 etc.).
    void writePixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y,
                    std::uint32_t value);

    std::uint32_t read32(std::uint32_t byteAddr) const;
    void write32(std::uint32_t byteAddr, std::uint32_t v);

private:
    std::unique_ptr<std::uint8_t[]> mem_;
};

}  // namespace anyps2::rt::gs
