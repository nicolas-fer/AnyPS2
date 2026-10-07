#pragma once

// Construtor de ELF32 little-endian MIPS para testes. Gera arquivos
// sintéticos válidos que os testes depois corrompem campo a campo.

#include <cstdint>
#include <string>
#include <vector>

#include "anyps2/common/bytes.h"
#include "anyps2/elf/elf_file.h"

namespace testutil {

struct BuildSection {
    std::string name;
    std::uint32_t type = anyps2::elf::SHT_PROGBITS;
    std::uint32_t flags = 0;
    std::uint32_t addr = 0;
    std::vector<std::uint8_t> data;  // para NOBITS, só o tamanho importa
    std::uint32_t nobitsSize = 0;
    std::uint32_t link = 0;
    std::uint32_t info = 0;
    std::uint32_t entsize = 0;
    // preenchidos por build():
    std::uint32_t offset = 0;
};

struct BuildSymbol {
    std::string name;
    std::uint32_t value = 0;
    std::uint32_t size = 0;
    std::uint8_t type = anyps2::elf::STT_FUNC;
    std::uint8_t bind = anyps2::elf::STB_GLOBAL;
    std::uint16_t shndx = 1;
};

struct BuildSegment {
    std::uint32_t type = anyps2::elf::PT_LOAD;
    std::string section;      // seção cujo offset/tamanho alimenta o segmento
    std::uint32_t vaddr = 0;
    std::uint32_t memsz = 0;  // 0 = igual ao filesz
    std::uint32_t flags = anyps2::elf::PF_R | anyps2::elf::PF_X;
};

struct BuildRel {
    std::uint32_t offset;
    std::uint32_t type;
    std::uint32_t sym;
};

class ElfBuilder {
public:
    std::uint16_t type = anyps2::elf::ET_EXEC;
    std::uint32_t entry = 0;
    std::uint32_t flags = anyps2::elf::E_MIPS_MACH_5900;
    std::vector<BuildSection> sections;
    std::vector<BuildSymbol> symbols;
    std::vector<BuildSegment> segments;
    std::vector<BuildRel> rels;     // aplicados a .text (gera .rel.text)
    std::string relTarget = ".text";

    // Offsets úteis do arquivo gerado (para corromper nos testes).
    std::uint32_t phoff = 0, shoff = 0, symtabOffset = 0;

    std::size_t addSection(BuildSection s) {
        sections.push_back(std::move(s));
        return sections.size();  // índice ELF (seção 0 é a nula)
    }

    static std::vector<std::uint8_t> words(std::initializer_list<std::uint32_t> ws) {
        std::vector<std::uint8_t> out(ws.size() * 4);
        std::size_t i = 0;
        for (auto w : ws) anyps2::writeLE32(out, 4 * i++, w);
        return out;
    }

