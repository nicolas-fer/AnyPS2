#include "anyps2/elf/elf_file.h"

#include <algorithm>
#include <fstream>
#include <limits>

#include "anyps2/common/bytes.h"

namespace anyps2::elf {
namespace {

constexpr std::size_t kEhdrSize = 52;
constexpr std::size_t kPhdrSize = 32;
constexpr std::size_t kShdrSize = 40;
constexpr std::size_t kSymSize = 16;
constexpr std::size_t kRelSize = 8;
constexpr std::size_t kRelaSize = 12;
constexpr std::uint16_t SHN_XINDEX = 0xFFFF;
constexpr std::uint16_t SHN_LORESERVE = 0xFF00;

}  // namespace

// ---------------------------------------------------------------------------
// Carregamento
// ---------------------------------------------------------------------------

ElfFile ElfFile::loadFromFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw ElfError("não foi possível abrir o arquivo ELF '" + path.string() + "'");
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) throw ElfError("não foi possível obter o tamanho de '" + path.string() + "'");
    if (static_cast<std::uint64_t>(size) > std::numeric_limits<std::uint32_t>::max()) {
        throw ElfError("'" + path.string() + "' é grande demais para um ELF32");
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), size)) {
        throw ElfError("falha ao ler '" + path.string() + "'");
    }
    return loadFromMemory(std::move(data), path.string());
}

ElfFile ElfFile::loadFromMemory(std::vector<std::uint8_t> data, std::string name) {
    ElfFile f;
    f.name_ = std::move(name);
    f.data_ = std::move(data);
    f.parse();
    return f;
}

void ElfFile::parse() {
    parseHeader();
    parseSegments();
    parseSections();
    parseSymbols();
    parseRelocations();
}

void ElfFile::fail(const std::string& message) const {
    throw ElfError(name_ + ": " + message);
}

void ElfFile::requireRange(std::uint64_t offset, std::uint64_t size, const std::string& what) const {
    if (offset > data_.size() || size > data_.size() - offset) {
        fail(what + " fora do arquivo (offset " + hex(offset) + ", tamanho " + hex(size) +
             ", arquivo tem " + hex(data_.size()) + " bytes)");
    }
}

// ---------------------------------------------------------------------------
// Cabeçalho
// ---------------------------------------------------------------------------

void ElfFile::parseHeader() {
    if (data_.size() < 16) fail("arquivo pequeno demais para ser um ELF");
    if (data_[0] != 0x7F || data_[1] != 'E' || data_[2] != 'L' || data_[3] != 'F') {
        fail("assinatura ELF ausente (esperado 7F 45 4C 46)");
    }
    if (data_[4] == 2) fail("ELF de 64 bits não é suportado (executáveis do PS2 são ELF32)");
    if (data_[4] != 1) fail("classe ELF desconhecida " + std::to_string(data_[4]));
    if (data_[5] == 2) fail("ELF big-endian não é suportado (o PS2 é little-endian)");
    if (data_[5] != 1) fail("codificação de dados ELF desconhecida " + std::to_string(data_[5]));
    if (data_[6] != 1) fail("versão de e_ident desconhecida " + std::to_string(data_[6]));
    if (data_.size() < kEhdrSize) fail("cabeçalho ELF truncado");

    std::span<const std::uint8_t> d(data_);
    type_ = readLE16(d, 16);
    machine_ = readLE16(d, 18);
    const std::uint32_t version = readLE32(d, 20);
    entry_ = readLE32(d, 24);
    phoff_ = readLE32(d, 28);
    shoff_ = readLE32(d, 32);
    flags_ = readLE32(d, 36);
    const std::uint16_t ehsize = readLE16(d, 40);
    const std::uint16_t phentsize = readLE16(d, 42);
    phnum_ = readLE16(d, 44);
    const std::uint16_t shentsize = readLE16(d, 46);
    shnum_ = readLE16(d, 48);
    shstrndx_ = readLE16(d, 50);

    if (version != 1) fail("e_version inválido " + std::to_string(version));
    if (machine_ != EM_MIPS) {
        fail("e_machine = " + std::to_string(machine_) + ", esperado " + std::to_string(EM_MIPS) +
             " (MIPS)");
    }
    if (type_ != ET_EXEC && type_ != ET_REL && type_ != ET_DYN && type_ != ET_SCE_IOPRELEXEC) {
        fail("tipo de ELF não suportado " + hex(type_, 4));
    }
    if (ehsize < kEhdrSize) fail("e_ehsize pequeno demais (" + std::to_string(ehsize) + ")");
    if (phnum_ != 0 && phentsize != kPhdrSize) {
        fail("e_phentsize = " + std::to_string(phentsize) + ", esperado 32");
    }
    if ((shnum_ != 0 || shoff_ != 0) && shentsize != kShdrSize) {
        fail("e_shentsize = " + std::to_string(shentsize) + ", esperado 40");
    }

    // Numeração estendida: e_shnum/e_shstrndx podem estar na seção 0.
    if (shoff_ != 0 && (shnum_ == 0 || shstrndx_ == SHN_XINDEX)) {
        requireRange(shoff_, kShdrSize, "cabeçalho de seção 0");
        if (shnum_ == 0) {
            const std::uint32_t realCount = readLE32(d, shoff_ + 20);
            if (realCount > 0xFFFF) fail("número de seções grande demais");
            shnum_ = static_cast<std::uint16_t>(realCount);
        }
        if (shstrndx_ == SHN_XINDEX) {
            const std::uint32_t realIdx = readLE32(d, shoff_ + 24);
            if (realIdx > 0xFFFF) fail("e_shstrndx estendido inválido");
            shstrndx_ = static_cast<std::uint16_t>(realIdx);
        }
    }
}

