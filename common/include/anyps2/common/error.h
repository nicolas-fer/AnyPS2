#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace anyps2 {

// Exceção base do projeto. Toda falha deve explicar *o que* faltou e
// *onde* (endereço, instrução, syscall), nunca falhar em silêncio.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Formata um valor como "0x%0*X" sem depender de iostreams.
inline std::string hex(std::uint64_t value, int minDigits = 8) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    char buf[24];
    int pos = static_cast<int>(sizeof(buf));
    int written = 0;
    do {
        buf[--pos] = kDigits[value & 0xF];
        value >>= 4;
        ++written;
    } while (value != 0 || written < minDigits);
    return "0x" + std::string(buf + pos, buf + sizeof(buf));
}

}  // namespace anyps2
