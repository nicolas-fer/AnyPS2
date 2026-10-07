#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "anyps2/common/error.h"

namespace anyps2::elf {

// Erro de formato/validação de ELF. A mensagem sempre identifica o arquivo
// e o campo/offset problemático.
class ElfError : public anyps2::Error {
public:
    using anyps2::Error::Error;
};

// ---- Constantes do formato ELF32 que usamos --------------------------------
inline constexpr std::uint16_t ET_REL = 1;
inline constexpr std::uint16_t ET_EXEC = 2;
inline constexpr std::uint16_t ET_DYN = 3;
inline constexpr std::uint16_t ET_SCE_IOPRELEXEC = 0xFF80;  // módulo IRX do IOP
inline constexpr std::uint16_t EM_MIPS = 8;

inline constexpr std::uint32_t PT_NULL = 0;
inline constexpr std::uint32_t PT_LOAD = 1;
inline constexpr std::uint32_t PT_SCE_IOPMOD = 0x70000080;  // cabeçalho .iopmod do IRX

inline constexpr std::uint32_t PF_X = 1, PF_W = 2, PF_R = 4;

inline constexpr std::uint32_t SHT_NULL = 0;
inline constexpr std::uint32_t SHT_PROGBITS = 1;
inline constexpr std::uint32_t SHT_SYMTAB = 2;
inline constexpr std::uint32_t SHT_STRTAB = 3;
inline constexpr std::uint32_t SHT_RELA = 4;
inline constexpr std::uint32_t SHT_NOBITS = 8;
inline constexpr std::uint32_t SHT_REL = 9;

inline constexpr std::uint32_t SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4;

inline constexpr std::uint8_t STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3,
                              STT_FILE = 4;
inline constexpr std::uint8_t STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2;

inline constexpr std::uint16_t SHN_UNDEF = 0, SHN_ABS = 0xFFF1, SHN_COMMON = 0xFFF2;

// e_flags do MIPS
inline constexpr std::uint32_t EF_MIPS_MACH = 0x00FF0000;
inline constexpr std::uint32_t E_MIPS_MACH_5900 = 0x00920000;

// Tipos de relocação MIPS mais comuns (usados por módulos IRX).
inline constexpr std::uint32_t R_MIPS_NONE = 0, R_MIPS_16 = 1, R_MIPS_32 = 2, R_MIPS_REL32 = 3,
                               R_MIPS_26 = 4, R_MIPS_HI16 = 5, R_MIPS_LO16 = 6,
                               R_MIPS_GPREL16 = 7;

struct Segment {
    std::uint32_t index = 0;
    std::uint32_t type = 0;
    std::uint32_t offset = 0;
    std::uint32_t vaddr = 0;
    std::uint32_t paddr = 0;
    std::uint32_t filesz = 0;
    std::uint32_t memsz = 0;
    std::uint32_t flags = 0;
    std::uint32_t align = 0;

    bool isLoad() const { return type == PT_LOAD; }
    bool executable() const { return (flags & PF_X) != 0; }
};

struct Section {
    std::uint32_t index = 0;
    std::string name;
    std::uint32_t type = 0;
    std::uint32_t flags = 0;
    std::uint32_t addr = 0;
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    std::uint32_t link = 0;
    std::uint32_t info = 0;
    std::uint32_t addralign = 0;
    std::uint32_t entsize = 0;

    bool isAlloc() const { return (flags & SHF_ALLOC) != 0; }
    bool isExec() const { return (flags & SHF_EXECINSTR) != 0; }
    bool hasFileData() const { return type != SHT_NOBITS && type != SHT_NULL; }
};

struct Symbol {
    std::string name;
    std::uint32_t value = 0;
    std::uint32_t size = 0;
    std::uint8_t type = STT_NOTYPE;
    std::uint8_t bind = STB_LOCAL;
    std::uint8_t other = 0;
    std::uint16_t sectionIndex = SHN_UNDEF;