std::string_view ElfFile::typeName(std::uint16_t type) {
    switch (type) {
        case ET_REL: return "REL (relocável)";
        case ET_EXEC: return "EXEC (executável)";
        case ET_DYN: return "DYN (objeto compartilhado)";
        case ET_SCE_IOPRELEXEC: return "SCE_IOPRELEXEC (módulo IRX)";
        default: return "desconhecido";
    }
}

// ---------------------------------------------------------------------------
// Segmentos
// ---------------------------------------------------------------------------

void ElfFile::parseSegments() {
    if (phnum_ == 0) return;
    requireRange(phoff_, std::uint64_t{phnum_} * kPhdrSize, "tabela de program headers");
    std::span<const std::uint8_t> d(data_);
    segments_.reserve(phnum_);
    for (std::uint32_t i = 0; i < phnum_; ++i) {
        const std::size_t base = phoff_ + i * kPhdrSize;
        Segment s;
        s.index = i;
        s.type = readLE32(d, base + 0);
        s.offset = readLE32(d, base + 4);
        s.vaddr = readLE32(d, base + 8);
        s.paddr = readLE32(d, base + 12);
        s.filesz = readLE32(d, base + 16);
        s.memsz = readLE32(d, base + 20);
        s.flags = readLE32(d, base + 24);
        s.align = readLE32(d, base + 28);

        const std::string what = "segmento " + std::to_string(i);
        if (s.filesz > 0) requireRange(s.offset, s.filesz, what);
        if (s.type == PT_LOAD) {
            if (s.filesz > s.memsz) {
                fail(what + ": p_filesz (" + hex(s.filesz) + ") maior que p_memsz (" +
                     hex(s.memsz) + ")");
            }
            if (std::uint64_t{s.vaddr} + s.memsz > 0x100000000ull) {
                fail(what + ": ultrapassa o espaço de endereçamento de 32 bits");
            }
        }
        segments_.push_back(s);
    }

    // Segmentos carregáveis não podem se sobrepor na memória virtual.
    std::vector<const Segment*> loads;
    for (const auto& s : segments_) {
        if (s.isLoad() && s.memsz > 0) loads.push_back(&s);
    }
    std::sort(loads.begin(), loads.end(),
              [](const Segment* a, const Segment* b) { return a->vaddr < b->vaddr; });
    for (std::size_t i = 1; i < loads.size(); ++i) {
        const Segment& prev = *loads[i - 1];
        if (std::uint64_t{prev.vaddr} + prev.memsz > loads[i]->vaddr) {
            fail("segmentos PT_LOAD " + std::to_string(prev.index) + " e " +
                 std::to_string(loads[i]->index) + " se sobrepõem em " + hex(loads[i]->vaddr));
        }
    }
}

