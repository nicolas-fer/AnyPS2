// Testes do parser de ELF.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "anyps2/elf/elf_file.h"
#include "anyps2/r5900/decoder.h"
#include "anyps2/r5900/disassembler.h"
#include "elf_builder.h"
#include "minitest.h"

using namespace anyps2::elf;
using testutil::ElfBuilder;

namespace {

ElfFile load(std::vector<std::uint8_t> bytes) {
    return ElfFile::loadFromMemory(std::move(bytes), "teste.elf");
}

std::string fixture(const char* name) {
    return std::string(ANYPS2_TEST_FIXTURES_DIR) + "/" + name;
}

}  // namespace

// ---------------------------------------------------------------------------
// Casos válidos
// ---------------------------------------------------------------------------

TEST_CASE(elf, header_fields) {
    auto b = testutil::typicalExecutable();
    const auto f = load(b.build());
    CHECK_EQ(f.name(), std::string("teste.elf"));
    CHECK_EQ(f.type(), ET_EXEC);
    CHECK_EQ(f.machine(), EM_MIPS);
    CHECK_EQ(f.entry(), 0x00100000u);
    CHECK(f.isR5900());
    CHECK(!f.isIopModule());
    CHECK_EQ(ElfFile::typeName(ET_EXEC), std::string_view("EXEC (executável)"));
}

TEST_CASE(elf, segments_and_virtual_memory) {
    auto b = testutil::typicalExecutable();
    const auto f = load(b.build());
    REQUIRE(f.segments().size() == 2);
    const auto& text = f.segments()[0];
    CHECK(text.isLoad());
    CHECK(text.executable());
    CHECK_EQ(text.vaddr, 0x00100000u);
    CHECK_EQ(text.filesz, 40u);
    const auto& data = f.segments()[1];
    CHECK(!data.executable());
    CHECK_EQ(data.filesz, 8u);
    CHECK_EQ(data.memsz, 0x108u);

    CHECK_EQ(f.read32(0x00100000), 0x27BDFFF0u);
    CHECK_EQ(f.read32(0x00100024), 0x24020007u);
    CHECK_EQ(f.read8(0x00100100), 0xEFu);
    CHECK_EQ(f.read32(0x00100104), 0x01234567u);
    // bss lê como zero
    CHECK_EQ(f.read32(0x00100108), 0u);
    CHECK_EQ(f.read32(0x00100204), 0u);
    CHECK(f.isMapped(0x00100100, 0x108));
    CHECK(!f.isMapped(0x00100100, 0x109));
    CHECK(!f.isMapped(0x00100028));  // entre .text e .data
    CHECK(f.isExecutable(0x00100010));
    CHECK(!f.isExecutable(0x00100100));
    CHECK(!f.tryRead32(0x00100206).has_value());  // atravessa o fim
    CHECK_EQ(f.loadBase(), 0x00100000u);
    CHECK_EQ(f.loadEnd(), 0x00100208u);

    const auto range = f.readRange(0x00100104, 8);
    REQUIRE(range.size() == 8);
    CHECK_EQ(range[0], 0x67);
    CHECK_EQ(range[4], 0x00);

    CHECK_THROWS_WITH(f.read32(0x00200000), "0x00200000");
    CHECK_THROWS_WITH(f.read8(0x0), "não está mapeado");
    CHECK_THROWS_WITH(f.readRange(0x00100000, 0x100), "não está inteiramente mapeada");
}

TEST_CASE(elf, sections) {
    auto b = testutil::typicalExecutable();
    const auto f = load(b.build());
    const auto* text = f.findSection(".text");
    REQUIRE(text != nullptr);
    CHECK(text->isExec());
    CHECK(text->isAlloc());
    CHECK_EQ(text->addr, 0x00100000u);
    CHECK_EQ(f.sectionData(*text).size(), 40u);
    CHECK_EQ(f.sectionData(*text)[0], 0xF0);
    const auto* bss = f.findSection(".bss");
    REQUIRE(bss != nullptr);
    CHECK_EQ(bss->type, SHT_NOBITS);
    CHECK_EQ(bss->size, 0x100u);
    CHECK(f.sectionData(*bss).empty());
    CHECK(f.findSection(".symtab") != nullptr);
    CHECK(f.findSection(".inexistente") == nullptr);
    CHECK_EQ(f.sections()[0].type, SHT_NULL);
}