    bool isFunction() const { return type == STT_FUNC; }
    bool isDefined() const { return sectionIndex != SHN_UNDEF; }
};

struct Relocation {
    std::uint32_t offset = 0;        // endereço/offset a corrigir
    std::uint32_t type = 0;          // R_MIPS_*
    std::uint32_t symbolIndex = 0;   // índice na tabela de símbolos ligada
    std::int32_t addend = 0;         // só significativo em RELA
    bool hasAddend = false;          // true para SHT_RELA
    std::uint32_t targetSection = 0; // seção onde a relocação se aplica (sh_info)
    std::uint32_t relocSection = 0;  // seção .rel/.rela de origem
};

class ElfFile {
public:
    // Carrega e valida. Lança ElfError com mensagem descritiva.
    static ElfFile loadFromFile(const std::filesystem::path& path);
    static ElfFile loadFromMemory(std::vector<std::uint8_t> data, std::string name = "<memória>");

    // ---- Cabeçalho -------------------------------------------------------
    const std::string& name() const { return name_; }
    std::uint16_t type() const { return type_; }
    std::uint16_t machine() const { return machine_; }
    std::uint32_t entry() const { return entry_; }
    std::uint32_t flags() const { return flags_; }
    bool isR5900() const { return (flags_ & EF_MIPS_MACH) == E_MIPS_MACH_5900; }
    bool isIopModule() const { return type_ == ET_SCE_IOPRELEXEC; }
    static std::string_view typeName(std::uint16_t type);

    // ---- Tabelas ---------------------------------------------------------
    const std::vector<Segment>& segments() const { return segments_; }
    const std::vector<Section>& sections() const { return sections_; }
    const std::vector<Symbol>& symbols() const { return symbols_; }
    const std::vector<Relocation>& relocations() const { return relocations_; }

    const Section* findSection(std::string_view sectionName) const;
    const Symbol* findSymbol(std::string_view symbolName) const;

    // Símbolo de função (ou objeto) que contém o endereço, preferindo
    // símbolos globais e com tamanho. nullptr se nenhum.
    const Symbol* symbolContaining(std::uint32_t address) const;
    // Símbolo cujo valor é exatamente o endereço.
    const Symbol* symbolAt(std::uint32_t address) const;

    // Conteúdo bruto no arquivo (vazio para NOBITS).
    std::span<const std::uint8_t> sectionData(const Section& section) const;
    std::span<const std::uint8_t> segmentData(const Segment& segment) const;
    std::span<const std::uint8_t> bytes() const { return data_; }

    // ---- Visão da memória virtual (segmentos PT_LOAD) ---------------------
    // Um endereço é "mapeado" se cai em [vaddr, vaddr+memsz) de algum PT_LOAD.
    // A parte entre filesz e memsz (bss) lê como zero.
    bool isMapped(std::uint32_t vaddr, std::uint32_t size = 1) const;
    bool isExecutable(std::uint32_t vaddr) const;
    std::optional<std::uint8_t> tryRead8(std::uint32_t vaddr) const;
    std::optional<std::uint32_t> tryRead32(std::uint32_t vaddr) const;
    // Versões que lançam ElfError("endereço 0x... não mapeado").
    std::uint8_t read8(std::uint32_t vaddr) const;
    std::uint32_t read32(std::uint32_t vaddr) const;
    // Copia [vaddr, vaddr+size) para um buffer, com zeros no bss.
    std::vector<std::uint8_t> readRange(std::uint32_t vaddr, std::uint32_t size) const;

    // Faixa total ocupada pelos PT_LOAD (menor vaddr, maior vaddr+memsz).
    std::uint32_t loadBase() const;
    std::uint32_t loadEnd() const;

private:
    ElfFile() = default;
    void parse();
    void parseHeader();
    void parseSegments();
    void parseSections();
    void parseSymbols();
    void parseRelocations();
    [[noreturn]] void fail(const std::string& message) const;
    void requireRange(std::uint64_t offset, std::uint64_t size, const std::string& what) const;
    std::string readString(const Section& strtab, std::uint32_t offset, const std::string& what) const;
    const Segment* loadSegmentFor(std::uint32_t vaddr) const;

    std::string name_;
    std::vector<std::uint8_t> data_;

    std::uint16_t type_ = 0;
    std::uint16_t machine_ = 0;
    std::uint32_t entry_ = 0;
    std::uint32_t flags_ = 0;
    std::uint32_t phoff_ = 0;
    std::uint32_t shoff_ = 0;
    std::uint16_t phnum_ = 0;
    std::uint16_t shnum_ = 0;
    std::uint16_t shstrndx_ = 0;

    std::vector<Segment> segments_;
    std::vector<Section> sections_;
    std::vector<Symbol> symbols_;
    std::vector<Relocation> relocations_;
};

}  // namespace anyps2::elf
