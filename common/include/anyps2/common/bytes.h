#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace anyps2 {

// Leitura little-endian portátil (independe do endianness do host e de
// alinhamento). O PS2 é little-endian, assim como x86-64 e ARM64 em uso
// comum, mas não dependemos disso.
inline std::uint16_t readLE16(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset] | (data[offset + 1] << 8));
}

inline std::uint32_t readLE32(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) |
           (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(data[offset + 3]) << 24);
}

inline void writeLE16(std::span<std::uint8_t> data, std::size_t offset, std::uint16_t v) {
    data[offset] = static_cast<std::uint8_t>(v);
    data[offset + 1] = static_cast<std::uint8_t>(v >> 8);
}

inline void writeLE32(std::span<std::uint8_t> data, std::size_t offset, std::uint32_t v) {
    data[offset] = static_cast<std::uint8_t>(v);
    data[offset + 1] = static_cast<std::uint8_t>(v >> 8);
    data[offset + 2] = static_cast<std::uint8_t>(v >> 16);
    data[offset + 3] = static_cast<std::uint8_t>(v >> 24);
}

}  // namespace anyps2
