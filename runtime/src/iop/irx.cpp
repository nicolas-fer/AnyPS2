// Cabeçalho de módulos IRX do IOP: ELF relocável (e_type 0xFF80) com um
// program header PT_SCE_IOPMOD (0x70000080) apontando para:
//   u32 moduleinfo; u32 entry; u32 gp; u32 text, data, bss; u16 version; char name[]
// Módulos da SCE gravam o nome ali; os do ps2sdk deixam o nome vazio e o
// guardam na estrutura ModuleInfo { const char* name; u16 version; } que
// `moduleinfo` aponta (endereços relativos à base 0 do módulo).

#include <cstring>

#include "anyps2/common/bytes.h"
#include "anyps2/runtime/iop/iop.h"

namespace anyps2::rt {

namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

std::optional<IrxInfo> parseIrx(const std::uint8_t* d, std::size_t size) {
    if (size < 52 || d[0] != 0x7F || d[1] != 'E' || d[2] != 'L' || d[3] != 'F') return std::nullopt;
    const std::uint32_t phoff = le32(d + 28);
    const std::uint16_t phentsize = le16(d + 42), phnum = le16(d + 44);
    if (phentsize < 32) return std::nullopt;
    std::uint32_t iopmod = 0;
    bool haveIopmod = false;
    struct Seg {
        std::uint32_t vaddr, offset, filesz;
    };
    std::vector<Seg> segs;
    for (std::uint32_t i = 0; i < phnum; ++i) {
        const std::size_t ph = phoff + std::size_t{i} * phentsize;
        if (ph + 32 > size) return std::nullopt;
        const std::uint32_t type = le32(d + ph);
        if (type == 0x70000080u) {
            iopmod = le32(d + ph + 4);
            haveIopmod = true;
        } else if (type == 1) {
            segs.push_back({le32(d + ph + 8), le32(d + ph + 4), le32(d + ph + 16)});
        }
    }
    if (!haveIopmod || std::size_t{iopmod} + 26 > size) return std::nullopt;
    IrxInfo info;
    info.version = le16(d + iopmod + 24);
    {
        const char* n = reinterpret_cast<const char*>(d + iopmod + 26);
        info.name.assign(n, strnlen(n, size - iopmod - 26));
    }
    if (!info.name.empty()) return info;
    auto toOffset = [&](std::uint32_t addr) -> std::optional<std::size_t> {
        for (const auto& s : segs) {
            if (addr >= s.vaddr && addr - s.vaddr < s.filesz) return std::size_t{s.offset} + (addr - s.vaddr);
        }
        return std::nullopt;
    };
    const auto mi = toOffset(le32(d + iopmod));
    if (!mi || *mi + 6 > size) return info;
    const auto np = toOffset(le32(d + *mi));
    if (!np) return info;
    const char* n = reinterpret_cast<const char*>(d + *np);
    info.name.assign(n, strnlen(n, size - *np));
    info.version = le16(d + *mi + 4);
    return info;
}

}  // namespace anyps2::rt
