#include "anyps2/runtime/gs/vram.h"

#include <cstring>

namespace anyps2::rt::gs {

namespace {

// Posição de um pixel dentro de uma "coluna de 32 bits" (8x2 pixels, 16
// palavras): os pixels pares/ímpares em x se alternam com as duas linhas.
constexpr std::uint32_t idx32(std::uint32_t x, std::uint32_t y) {
    return (x & 1) | ((y & 1) << 1) | ((x & 6) << 1);
}

// Ordem dos blocos numa página para os formatos de "8 colunas x 4 linhas"
// de blocos (PSMCT32 e PSMT8) e de "4 colunas x 8 linhas" (PSMCT16, PSMT4).
constexpr std::uint32_t blockOrder32(std::uint32_t bx, std::uint32_t by) {
    return (bx & 1) | ((by & 1) << 1) | ((bx & 2) << 1) | ((by & 2) << 2) | ((bx & 4) << 2);
}
constexpr std::uint32_t blockOrder16(std::uint32_t bx, std::uint32_t by) {
    return (by & 1) | ((bx & 1) << 1) | ((by & 2) << 1) | ((bx & 2) << 2) | ((by & 4) << 2);
}
constexpr std::uint32_t blockOrder16S(std::uint32_t bx, std::uint32_t by) {
    return (by & 1) | ((bx & 1) << 1) | (by & 4) | ((by & 2) << 2) | ((bx & 2) << 3);
}
// Os formatos de Z usam a mesma ordem com as metades da página trocadas.
constexpr std::uint32_t kZSwap = 0x18;

// Palavra (0..63) de um pixel de 32 bits num bloco 8x8.
constexpr std::uint32_t column32(std::uint32_t x, std::uint32_t y) {
    return (y >> 1) * 16 + idx32(x, y);
}
// Meia-palavra (0..127) de um pixel de 16 bits num bloco 16x8.
constexpr std::uint32_t column16(std::uint32_t x, std::uint32_t y) {
    return ((y >> 1) & 3) * 32 + idx32(x & 7, y) * 2 + ((x >> 3) & 1);
}
// Byte (0..255) de um pixel de 8 bits num bloco 16x16. Nas colunas
// pares as linhas 2–3 têm as metades trocadas; nas ímpares, as linhas 0–1.
constexpr std::uint32_t column8(std::uint32_t x, std::uint32_t y) {
    const std::uint32_t c = y >> 2, y4 = y & 3;
    const std::uint32_t swap = ((y4 >> 1) ^ (c & 1)) ? 4 : 0;
    const std::uint32_t k = (y4 >> 1) + 2 * ((x >> 3) & 1);
    return c * 64 + idx32((x & 7) ^ swap, y4) * 4 + k;
}
// Nibble (0..511) de um pixel de 4 bits num bloco 32x16.
constexpr std::uint32_t column4(std::uint32_t x, std::uint32_t y) {
    const std::uint32_t c = y >> 2, y4 = y & 3;
    const std::uint32_t swap = ((y4 >> 1) ^ (c & 1)) ? 4 : 0;
    const std::uint32_t n = (y4 >> 1) + 2 * ((x >> 3) & 3);
    return c * 128 + idx32((x & 7) ^ swap, y4) * 8 + n;
}

}  // namespace

bool isValidPsm(std::uint32_t psm) {
    switch (psm) {
        case PSMCT32: case PSMCT24: case PSMCT16: case PSMCT16S: case PSMT8: case PSMT4: case PSMT8H:
        case PSMT4HL: case PSMT4HH: case PSMZ32: case PSMZ24: case PSMZ16: case PSMZ16S:
            return true;
        default:
            return false;
    }
}

std::string psmName(std::uint32_t psm) {
    switch (psm) {
        case PSMCT32: return "PSMCT32";
        case PSMCT24: return "PSMCT24";
        case PSMCT16: return "PSMCT16";
        case PSMCT16S: return "PSMCT16S";
        case PSMT8: return "PSMT8";
        case PSMT4: return "PSMT4";
        case PSMT8H: return "PSMT8H";
        case PSMT4HL: return "PSMT4HL";
        case PSMT4HH: return "PSMT4HH";
        case PSMZ32: return "PSMZ32";
        case PSMZ24: return "PSMZ24";
        case PSMZ16: return "PSMZ16";
        case PSMZ16S: return "PSMZ16S";
        default: return "PSM inválido 0x" + std::to_string(psm);
    }
}

unsigned psmStorageBits(std::uint32_t psm) {
    switch (psm) {
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
        case PSMT8: return 8;
        case PSMT4: return 4;
        default: return 32;
    }
}

unsigned psmTransferBits(std::uint32_t psm) {
    switch (psm) {
        case PSMCT24: case PSMZ24: return 24;
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
        case PSMT8: case PSMT8H: return 8;
        case PSMT4: case PSMT4HL: case PSMT4HH: return 4;
        default: return 32;
    }
}

Vram::Vram() : mem_(new std::uint8_t[kSize]()) {}

std::uint32_t Vram::byteAddress32(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y, bool z) {
    x &= 2047;
    y &= 2047;
    std::uint32_t order = blockOrder32((x >> 3) & 7, (y >> 3) & 3);
    if (z) order ^= kZSwap;
    const std::uint32_t block = bp + (y >> 5) * bw * 32 + (x >> 6) * 32 + order;
    return (block % kBlocks) * 256 + column32(x & 7, y & 7) * 4;
}

std::uint32_t Vram::byteAddress16(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y,
                                  std::uint32_t psm) {
    x &= 2047;
    y &= 2047;
    const std::uint32_t bx = (x >> 4) & 3, by = (y >> 3) & 7;
    std::uint32_t order = 0;
    switch (psm) {
        case PSMCT16S: order = blockOrder16S(bx, by); break;
        case PSMZ16: order = blockOrder16(bx, by) ^ kZSwap; break;
        case PSMZ16S: order = blockOrder16S(bx, by) ^ kZSwap; break;
        default: order = blockOrder16(bx, by); break;
    }
    const std::uint32_t block = bp + (y >> 6) * bw * 32 + (x >> 6) * 32 + order;
    return (block % kBlocks) * 256 + column16(x & 15, y & 7) * 2;
}

std::uint32_t Vram::byteAddress8(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y) {
    x &= 2047;
    y &= 2047;
    const std::uint32_t block = bp + (y >> 6) * (bw >> 1) * 32 + (x >> 7) * 32 + blockOrder32((x >> 4) & 7, (y >> 4) & 3);
    return (block % kBlocks) * 256 + column8(x & 15, y & 15);
}

std::uint32_t Vram::nibbleAddress4(std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y) {
    x &= 2047;
    y &= 2047;
    const std::uint32_t block = bp + (y >> 7) * (bw >> 1) * 32 + (x >> 7) * 32 + blockOrder16((x >> 5) & 3, (y >> 4) & 7);
    return (block % kBlocks) * 512 + column4(x & 31, y & 15);
}

std::uint32_t Vram::read32(std::uint32_t a) const {
    std::uint32_t v;
    std::memcpy(&v, mem_.get() + (a & (kSize - 4)), 4);
    return v;
}

void Vram::write32(std::uint32_t a, std::uint32_t v) {
    std::memcpy(mem_.get() + (a & (kSize - 4)), &v, 4);
}

std::uint32_t Vram::readPixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x,
                              std::uint32_t y) const {
    switch (psm) {
        case PSMCT32: case PSMZ32: return read32(byteAddress32(bp, bw, x, y, psm == PSMZ32));
        case PSMCT24: case PSMZ24: return read32(byteAddress32(bp, bw, x, y, psm == PSMZ24)) & 0xFFFFFFu;
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: {
            const std::uint32_t a = byteAddress16(bp, bw, x, y, psm);
            return static_cast<std::uint32_t>(mem_[a]) | (static_cast<std::uint32_t>(mem_[a + 1]) << 8);
        }
        case PSMT8: return mem_[byteAddress8(bp, bw, x, y)];
        case PSMT4: {
            const std::uint32_t n = nibbleAddress4(bp, bw, x, y);
            return (mem_[n >> 1] >> ((n & 1) * 4)) & 0xF;
        }
        case PSMT8H: return read32(byteAddress32(bp, bw, x, y)) >> 24;
        case PSMT4HL: return (read32(byteAddress32(bp, bw, x, y)) >> 24) & 0xF;
        case PSMT4HH: return read32(byteAddress32(bp, bw, x, y)) >> 28;
        default: return 0;
    }
}