    std::vector<std::uint8_t> build() {
        using namespace anyps2::elf;
        std::vector<BuildSection> all = sections;

        // .symtab / .strtab
        std::string strtab(1, '\0');
        std::vector<std::uint8_t> symtab(16, 0);
        for (const auto& s : symbols) {
            const std::uint32_t nameOff = static_cast<std::uint32_t>(strtab.size());
            strtab += s.name;
            strtab += '\0';
            std::vector<std::uint8_t> e(16, 0);
            anyps2::writeLE32(e, 0, nameOff);
            anyps2::writeLE32(e, 4, s.value);
            anyps2::writeLE32(e, 8, s.size);
            e[12] = static_cast<std::uint8_t>((s.bind << 4) | s.type);
            anyps2::writeLE16(e, 14, s.shndx);
            symtab.insert(symtab.end(), e.begin(), e.end());
        }
        const std::uint32_t symtabIdx = static_cast<std::uint32_t>(all.size() + 1);
        if (!rels.empty()) {
            std::uint32_t target = 0;
            for (std::size_t i = 0; i < all.size(); ++i)
                if (all[i].name == relTarget) target = static_cast<std::uint32_t>(i + 1);
            std::vector<std::uint8_t> rel;
            for (const auto& r : rels) {
                std::vector<std::uint8_t> e(8, 0);
                anyps2::writeLE32(e, 0, r.offset);
                anyps2::writeLE32(e, 4, (r.sym << 8) | r.type);
                rel.insert(rel.end(), e.begin(), e.end());
            }
            BuildSection rs{".rel" + relTarget, SHT_REL, 0, 0, rel, 0, symtabIdx + 0, target, 8};
            all.push_back(rs);
        }
        const std::uint32_t realSymtabIdx = static_cast<std::uint32_t>(all.size() + 1);
        // Corrige o sh_link das relocações se a .rel foi inserida antes.
        for (auto& s : all)
            if (s.type == SHT_REL) s.link = realSymtabIdx;
        all.push_back({".symtab", SHT_SYMTAB, 0, 0, symtab, 0, realSymtabIdx + 1, 1, 16});
        all.push_back({".strtab", SHT_STRTAB, 0, 0,
                       std::vector<std::uint8_t>(strtab.begin(), strtab.end()), 0, 0, 0, 0});
        // .shstrtab
        std::string shstr(1, '\0');
        std::vector<std::uint32_t> nameOffs;
        for (const auto& s : all) {
            nameOffs.push_back(static_cast<std::uint32_t>(shstr.size()));
            shstr += s.name;
            shstr += '\0';
        }
        nameOffs.push_back(static_cast<std::uint32_t>(shstr.size()));
        shstr += ".shstrtab";
        shstr += '\0';
        all.push_back({".shstrtab", SHT_STRTAB, 0, 0,
                       std::vector<std::uint8_t>(shstr.begin(), shstr.end()), 0, 0, 0, 0});
        const std::uint16_t shstrndx = static_cast<std::uint16_t>(all.size());

        // Layout: ehdr | phdrs | dados das seções (alinhados a 16) | shdrs
        std::vector<std::uint8_t> out(52, 0);
        phoff = segments.empty() ? 0 : 52;
        out.resize(52 + segments.size() * 32, 0);
        for (auto& s : all) {
            while (out.size() % 16) out.push_back(0);
            s.offset = static_cast<std::uint32_t>(out.size());
            if (s.type != SHT_NOBITS) out.insert(out.end(), s.data.begin(), s.data.end());
            if (s.name == ".symtab") symtabOffset = s.offset;
        }
        while (out.size() % 4) out.push_back(0);
        shoff = static_cast<std::uint32_t>(out.size());
        out.resize(out.size() + (all.size() + 1) * 40, 0);
        for (std::size_t i = 0; i < all.size(); ++i) {
            const auto& s = all[i];
            const std::size_t b = shoff + (i + 1) * 40;
            anyps2::writeLE32(out, b + 0, nameOffs[i]);
            anyps2::writeLE32(out, b + 4, s.type);
            anyps2::writeLE32(out, b + 8, s.flags);
            anyps2::writeLE32(out, b + 12, s.addr);
            anyps2::writeLE32(out, b + 16, s.offset);
            anyps2::writeLE32(out, b + 20,
                              s.type == SHT_NOBITS ? s.nobitsSize
                                                   : static_cast<std::uint32_t>(s.data.size()));
            anyps2::writeLE32(out, b + 24, s.link);
            anyps2::writeLE32(out, b + 28, s.info);
            anyps2::writeLE32(out, b + 32, 4);
            anyps2::writeLE32(out, b + 36, s.entsize);
        }
        for (std::size_t i = 0; i < segments.size(); ++i) {
            const auto& g = segments[i];
            std::uint32_t off = 0, filesz = 0;
            for (const auto& s : all) {
                if (s.name == g.section) {
                    off = s.offset;
                    filesz = s.type == SHT_NOBITS ? 0 : static_cast<std::uint32_t>(s.data.size());
                }
            }
            const std::size_t b = phoff + i * 32;
            anyps2::writeLE32(out, b + 0, g.type);
            anyps2::writeLE32(out, b + 4, off);
            anyps2::writeLE32(out, b + 8, g.vaddr);
            anyps2::writeLE32(out, b + 12, g.vaddr);
            anyps2::writeLE32(out, b + 16, filesz);
            anyps2::writeLE32(out, b + 20, g.memsz ? g.memsz : filesz);
            anyps2::writeLE32(out, b + 24, g.flags);
            anyps2::writeLE32(out, b + 28, 16);
        }
        // ELF header
        out[0] = 0x7F;
        out[1] = 'E';
        out[2] = 'L';
        out[3] = 'F';
        out[4] = 1;  // ELFCLASS32
        out[5] = 1;  // little-endian
        out[6] = 1;  // EV_CURRENT
        anyps2::writeLE16(out, 16, type);
        anyps2::writeLE16(out, 18, EM_MIPS);
        anyps2::writeLE32(out, 20, 1);
        anyps2::writeLE32(out, 24, entry);
        anyps2::writeLE32(out, 28, phoff);
        anyps2::writeLE32(out, 32, shoff);
        anyps2::writeLE32(out, 36, flags);
        anyps2::writeLE16(out, 40, 52);
        anyps2::writeLE16(out, 42, 32);
        anyps2::writeLE16(out, 44, static_cast<std::uint16_t>(segments.size()));
        anyps2::writeLE16(out, 46, 40);
        anyps2::writeLE16(out, 48, static_cast<std::uint16_t>(all.size() + 1));
        anyps2::writeLE16(out, 50, shstrndx);
        return out;
    }
};

// Um executável típico do EE: .text em 0x00100000, .data e .bss.
inline ElfBuilder typicalExecutable() {
    using namespace anyps2::elf;
    ElfBuilder b;
    b.entry = 0x00100000;
    b.addSection({".text", SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, 0x00100000,
                  ElfBuilder::words({0x27BDFFF0, 0x0C040008, 0x00000000, 0x03E00008,
                                     0x27BD0010, 0x00000000, 0x00000000, 0x00000000,
                                     0x03E00008, 0x24020007})});
    b.addSection({".data", SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, 0x00100100,
                  ElfBuilder::words({0xDEADBEEF, 0x01234567})});
    b.addSection({".bss", SHT_NOBITS, SHF_ALLOC | SHF_WRITE, 0x00100108, {}, 0x100});
    b.symbols = {
        {"_start", 0x00100000, 0x14, STT_FUNC, STB_GLOBAL, 1},
        {"helper", 0x00100020, 0x08, STT_FUNC, STB_LOCAL, 1},
        {"helper_alias", 0x00100020, 0x08, STT_FUNC, STB_GLOBAL, 1},
        {"table", 0x00100100, 0x08, STT_OBJECT, STB_GLOBAL, 2},
        {"buffer", 0x00100108, 0x100, STT_OBJECT, STB_GLOBAL, 3},
        {"external", 0, 0, STT_NOTYPE, STB_GLOBAL, SHN_UNDEF},
    };
    b.segments = {
        {PT_LOAD, ".text", 0x00100000, 0, PF_R | PF_X},
        {PT_LOAD, ".data", 0x00100100, 0x108, PF_R | PF_W},  // .data + .bss
    };
    return b;
}

}  // namespace testutil