// ---------------------------------------------------------------------------
// Seções
// ---------------------------------------------------------------------------

std::string ElfFile::readString(const Section& strtab, std::uint32_t offset,
                                const std::string& what) const {
    if (offset >= strtab.size) {
        fail(what + ": offset de nome " + hex(offset) + " fora da tabela de strings '" +
             strtab.name + "' (tamanho " + hex(strtab.size) + ")");
    }
    const std::size_t begin = std::size_t{strtab.offset} + offset;
    const std::size_t end = std::size_t{strtab.offset} + strtab.size;
    std::size_t p = begin;
    while (p < end && data_[p] != 0) ++p;
    if (p == end) fail(what + ": string sem terminador na tabela '" + strtab.name + "'");
    return std::string(reinterpret_cast<const char*>(data_.data() + begin), p - begin);
}

void ElfFile::parseSections() {
    if (shnum_ == 0) return;
    requireRange(shoff_, std::uint64_t{shnum_} * kShdrSize, "tabela de section headers");
    std::span<const std::uint8_t> d(data_);
    sections_.reserve(shnum_);
    std::vector<std::uint32_t> nameOffsets;
    nameOffsets.reserve(shnum_);
    for (std::uint32_t i = 0; i < shnum_; ++i) {
        const std::size_t base = shoff_ + i * kShdrSize;
        Section s;
        s.index = i;
        nameOffsets.push_back(readLE32(d, base + 0));
        s.type = readLE32(d, base + 4);
        s.flags = readLE32(d, base + 8);
        s.addr = readLE32(d, base + 12);
        s.offset = readLE32(d, base + 16);
        s.size = readLE32(d, base + 20);
        s.link = readLE32(d, base + 24);
        s.info = readLE32(d, base + 28);
        s.addralign = readLE32(d, base + 32);
        s.entsize = readLE32(d, base + 36);
        if (i != 0 && s.hasFileData() && s.size > 0) {
            requireRange(s.offset, s.size, "seção " + std::to_string(i));
        }
        sections_.push_back(std::move(s));
    }

    if (shstrndx_ == 0) return;  // sem nomes de seção
    if (shstrndx_ >= sections_.size()) {
        fail("e_shstrndx = " + std::to_string(shstrndx_) + " fora da tabela de seções (" +
             std::to_string(sections_.size()) + " seções)");
    }
    const Section& shstr = sections_[shstrndx_];
    if (shstr.type != SHT_STRTAB) fail("seção de nomes (e_shstrndx) não é SHT_STRTAB");
    for (std::size_t i = 1; i < sections_.size(); ++i) {
        sections_[i].name = readString(shstr, nameOffsets[i], "seção " + std::to_string(i));
    }
}

// ---------------------------------------------------------------------------
// Símbolos e relocações
// ---------------------------------------------------------------------------

void ElfFile::parseSymbols() {
    std::span<const std::uint8_t> d(data_);
    for (const Section& sec : sections_) {
        if (sec.type != SHT_SYMTAB) continue;
        const std::string what = "tabela de símbolos '" + sec.name + "'";
        if (sec.entsize != kSymSize) {
            fail(what + ": sh_entsize = " + std::to_string(sec.entsize) + ", esperado 16");
        }
        if (sec.size % kSymSize != 0) fail(what + ": tamanho não é múltiplo de 16");
        if (sec.link == 0 || sec.link >= sections_.size() ||
            sections_[sec.link].type != SHT_STRTAB) {
            fail(what + ": sh_link (" + std::to_string(sec.link) +
                 ") não aponta para uma SHT_STRTAB");
        }
        const Section& strtab = sections_[sec.link];
        const std::size_t count = sec.size / kSymSize;
        for (std::size_t i = 1; i < count; ++i) {  // o símbolo 0 é sempre nulo
            const std::size_t base = sec.offset + i * kSymSize;
            Symbol s;
            const std::uint32_t nameOff = readLE32(d, base + 0);
            s.value = readLE32(d, base + 4);
            s.size = readLE32(d, base + 8);
            const std::uint8_t info = d[base + 12];
            s.type = static_cast<std::uint8_t>(info & 0xF);
            s.bind = static_cast<std::uint8_t>(info >> 4);
            s.other = d[base + 13];
            s.sectionIndex = readLE16(d, base + 14);
            if (nameOff != 0) {
                s.name = readString(strtab, nameOff, what + ", símbolo " + std::to_string(i));
            }
            if (s.sectionIndex != SHN_UNDEF && s.sectionIndex < SHN_LORESERVE &&
                s.sectionIndex >= sections_.size()) {
                fail(what + ", símbolo '" + s.name + "': índice de seção " +
                     std::to_string(s.sectionIndex) + " inexistente");
            }
            symbols_.push_back(std::move(s));
        }
    }
}