TEST_CASE(elf, symbols) {
    auto b = testutil::typicalExecutable();
    const auto f = load(b.build());
    CHECK_EQ(f.symbols().size(), 6u);
    const auto* start = f.findSymbol("_start");
    REQUIRE(start != nullptr);
    CHECK(start->isFunction());
    CHECK_EQ(start->value, 0x00100000u);
    CHECK_EQ(start->size, 0x14u);
    CHECK(f.findSymbol("external") == nullptr);  // indefinido
    CHECK(f.findSymbol("nao_existe") == nullptr);

    // Endereço exato: prefere o símbolo global entre dois de mesmo endereço.
    const auto* at = f.symbolAt(0x00100020);
    REQUIRE(at != nullptr);
    CHECK_EQ(at->name, std::string("helper_alias"));
    CHECK(f.symbolAt(0x00100004) == nullptr);

    const auto* inside = f.symbolContaining(0x00100010);
    REQUIRE(inside != nullptr);
    CHECK_EQ(inside->name, std::string("_start"));
    CHECK(f.symbolContaining(0x00100014) == nullptr);  // logo após _start
    const auto* obj = f.symbolContaining(0x00100150);
    REQUIRE(obj != nullptr);
    CHECK_EQ(obj->name, std::string("buffer"));
}

TEST_CASE(elf, relocations) {
    auto b = testutil::typicalExecutable();
    b.rels = {{0x00100004, R_MIPS_26, 2}, {0x00100008, R_MIPS_HI16, 4}};
    const auto f = load(b.build());
    REQUIRE(f.relocations().size() == 2);
    const auto& r = f.relocations()[0];
    CHECK_EQ(r.offset, 0x00100004u);
    CHECK_EQ(r.type, R_MIPS_26);
    CHECK_EQ(r.symbolIndex, 2u);
    CHECK(!r.hasAddend);
    CHECK_EQ(f.sections()[r.targetSection].name, std::string(".text"));
    CHECK_EQ(f.sections()[r.relocSection].name, std::string(".rel.text"));
    CHECK_EQ(f.relocations()[1].type, R_MIPS_HI16);
}

TEST_CASE(elf, iop_module_type) {
    auto b = testutil::typicalExecutable();
    b.type = ET_SCE_IOPRELEXEC;
    b.flags = 0;  // módulos do IOP não são R5900
    const auto f = load(b.build());
    CHECK(f.isIopModule());
    CHECK(!f.isR5900());
}

TEST_CASE(elf, without_section_headers) {
    // Muitos executáveis "strippados" ainda têm seções, mas o loader só
    // precisa dos program headers.
    auto b = testutil::typicalExecutable();
    auto bytes = b.build();
    anyps2::writeLE32(bytes, 32, 0);  // e_shoff
    anyps2::writeLE16(bytes, 48, 0);  // e_shnum
    anyps2::writeLE16(bytes, 50, 0);  // e_shstrndx
    const auto f = load(bytes);
    CHECK(f.sections().empty());
    CHECK(f.symbols().empty());
    CHECK_EQ(f.read32(0x00100000), 0x27BDFFF0u);
}

// ---------------------------------------------------------------------------
// Erros: toda falha deve ser explícita e dizer o que está errado
// ---------------------------------------------------------------------------

TEST_CASE(elf, rejects_bad_identification) {
    CHECK_THROWS_WITH(load({}), "pequeno demais");
    CHECK_THROWS_WITH(load({'M', 'Z', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}), "assinatura ELF");

    auto good = testutil::typicalExecutable().build();
    auto elf64 = good;
    elf64[4] = 2;
    CHECK_THROWS_WITH(load(elf64), "64 bits");
    auto be = good;
    be[5] = 2;
    CHECK_THROWS_WITH(load(be), "big-endian");
    auto x86 = good;
    anyps2::writeLE16(x86, 18, 62);
    CHECK_THROWS_WITH(load(x86), "e_machine = 62");
    auto core = good;
    anyps2::writeLE16(core, 16, 4);  // ET_CORE
    CHECK_THROWS_WITH(load(core), "tipo de ELF não suportado");
    auto truncated = good;
    truncated.resize(40);
    CHECK_THROWS_WITH(load(truncated), "cabeçalho ELF truncado");
    auto badPhent = good;
    anyps2::writeLE16(badPhent, 42, 56);
    CHECK_THROWS_WITH(load(badPhent), "e_phentsize");
    // A mensagem identifica o arquivo.
    CHECK_THROWS_WITH(load(x86), "teste.elf:");
}

