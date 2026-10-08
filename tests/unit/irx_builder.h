#pragma once

// Monta IRX mínimos para os testes (cabeçalho ELF + PT_SCE_IOPMOD + .iopmod).

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace testutil {

inline void put32(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) {
    if (v.size() < off + 4) v.resize(off + 4);
    for (int i = 0; i < 4; ++i) v[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x >> (8 * i));
}
inline void put16(std::vector<std::uint8_t>& v, std::size_t off, std::uint16_t x) {
    if (v.size() < off + 2) v.resize(off + 2);
    v[off] = static_cast<std::uint8_t>(x);
    v[off + 1] = static_cast<std::uint8_t>(x >> 8);
}

// IRX mínimo: cabeçalho ELF + phdrs (PT_SCE_IOPMOD e um PT_LOAD) + .iopmod.
// Com `name` vazio, o nome vai numa ModuleInfo dentro do segmento carregável
// (como no ps2sdk).
inline std::vector<std::uint8_t> makeIrx(const std::string& iopmodName, const std::string& moduleInfoName,
                                         std::uint16_t version) {
    std::vector<std::uint8_t> d(52, 0);
    d[0] = 0x7F;
    d[1] = 'E';
    d[2] = 'L';
    d[3] = 'F';
    d[4] = 1;  // 32 bits
    d[5] = 1;  // little-endian
    put16(d, 16, 0xFF80);  // ET_SCE_IOPRELEXEC
    put16(d, 18, 8);       // MIPS
    put32(d, 28, 52);      // phoff
    put16(d, 42, 32);      // phentsize
    put16(d, 44, 2);       // phnum
    const std::uint32_t iopmodOff = 52 + 64, loadOff = 0x100;
    d.resize(loadOff + 0x40, 0);
    // PT_SCE_IOPMOD
    put32(d, 52, 0x70000080u);
    put32(d, 56, iopmodOff);
    // PT_LOAD: vaddr 0 no arquivo em loadOff
    put32(d, 84, 1);
    put32(d, 88, loadOff);
    put32(d, 92, 0);
    put32(d, 100, 0x40);
    // .iopmod: moduleinfo (0x10 no segmento), entry, gp, text, data, bss, version, name
    put32(d, iopmodOff, 0x10);
    put16(d, iopmodOff + 24, iopmodName.empty() ? 0xFFFF : version);
    std::memcpy(d.data() + iopmodOff + 26, iopmodName.c_str(), iopmodName.size() + 1);
    // ModuleInfo {const char* name = 0x20; u16 version}
    put32(d, loadOff + 0x10, 0x20);
    put16(d, loadOff + 0x14, version);
    std::memcpy(d.data() + loadOff + 0x20, moduleInfoName.c_str(), moduleInfoName.size() + 1);
    return d;
}

}  // namespace testutil