void ElfFile::parseRelocations() {
    std::span<const std::uint8_t> d(data_);
    for (const Section& sec : sections_) {
        if (sec.type != SHT_REL && sec.type != SHT_RELA) continue;
        const bool rela = sec.type == SHT_RELA;
        const std::size_t entSize = rela ? kRelaSize : kRelSize;
        const std::string what = "seção de relocação '" + sec.name + "'";
        if (sec.entsize != 0 && sec.entsize != entSize) {
            fail(what + ": sh_entsize = " + std::to_string(sec.entsize) + ", esperado " +
                 std::to_string(entSize));
        }
        if (sec.size % entSize != 0) fail(what + ": tamanho não é múltiplo da entrada");
        if (sec.info >= sections_.size()) {
            fail(what + ": sh_info aponta para seção inexistente " + std::to_string(sec.info));
        }
        const std::size_t count = sec.size / entSize;
        for (std::size_t i = 0; i < count; ++i) {
            const std::size_t base = sec.offset + i * entSize;
            Relocation r;
            r.offset = readLE32(d, base + 0);
            const std::uint32_t info = readLE32(d, base + 4);
            r.type = info & 0xFF;
            r.symbolIndex = info >> 8;
            r.hasAddend = rela;
            if (rela) r.addend = static_cast<std::int32_t>(readLE32(d, base + 8));
            r.targetSection = sec.info;
            r.relocSection = sec.index;
            relocations_.push_back(r);
        }
    }
}

// ---------------------------------------------------------------------------
// Consultas
// ---------------------------------------------------------------------------

const Section* ElfFile::findSection(std::string_view sectionName) const {
    for (const auto& s : sections_) {
        if (s.name == sectionName) return &s;
    }
    return nullptr;
}

const Symbol* ElfFile::findSymbol(std::string_view symbolName) const {
    const Symbol* best = nullptr;
    for (const auto& s : symbols_) {
        if (s.name != symbolName || !s.isDefined()) continue;
        if (!best || (best->bind == STB_LOCAL && s.bind != STB_LOCAL)) best = &s;
    }
    return best;
}

namespace {
bool isAddressSymbol(const Symbol& s) {
    return s.isDefined() && !s.name.empty() &&
           (s.type == STT_FUNC || s.type == STT_OBJECT || s.type == STT_NOTYPE);
}
int symbolRank(const Symbol& s) {
    int rank = 0;
    if (s.type == STT_FUNC) rank += 4;
    if (s.bind != STB_LOCAL) rank += 2;
    if (s.size > 0) rank += 1;
    return rank;
}
}  // namespace

const Symbol* ElfFile::symbolAt(std::uint32_t address) const {
    const Symbol* best = nullptr;
    for (const auto& s : symbols_) {
        if (!isAddressSymbol(s) || s.value != address) continue;
        if (!best || symbolRank(s) > symbolRank(*best)) best = &s;
    }
    return best;
}

const Symbol* ElfFile::symbolContaining(std::uint32_t address) const {
    const Symbol* best = nullptr;
    for (const auto& s : symbols_) {
        if (!isAddressSymbol(s) || s.size == 0) continue;
        if (address < s.value || address - s.value >= s.size) continue;
        if (!best || symbolRank(s) > symbolRank(*best) ||
            (symbolRank(s) == symbolRank(*best) && s.value > best->value)) {
            best = &s;
        }
    }
    return best;
}