TEST_CASE(elf, rejects_bad_segments) {
    auto b = testutil::typicalExecutable();
    const auto good = b.build();

    auto phOut = good;
    anyps2::writeLE32(phOut, 28, static_cast<std::uint32_t>(good.size() - 8));  // e_phoff
    CHECK_THROWS_WITH(load(phOut), "tabela de program headers fora do arquivo");

    auto dataOut = good;
    anyps2::writeLE32(dataOut, b.phoff + 16, 0x100000);  // p_filesz enorme
    anyps2::writeLE32(dataOut, b.phoff + 20, 0x100000);
    CHECK_THROWS_WITH(load(dataOut), "segmento 0 fora do arquivo");

    auto fileBig = good;
    anyps2::writeLE32(fileBig, b.phoff + 20, 4);  // p_memsz < p_filesz
    CHECK_THROWS_WITH(load(fileBig), "p_filesz");

    auto overlap = good;
    anyps2::writeLE32(overlap, b.phoff + 32 + 8, 0x00100010);  // vaddr do 2º segmento
    CHECK_THROWS_WITH(load(overlap), "se sobrepõem");

    auto wrap = good;
    anyps2::writeLE32(wrap, b.phoff + 8, 0xFFFFFFF0);
    CHECK_THROWS_WITH(load(wrap), "32 bits");
}

TEST_CASE(elf, rejects_bad_sections_and_symbols) {
    auto b = testutil::typicalExecutable();
    const auto good = b.build();

    auto shOut = good;
    anyps2::writeLE32(shOut, 32, static_cast<std::uint32_t>(good.size() - 4));
    CHECK_THROWS_WITH(load(shOut), "tabela de section headers fora do arquivo");

    auto badStrndx = good;
    anyps2::writeLE16(badStrndx, 50, 99);
    CHECK_THROWS_WITH(load(badStrndx), "e_shstrndx = 99");

    auto badName = good;
    anyps2::writeLE32(badName, b.shoff + 40, 0x7FFF);  // sh_name da seção 1
    CHECK_THROWS_WITH(load(badName), "fora da tabela de strings");

    auto secOut = good;
    anyps2::writeLE32(secOut, b.shoff + 40 + 16, 0x7FFFFFF0);  // sh_offset da .text
    CHECK_THROWS_WITH(load(secOut), "seção 1 fora do arquivo");

    // Símbolo 1 (_start): offset de nome inválido e índice de seção inexistente.
    auto symName = good;
    anyps2::writeLE32(symName, b.symtabOffset + 16, 0xFFFF);
    CHECK_THROWS_WITH(load(symName), "símbolo 1");
    auto symShndx = good;
    anyps2::writeLE16(symShndx, b.symtabOffset + 16 + 14, 77);
    CHECK_THROWS_WITH(load(symShndx), "índice de seção 77 inexistente");

    // sh_link da .symtab apontando para uma seção que não é STRTAB.
    const auto f = load(good);
    const auto* symtab = f.findSection(".symtab");
    REQUIRE(symtab != nullptr);
    auto badLink = good;
    anyps2::writeLE32(badLink, b.shoff + symtab->index * 40 + 24, 1);
    CHECK_THROWS_WITH(load(badLink), "sh_link");
    auto badEnt = good;
    anyps2::writeLE32(badEnt, b.shoff + symtab->index * 40 + 36, 24);
    CHECK_THROWS_WITH(load(badEnt), "sh_entsize = 24");

    // String sem terminador: remove o '\0' final da .strtab.
    const auto* strtab = f.findSection(".strtab");
    REQUIRE(strtab != nullptr);
    auto unterminated = good;
    unterminated[strtab->offset + strtab->size - 1] = 'X';
    CHECK_THROWS_WITH(load(unterminated), "sem terminador");
}

TEST_CASE(elf, load_from_file_errors) {
    CHECK_THROWS_WITH(ElfFile::loadFromFile("/caminho/que/nao/existe.elf"),
                      "não foi possível abrir");
}