void Vram::writePixel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x, std::uint32_t y,
                      std::uint32_t v) {
    switch (psm) {
        case PSMCT32: case PSMZ32:
            write32(byteAddress32(bp, bw, x, y, psm == PSMZ32), v);
            return;
        case PSMCT24: case PSMZ24: {
            const std::uint32_t a = byteAddress32(bp, bw, x, y, psm == PSMZ24);
            write32(a, (read32(a) & 0xFF000000u) | (v & 0xFFFFFFu));
            return;
        }
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: {
            const std::uint32_t a = byteAddress16(bp, bw, x, y, psm);
            mem_[a] = static_cast<std::uint8_t>(v);
            mem_[a + 1] = static_cast<std::uint8_t>(v >> 8);
            return;
        }
        case PSMT8:
            mem_[byteAddress8(bp, bw, x, y)] = static_cast<std::uint8_t>(v);
            return;
        case PSMT4: {
            const std::uint32_t n = nibbleAddress4(bp, bw, x, y);
            std::uint8_t& b = mem_[n >> 1];
            const unsigned shift = (n & 1) * 4;
            b = static_cast<std::uint8_t>((b & ~(0xFu << shift)) | ((v & 0xFu) << shift));
            return;
        }
        case PSMT8H: {
            const std::uint32_t a = byteAddress32(bp, bw, x, y);
            write32(a, (read32(a) & 0x00FFFFFFu) | (v << 24));
            return;
        }
        case PSMT4HL: {
            const std::uint32_t a = byteAddress32(bp, bw, x, y);
            write32(a, (read32(a) & 0xF0FFFFFFu) | ((v & 0xF) << 24));
            return;
        }
        case PSMT4HH: {
            const std::uint32_t a = byteAddress32(bp, bw, x, y);
            write32(a, (read32(a) & 0x0FFFFFFFu) | ((v & 0xF) << 28));
            return;
        }
        default:
            return;
    }
}

}  // namespace anyps2::rt::gs