std::span<const std::uint8_t> ElfFile::sectionData(const Section& section) const {
    if (!section.hasFileData() || section.size == 0) return {};
    return std::span<const std::uint8_t>(data_).subspan(section.offset, section.size);
}

std::span<const std::uint8_t> ElfFile::segmentData(const Segment& segment) const {
    if (segment.filesz == 0) return {};
    return std::span<const std::uint8_t>(data_).subspan(segment.offset, segment.filesz);
}

// ---------------------------------------------------------------------------
// Memória virtual
// ---------------------------------------------------------------------------

const Segment* ElfFile::loadSegmentFor(std::uint32_t vaddr) const {
    for (const auto& s : segments_) {
        if (s.isLoad() && vaddr >= s.vaddr && vaddr - s.vaddr < s.memsz) return &s;
    }
    return nullptr;
}

bool ElfFile::isMapped(std::uint32_t vaddr, std::uint32_t size) const {
    if (size == 0) return true;
    const Segment* s = loadSegmentFor(vaddr);
    if (!s) return false;
    // Permite faixas que atravessam segmentos contíguos.
    std::uint64_t end = std::uint64_t{vaddr} + size;
    std::uint64_t segEnd = std::uint64_t{s->vaddr} + s->memsz;
    if (end <= segEnd) return true;
    if (segEnd > 0xFFFFFFFFull) return false;
    return isMapped(static_cast<std::uint32_t>(segEnd), static_cast<std::uint32_t>(end - segEnd));
}

bool ElfFile::isExecutable(std::uint32_t vaddr) const {
    const Segment* s = loadSegmentFor(vaddr);
    return s && s->executable();
}

std::optional<std::uint8_t> ElfFile::tryRead8(std::uint32_t vaddr) const {
    const Segment* s = loadSegmentFor(vaddr);
    if (!s) return std::nullopt;
    const std::uint32_t off = vaddr - s->vaddr;
    if (off >= s->filesz) return std::uint8_t{0};  // bss
    return data_[std::size_t{s->offset} + off];
}

std::optional<std::uint32_t> ElfFile::tryRead32(std::uint32_t vaddr) const {
    std::uint32_t v = 0;
    for (std::uint32_t i = 0; i < 4; ++i) {
        auto b = tryRead8(vaddr + i);
        if (!b) return std::nullopt;
        v |= std::uint32_t{*b} << (8 * i);
    }
    return v;
}

std::uint8_t ElfFile::read8(std::uint32_t vaddr) const {
    auto v = tryRead8(vaddr);
    if (!v) fail("endereço " + hex(vaddr) + " não está mapeado por nenhum segmento PT_LOAD");
    return *v;
}

std::uint32_t ElfFile::read32(std::uint32_t vaddr) const {
    auto v = tryRead32(vaddr);
    if (!v) {
        fail("palavra em " + hex(vaddr) + " não está (inteiramente) mapeada por segmentos PT_LOAD");
    }
    return *v;
}

std::vector<std::uint8_t> ElfFile::readRange(std::uint32_t vaddr, std::uint32_t size) const {
    if (!isMapped(vaddr, size)) {
        fail("faixa [" + hex(vaddr) + ", +" + hex(size) + ") não está inteiramente mapeada");
    }
    std::vector<std::uint8_t> out(size);
    for (std::uint32_t i = 0; i < size; ++i) out[i] = *tryRead8(vaddr + i);
    return out;
}

std::uint32_t ElfFile::loadBase() const {
    std::uint32_t base = 0xFFFFFFFFu;
    bool any = false;
    for (const auto& s : segments_) {
        if (s.isLoad() && s.memsz > 0) {
            base = std::min(base, s.vaddr);
            any = true;
        }
    }
    return any ? base : 0;
}

std::uint32_t ElfFile::loadEnd() const {
    std::uint64_t end = 0;
    for (const auto& s : segments_) {
        if (s.isLoad() && s.memsz > 0) end = std::max(end, std::uint64_t{s.vaddr} + s.memsz);
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(end, 0xFFFFFFFFu));
}

}  // namespace anyps2::elf