// ---------------------------------------------------------------------------
// Fixture real, montada pelo GNU as -march=r5900 (tests/fixtures)
// ---------------------------------------------------------------------------

TEST_CASE(fixture, hello_r5900_parses) {
    const auto f = ElfFile::loadFromFile(fixture("hello_r5900.elf"));
    CHECK_EQ(f.type(), ET_EXEC);
    CHECK(f.isR5900());
    CHECK_EQ(f.entry(), 0x00100000u);
    const auto* start = f.findSymbol("_start");
    REQUIRE(start != nullptr);
    CHECK_EQ(start->value, f.entry());
    const auto* main = f.findSymbol("main");
    REQUIRE(main != nullptr);
    CHECK(main->isFunction());
    CHECK(main->size > 0);
    const auto* message = f.findSymbol("message");
    REQUIRE(message != nullptr);
    std::string text;
    for (std::uint32_t a = message->value; f.read8(a) != 0; ++a) {
        text += static_cast<char>(f.read8(a));
    }
    CHECK_EQ(text, std::string("Ola, PS2!\n"));
    const auto* buffer = f.findSymbol("buffer");
    REQUIRE(buffer != nullptr);
    CHECK_EQ(f.read32(buffer->value), 0u);  // bss
    CHECK(f.findSection(".bss") != nullptr);
}

TEST_CASE(fixture, hello_r5900_disassembly_matches_objdump) {
    const auto f = ElfFile::loadFromFile(fixture("hello_r5900.elf"));
    std::ifstream in(fixture("hello_r5900.text.expected"));
    REQUIRE(in.good());
    std::string line;
    std::size_t count = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::istringstream ls(line);
        std::string addrHex, wordHex;
        ls >> addrHex >> wordHex;
        const auto addr = static_cast<std::uint32_t>(std::stoul(addrHex, nullptr, 16));
        const auto word = static_cast<std::uint32_t>(std::stoul(wordHex, nullptr, 16));
        const std::string expected = line.substr(line.find('\t') + 1);
        CHECK_EQ(f.read32(addr), word);
        const auto insn = anyps2::r5900::decode(word, addr);
        CHECK_MSG(insn.valid() && insn.canonical, line);
        CHECK_EQ(anyps2::r5900::disassemble(insn).str(), expected);
        ++count;
    }
    const auto* text = f.findSection(".text");
    REQUIRE(text != nullptr);
    CHECK_EQ(count, text->size / 4);
}

// ---------------------------------------------------------------------------
// Robustez: mutações aleatórias devem carregar ou lançar ElfError, nunca
// travar ou ler fora do buffer (rode também com -fsanitize=address).
// ---------------------------------------------------------------------------

TEST_CASE(elf, fuzz_mutations_never_crash) {
    auto b = testutil::typicalExecutable();
    b.rels = {{0x00100004, R_MIPS_26, 2}};
    const auto good = b.build();
    std::uint32_t x = 0xC0FFEEu;
    auto next = [&x]() {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    };
    std::size_t loaded = 0, rejected = 0;
    for (int iter = 0; iter < 20000; ++iter) {
        auto bytes = good;
        const int edits = 1 + static_cast<int>(next() % 4);
        for (int e = 0; e < edits; ++e) {
            const std::size_t pos = next() % bytes.size();
            switch (next() % 3) {
                case 0: bytes[pos] = static_cast<std::uint8_t>(next()); break;
                case 1: bytes[pos] ^= static_cast<std::uint8_t>(1u << (next() % 8)); break;
                default: bytes.resize(pos + 1); break;  // trunca
            }
        }
        try {
            const auto f = ElfFile::loadFromMemory(bytes, "fuzz");
            ++loaded;
            // Exercita as consultas no arquivo aceito.
            for (const auto& s : f.sections()) (void)f.sectionData(s);
            for (const auto& s : f.segments()) (void)f.segmentData(s);
            (void)f.symbolContaining(0x00100010);
            (void)f.tryRead32(0x00100000);
            (void)f.isMapped(0xFFFFFFF0u, 0x100);
        } catch (const ElfError&) {
            ++rejected;
        }
    }
    CHECK(loaded > 0);
    CHECK(rejected > 0);
}
