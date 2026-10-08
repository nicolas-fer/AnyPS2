// anyps2 — ferramenta de linha de comando do AnyPS2.
//
// Fase 1: inspeção de ELF e desmontagem do código R5900.
// Fase 2+: "anyps2 recomp" gera o projeto C++/CMake a partir do ELF.
// Fase 7: "anyps2 disc" faz a triagem de um disco (o que o jogo usa e o que
// disso já tem HLE).

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "anyps2/analysis/analyzer.h"
#include "anyps2/codegen/generator.h"
#include "anyps2/codegen/vu_generator.h"
#include "anyps2/common/bytes.h"
#include "anyps2/common/error.h"
#include "anyps2/elf/elf_file.h"
#include "anyps2/r5900/decoder.h"
#include "anyps2/r5900/disassembler.h"
#include "anyps2/vu/isa.h"
#include "anyps2/vu/scan.h"
#include "disc.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace {

using anyps2::Error;
using anyps2::hex;
namespace elf = anyps2::elf;
namespace r5900 = anyps2::r5900;

constexpr const char* kVersion = "0.5.0 (Fase 5)";

void printUsage() {
    std::cout <<
        R"(AnyPS2 — recompilador estático de PS2 para PC

Uso:
  anyps2 info <arquivo.elf>
      Mostra cabeçalho, segmentos, seções e resumo de símbolos.

  anyps2 symbols <arquivo.elf> [--functions]
      Lista os símbolos (ou só funções).

  anyps2 disasm <arquivo.elf> [--section NOME | --symbol NOME |
                               --range INICIO FIM] [--no-symbols]
                               [--mark-noncanonical]
      Desmonta código R5900 na sintaxe do GNU objdump. Sem filtros,
      desmonta todas as seções executáveis (ou segmentos executáveis).

  anyps2 disasm-bin <arquivo.bin> [--base ENDERECO] [--mark-noncanonical]
      Desmonta um binário cru de palavras little-endian.

  anyps2 recomp <arquivo.elf> -o <diretório> [--name NOME] [--root RAIZ]
               [--function 0xENDERECO ...] [--vu-dumps DIR] [--no-vu]
               [--extra OUTRO.elf ...]
      Gera um projeto CMake com o código C++ recompilado. Compile com
      "cmake -S <diretório> -B <diretório>/build && cmake --build ...".
      --root aponta para a raiz do AnyPS2 (padrão: a usada neste build).
      --function adiciona inícios de função que a análise não achou.
      O microcódigo dos VUs achado no ELF também é recompilado; --vu-dumps
      acrescenta os dumps gravados com ANYPS2_VU_DUMP=DIR (microcódigo que
      só existe em tempo de execução); --no-vu deixa tudo para o
      interpretador.
      --extra inclui código que o programa só cria em tempo de execução (ex.:
      o executável principal que o boot de um jogo descomprime e chama com
      ExecPS2): é recompilado no mesmo projeto, mas fica fora da imagem.

  anyps2 ram2elf <ram.bin> --entry ENDERECO --text INICIO FIM [--text ...]
                 [--data INICIO FIM ...] [--bss-end FIM] -o <saida.elf>
      Monta um ELF a partir de uma imagem da RAM do EE (ANYPS2_EXEC_DUMP):
      faixas de código (--text) e de dados (--data), e .bss depois da última. Serve para recompilar
      com "recomp --extra" um programa que só existe na memória.

  anyps2 vu <arquivo.elf | dump.bin> [--disasm]
      Lista (e desmonta) o microcódigo de VU encontrado: pacotes VIF MPG,
      blocos crus nos dados ou um dump da micro memória.

  anyps2 vu-gen <arquivo.elf | dump.bin> -o <diretório> [--table NOME]
      Gera só o C++ do microcódigo (vu_NNN.cpp + vu_programs.cpp).

  anyps2 disc <imagem.iso>
      Triagem de um disco de PS2 (dump próprio): executável principal pelo
      SYSTEM.CNF (tamanho, segmentos), módulos do IOP que ele referencia
      (rom0:/cdrom0:/host:) e IRX embutidos, cada IRX e imagem IOPRP do
      disco com nome, versão e se tem HLE no AnyPS2, e outros ELFs (overlays).
      Lê as duas camadas de DVDs de camada dupla. Não extrai nada.

  anyps2 --version | --help
)";
}

std::uint32_t parseNumber(std::string_view text) {
    std::string s(text);
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 0);
    if (s.empty() || end == nullptr || *end != '\0' || v > 0xFFFFFFFFull) {
        throw Error("número inválido: '" + s + "'");
    }
    return static_cast<std::uint32_t>(v);
}

std::string pad(std::string s, std::size_t width) {
    if (s.size() < width) s.append(width - s.size(), ' ');
    return s;
}

std::string flagsString(std::uint32_t pflags) {
    std::string s;
    s += (pflags & elf::PF_R) ? 'R' : '-';
    s += (pflags & elf::PF_W) ? 'W' : '-';
    s += (pflags & elf::PF_X) ? 'X' : '-';
    return s;
}

std::string sectionTypeName(std::uint32_t type) {
    switch (type) {
        case elf::SHT_NULL: return "NULL";
        case elf::SHT_PROGBITS: return "PROGBITS";
        case elf::SHT_SYMTAB: return "SYMTAB";
        case elf::SHT_STRTAB: return "STRTAB";
        case elf::SHT_RELA: return "RELA";
        case elf::SHT_NOBITS: return "NOBITS";
        case elf::SHT_REL: return "REL";
        default: return hex(type);
    }
}

std::string segmentTypeName(std::uint32_t type) {
    switch (type) {
        case elf::PT_NULL: return "NULL";
        case elf::PT_LOAD: return "LOAD";
        case elf::PT_SCE_IOPMOD: return "SCE_IOPMOD";
        default: return hex(type);
    }
}

// ---------------------------------------------------------------------------

int cmdInfo(const std::vector<std::string_view>& args) {
    if (args.size() != 1) throw Error("uso: anyps2 info <arquivo.elf>");
    const auto f = elf::ElfFile::loadFromFile(std::string(args[0]));

    std::cout << "Arquivo:        " << f.name() << "\n";
    std::cout << "Tipo:           " << elf::ElfFile::typeName(f.type()) << "\n";
    std::cout << "Máquina:        MIPS (e_flags " << hex(f.flags())
              << (f.isR5900() ? ", R5900/Emotion Engine)" : ", sem marca de R5900)") << "\n";
    std::cout << "Entrada:        " << hex(f.entry()) << "\n";
    std::cout << "Faixa carregada:" << " " << hex(f.loadBase()) << " .. " << hex(f.loadEnd())
              << "\n";
    if (!f.isR5900()) {
        std::cout << "Aviso: o ELF não declara a máquina R5900; pode ser um binário do IOP "
                     "(R3000) ou de outro MIPS.\n";
    }

    std::cout << "\nSegmentos (" << f.segments().size() << "):\n";
    std::cout << "  #  Tipo        Offset      VAddr       FileSz      MemSz       Flags\n";
    for (const auto& s : f.segments()) {
        std::printf("  %-2u %-11s %s  %s  %s  %s  %s\n", s.index, segmentTypeName(s.type).c_str(),
                    hex(s.offset).c_str(), hex(s.vaddr).c_str(), hex(s.filesz).c_str(),
                    hex(s.memsz).c_str(), flagsString(s.flags).c_str());
    }
    std::fflush(stdout);

    std::cout << "\nSeções (" << f.sections().size() << "):\n";
    std::cout << "  #   Nome                 Tipo        Endereço    Offset      Tamanho     "
                 "Flags\n";
    for (const auto& s : f.sections()) {
        if (s.index == 0) continue;
        std::string fl;
        if (s.flags & elf::SHF_ALLOC) fl += 'A';
        if (s.flags & elf::SHF_WRITE) fl += 'W';
        if (s.flags & elf::SHF_EXECINSTR) fl += 'X';
        std::printf("  %-3u %-20s %-11s %s  %s  %s  %s\n", s.index, pad(s.name, 20).c_str(),
                    sectionTypeName(s.type).c_str(), hex(s.addr).c_str(), hex(s.offset).c_str(),
                    hex(s.size).c_str(), fl.c_str());
    }
    std::fflush(stdout);

    std::size_t funcs = 0;
    for (const auto& s : f.symbols()) funcs += s.isFunction() ? 1u : 0u;
    std::cout << "\nSímbolos: " << f.symbols().size() << " (" << funcs << " funções)\n";
    std::cout << "Relocações: " << f.relocations().size() << "\n";
    if (const auto* entry = f.symbolContaining(f.entry())) {
        std::cout << "Ponto de entrada está em: " << entry->name << "\n";
    }
    return 0;
}

int cmdSymbols(const std::vector<std::string_view>& args) {
    bool onlyFunctions = false;
    std::optional<std::string> path;
    for (auto a : args) {
        if (a == "--functions") onlyFunctions = true;
        else if (!path) path = std::string(a);
        else throw Error("argumento inesperado: " + std::string(a));
    }
    if (!path) throw Error("uso: anyps2 symbols <arquivo.elf> [--functions]");
    const auto f = elf::ElfFile::loadFromFile(*path);
    for (const auto& s : f.symbols()) {
        if (onlyFunctions && !s.isFunction()) continue;
        if (s.name.empty()) continue;
        const char* type = s.type == elf::STT_FUNC     ? "FUNC"
                           : s.type == elf::STT_OBJECT ? "OBJECT"
                           : s.type == elf::STT_SECTION ? "SECTION"
                           : s.type == elf::STT_FILE   ? "FILE"
                                                       : "NOTYPE";
        const char* bind = s.bind == elf::STB_GLOBAL ? "GLOBAL"
                           : s.bind == elf::STB_WEAK ? "WEAK"
                                                     : "LOCAL";
        std::printf("%s %8u %-7s %-6s %s\n", hex(s.value).c_str(), s.size, type, bind,
                    s.name.c_str());
    }
    return 0;
}

struct DisasmRange {
    std::uint32_t start;
    std::uint32_t end;  // exclusivo
    std::string label;
};

void disassembleRange(const elf::ElfFile& f, const DisasmRange& range, bool useSymbols,
                      bool markNonCanonical) {
    r5900::DisasmOptions options;
    if (useSymbols) {
        options.symbolize = [&f](std::uint32_t addr) -> std::string {
            if (const auto* s = f.symbolAt(addr)) return s->name;
            if (const auto* s = f.symbolContaining(addr)) {
                return s->name + "+" + hex(addr - s->value, 1);
            }
            return {};
        };
    }
    std::cout << "\nDesmontagem de " << range.label << ":\n";
    for (std::uint32_t addr = range.start; addr + 4 <= range.end && addr >= range.start; addr += 4) {
        if (useSymbols) {
            if (const auto* s = f.symbolAt(addr)) {
                std::printf("\n%08x <%s>:\n", addr, s->name.c_str());
            }
        }
        const auto word = f.tryRead32(addr);
        if (!word) {
            std::printf("%8x:\t<não mapeado>\n", addr);
            continue;
        }
        const auto insn = r5900::decode(*word, addr);
        const std::string text = r5900::disassemble(insn, options).str();
        if (markNonCanonical && insn.valid() && !insn.canonical) {
            std::printf("%8x:\t%08x \t%s\t# não canônica\n", addr, *word, text.c_str());
        } else {
            std::printf("%8x:\t%08x \t%s\n", addr, *word, text.c_str());
        }
    }
}

int cmdDisasm(const std::vector<std::string_view>& args) {
    std::optional<std::string> path, section, symbol;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> range;
    bool useSymbols = true;
    bool markNonCanonical = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto a = args[i];
        auto next = [&]() -> std::string_view {
            if (i + 1 >= args.size()) throw Error("faltou valor para " + std::string(a));
            return args[++i];
        };
        if (a == "--section") section = std::string(next());
        else if (a == "--symbol") symbol = std::string(next());
        else if (a == "--range") {
            const auto s = parseNumber(next());
            const auto e = parseNumber(next());
            if (e <= s) throw Error("--range: FIM deve ser maior que INICIO");
            range = std::make_pair(s, e);
        } else if (a == "--no-symbols") useSymbols = false;
        else if (a == "--mark-noncanonical") markNonCanonical = true;
        else if (!path) path = std::string(a);
        else throw Error("argumento inesperado: " + std::string(a));
    }
    if (!path) throw Error("uso: anyps2 disasm <arquivo.elf> [opções]");
    const auto f = elf::ElfFile::loadFromFile(*path);

    std::vector<DisasmRange> ranges;
    if (section) {
        const auto* s = f.findSection(*section);
        if (!s) throw Error("seção '" + *section + "' não existe em " + f.name());
        ranges.push_back({s->addr, s->addr + s->size, "seção " + s->name});
    } else if (symbol) {
        const auto* s = f.findSymbol(*symbol);
        if (!s) throw Error("símbolo '" + *symbol + "' não existe em " + f.name());
        if (s->size == 0) throw Error("símbolo '" + *symbol + "' tem tamanho 0; use --range");
        ranges.push_back({s->value, s->value + s->size, "símbolo " + s->name});
    } else if (range) {
        ranges.push_back({range->first, range->second, hex(range->first) + ".." + hex(range->second)});
    } else {
        for (const auto& s : f.sections()) {
            if (s.isExec() && s.isAlloc() && s.size > 0) {
                ranges.push_back({s.addr, s.addr + s.size, "seção " + s.name});
            }
        }
        if (ranges.empty()) {
            for (const auto& s : f.segments()) {
                if (s.isLoad() && s.executable() && s.filesz > 0) {
                    ranges.push_back({s.vaddr, s.vaddr + s.filesz,
                                      "segmento " + std::to_string(s.index)});
                }
            }
        }
        if (ranges.empty()) throw Error(f.name() + " não tem seções nem segmentos executáveis");
    }
    for (const auto& r : ranges) disassembleRange(f, r, useSymbols, markNonCanonical);
    return 0;
}

int cmdDisasmBin(const std::vector<std::string_view>& args) {
    std::optional<std::string> path;
    std::uint32_t base = 0;
    bool markNonCanonical = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto a = args[i];
        if (a == "--base") {
            if (i + 1 >= args.size()) throw Error("faltou valor para --base");
            base = parseNumber(args[++i]);
        } else if (a == "--mark-noncanonical") markNonCanonical = true;
        else if (!path) path = std::string(a);
        else throw Error("argumento inesperado: " + std::string(a));
    }
    if (!path) throw Error("uso: anyps2 disasm-bin <arquivo.bin> [--base ENDERECO]");
    std::ifstream in(*path, std::ios::binary);
    if (!in) throw Error("não foi possível abrir '" + *path + "'");
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    if (data.size() % 4 != 0) {
        throw Error("'" + *path + "' tem " + std::to_string(data.size()) +
                    " bytes, que não é múltiplo de 4");
    }
    std::string out;
    out.reserve(data.size() * 12);
    char line[160];
    for (std::size_t off = 0; off < data.size(); off += 4) {
        const std::uint32_t word = anyps2::readLE32(data, off);
        const std::uint32_t addr = base + static_cast<std::uint32_t>(off);
        const auto insn = r5900::decode(word, addr);
        const std::string text = r5900::disassemble(insn).str();
        const char* mark = (markNonCanonical && insn.valid() && !insn.canonical) ? "\t# não canônica" : "";
        std::snprintf(line, sizeof(line), "%8x:\t%08x \t%s%s\n", addr, word, text.c_str(), mark);
        out += line;
    }
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}

std::string sanitizeName(std::string s) {
    for (char& ch : s) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') ch = '_';
    }
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) s = "ps2_" + s;
    return s;
}

// Dumps .bin gravados com ANYPS2_VU_DUMP (ordem estável).
std::vector<std::filesystem::path> listDumps(const std::string& dir) {
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::is_directory(dir)) throw Error("--vu-dumps: " + dir + " não é um diretório");
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.is_regular_file() && e.path().extension() == ".bin") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Microcódigo de um ELF ou de um dump .bin.
std::vector<anyps2::vu::MicroBlob> loadMicrocode(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    char magic[4] = {};
    f.read(magic, 4);
    if (f && magic[0] == 0x7F && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F') {
        return anyps2::codegen::findElfMicrocode(elf::ElfFile::loadFromFile(path));
    }
    return anyps2::codegen::findDumpMicrocode(path);
}

int cmdVu(const std::vector<std::string_view>& args) {
    std::optional<std::string> path;
    bool disasm = false;
    for (const auto a : args) {
        if (a == "--disasm") disasm = true;
        else if (!path) path = std::string(a);
        else throw Error("argumento inesperado: " + std::string(a));
    }
    if (!path) throw Error("uso: anyps2 vu <arquivo.elf | dump.bin> [--disasm]");
    const auto blobs = loadMicrocode(*path);
    std::cout << blobs.size() << " bloco(s) de microcódigo\n";
    for (std::size_t i = 0; i < blobs.size(); ++i) {
        const auto& b = blobs[i];
        std::cout << "\nbloco " << i << ": " << anyps2::codegen::describeBlob(b) << ", " << b.words.size() / 2
                  << " pares\n";
        if (!disasm) continue;
        const std::uint32_t base = b.loadAddress == anyps2::vu::kUnknownLoad ? 0 : b.loadAddress;
        for (std::size_t w = 0; w + 1 < b.words.size(); w += 2) {
            const auto pc = static_cast<std::uint32_t>(base + w * 4);
            const auto in = anyps2::vu::decode(b.words[w], b.words[w + 1]);
            char head[40];
            std::snprintf(head, sizeof(head), "  %04x: %08x %08x  ", pc, b.words[w], b.words[w + 1]);
            std::cout << head << anyps2::vu::disassemble(in, pc) << "\n";
        }
    }
    return 0;
}

int cmdVuGen(const std::vector<std::string_view>& args) {
    std::optional<std::string> path, out;
    std::string table = "kVuPrograms";
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto a = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) throw Error("faltou valor para " + std::string(a));
            return std::string(args[++i]);
        };
        if (a == "-o" || a == "--output") out = next();
        else if (a == "--table") table = next();
        else if (!path) path = std::string(a);
        else throw Error("argumento inesperado: " + std::string(a));
    }
    if (!path || !out) throw Error("uso: anyps2 vu-gen <arquivo.elf | dump.bin> -o <diretório> [--table NOME]");
    const auto blobs = loadMicrocode(*path);
    std::filesystem::create_directories(*out);
    for (const auto& f : anyps2::codegen::generateVuSources(blobs, table)) {
        std::ofstream o(std::filesystem::path(*out) / f.name, std::ios::binary);
        if (!o) throw Error("não foi possível criar " + (std::filesystem::path(*out) / f.name).string());
        o << f.text;
    }
    return 0;
}

// Faixa da RAM que vira um segmento do ELF gerado por "ram2elf".
struct RamRegion {
    std::uint32_t start = 0, end = 0;
    bool code = false;
};

// ELF32 mínimo (MIPS LE, marca R5900) com as faixas dadas como segmentos e
// seções .text/.data, e um .bss depois da última faixa (até bssEnd).
std::vector<std::uint8_t> buildRamElf(const std::vector<std::uint8_t>& ram, std::uint32_t entry,
                                      std::vector<RamRegion> regions, std::uint32_t bssEnd) {
    auto put16 = [](std::vector<std::uint8_t>& v, std::size_t off, std::uint16_t x) {
        v[off] = static_cast<std::uint8_t>(x);
        v[off + 1] = static_cast<std::uint8_t>(x >> 8);
    };
    auto put32 = [](std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) {
        for (int i = 0; i < 4; ++i) v[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x >> (8 * i));
    };
    auto align = [](std::size_t v, std::size_t a) { return (v + a - 1) & ~(a - 1); };
    std::sort(regions.begin(), regions.end(), [](const RamRegion& a, const RamRegion& b) { return a.start < b.start; });
    const std::uint32_t lastEnd = regions.back().end;
    const std::uint32_t bssSize = bssEnd > lastEnd ? bssEnd - lastEnd : 0;
    const std::string shstr = std::string("\0.text\0.data\0.bss\0.shstrtab\0", 28);
    const auto n = static_cast<unsigned>(regions.size());
    // Cabeçalho, phdrs, conteúdo das faixas, shstrtab, shdrs.
    std::vector<std::size_t> offsets;
    std::size_t off = align(52 + n * 32, 16);
    for (const auto& r : regions) {
        offsets.push_back(off);
        off = align(off + (r.end - r.start), 16);
    }
    const std::size_t strOff = off;
    const std::size_t shOff = align(strOff + shstr.size(), 4);
    const unsigned shnum = n + 3;  // nulo, faixas, .bss, .shstrtab
    std::vector<std::uint8_t> e(shOff + shnum * 40, 0);
    const std::uint8_t ident[] = {0x7F, 'E', 'L', 'F', 1, 1, 1};
    std::copy(std::begin(ident), std::end(ident), e.begin());
    put16(e, 16, 2);  // ET_EXEC
    put16(e, 18, 8);  // EM_MIPS
    put32(e, 20, 1);
    put32(e, 24, entry);
    put32(e, 28, 52);
    put32(e, 32, static_cast<std::uint32_t>(shOff));
    put32(e, 36, 0x20924001u);  // MIPS III, R5900, noreorder
    put16(e, 40, 52);
    put16(e, 42, 32);
    put16(e, 44, static_cast<std::uint16_t>(n));
    put16(e, 46, 40);
    put16(e, 48, static_cast<std::uint16_t>(shnum));
    put16(e, 50, static_cast<std::uint16_t>(shnum - 1));
    auto shdr = [&](unsigned i, std::uint32_t nameOff, std::uint32_t type, std::uint32_t flags, std::uint32_t addr,
                    std::size_t fileOff, std::uint32_t size) {
        const std::size_t p = shOff + i * 40;
        put32(e, p, nameOff);
        put32(e, p + 4, type);
        put32(e, p + 8, flags);
        put32(e, p + 12, addr);
        put32(e, p + 16, static_cast<std::uint32_t>(fileOff));
        put32(e, p + 20, size);
        put32(e, p + 32, type == elf::SHT_STRTAB ? 1 : 16);
    };
    for (unsigned i = 0; i < n; ++i) {
        const RamRegion& r = regions[i];
        const std::uint32_t size = r.end - r.start;
        const bool last = i + 1 == n;
        const std::size_t p = 52 + i * 32;
        put32(e, p, elf::PT_LOAD);
        put32(e, p + 4, static_cast<std::uint32_t>(offsets[i]));
        put32(e, p + 8, r.start);
        put32(e, p + 12, r.start);
        put32(e, p + 16, size);
        put32(e, p + 20, size + (last ? bssSize : 0));
        put32(e, p + 24, r.code ? (elf::PF_R | elf::PF_X) : (elf::PF_R | elf::PF_W));
        put32(e, p + 28, 16);
        std::copy(ram.begin() + r.start, ram.begin() + r.end, e.begin() + static_cast<std::ptrdiff_t>(offsets[i]));
        shdr(i + 1, r.code ? 1 : 7, elf::SHT_PROGBITS,
             r.code ? (elf::SHF_ALLOC | elf::SHF_EXECINSTR) : (elf::SHF_ALLOC | elf::SHF_WRITE), r.start, offsets[i],
             size);
    }
    shdr(n + 1, 13, bssSize ? elf::SHT_NOBITS : elf::SHT_NULL, elf::SHF_ALLOC | elf::SHF_WRITE, lastEnd,
         offsets.back() + (regions.back().end - regions.back().start), bssSize);
    std::copy(shstr.begin(), shstr.end(), e.begin() + static_cast<std::ptrdiff_t>(strOff));
    shdr(n + 2, 18, elf::SHT_STRTAB, 0, 0, strOff, static_cast<std::uint32_t>(shstr.size()));
    return e;
}

int cmdRam2Elf(const std::vector<std::string_view>& args) {
    std::optional<std::string> path, out;
    std::optional<std::uint32_t> entry;
    std::vector<RamRegion> regions;
    std::uint32_t bssEnd = 0;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto a = args[i];
        auto next = [&]() -> std::string_view {
            if (i + 1 >= args.size()) throw Error("faltou valor para " + std::string(a));
            return args[++i];
        };
        if (a == "-o" || a == "--output") out = std::string(next());
        else if (a == "--entry") entry = parseNumber(next());
        else if (a == "--text" || a == "--data") {
            RamRegion r;
            r.code = a == "--text";
            r.start = parseNumber(next());
            r.end = parseNumber(next());
            regions.push_back(r);
        } else if (a == "--bss-end") bssEnd = parseNumber(next());
        else if (!path) path = std::string(a);
        else throw Error("argumento inesperado: " + std::string(a));
    }
    const bool anyCode = std::any_of(regions.begin(), regions.end(), [](const RamRegion& r) { return r.code; });
    if (!path || !out || !entry || !anyCode) {
        throw Error("uso: anyps2 ram2elf <ram.bin> --entry ENDERECO --text INICIO FIM [--text ...] "
                    "[--data INICIO FIM ...] [--bss-end FIM] -o <saida.elf>");
    }
    std::ifstream in(*path, std::ios::binary);
    if (!in) throw Error("não foi possível abrir '" + *path + "'");
    std::vector<std::uint8_t> ram((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::sort(regions.begin(), regions.end(), [](const RamRegion& x, const RamRegion& y) { return x.start < y.start; });
    bool entryInCode = false;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        const RamRegion& r = regions[i];
        const char* what = r.code ? "--text" : "--data";
        if (r.end <= r.start) throw Error(std::string(what) + ": FIM deve ser maior que INICIO");
        if ((r.start & 3) || (r.end & 3)) throw Error(std::string(what) + ": as faixas precisam ser alinhadas a 4 bytes");
        if (r.end > ram.size()) {
            throw Error(std::string(what) + ": " + hex(r.end) + " passa do fim da imagem (" + hex(ram.size()) + ")");
        }
        if (i > 0 && r.start < regions[i - 1].end) {
            throw Error("as faixas " + hex(regions[i - 1].start) + ".." + hex(regions[i - 1].end) + " e " +
                        hex(r.start) + ".." + hex(r.end) + " se sobrepõem");
        }
        if (r.code && *entry >= r.start && *entry < r.end) entryInCode = true;
    }
    if (!entryInCode) throw Error("--entry " + hex(*entry) + " fora das faixas --text");
    const auto bytes = buildRamElf(ram, *entry, regions, bssEnd);
    std::ofstream o(*out, std::ios::binary);
    o.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    o.close();
    if (!o) throw Error("não foi possível gravar '" + *out + "'");
    // Confere com o próprio parser.
    const auto f = elf::ElfFile::loadFromFile(*out);
    std::cout << "ELF gerado: " << *out << " (entrada " << hex(f.entry()) << ", " << f.segments().size()
              << " segmento(s))\n";
    return 0;
}

int cmdRecomp(const std::vector<std::string_view>& args) {
    std::optional<std::string> path, out, name, vuDumps;
    std::vector<std::string> extras;
    bool noVu = false;
    std::string root = ANYPS2_SOURCE_DIR;
    anyps2::analysis::AnalysisOptions aopt;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto a = args[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= args.size()) throw Error("faltou valor para " + std::string(a));
            return std::string(args[++i]);
        };
        if (a == "-o" || a == "--output") out = next();
        else if (a == "--name") name = next();
        else if (a == "--root") root = next();
        else if (a == "--function") aopt.extraFunctions.push_back(parseNumber(next()));
        else if (a == "--vu-dumps") vuDumps = next();
        else if (a == "--no-vu") noVu = true;
        else if (a == "--extra") extras.push_back(next());
        else if (!path) path = std::string(a);
        else throw Error("argumento inesperado: " + std::string(a));
    }
    if (!path || !out) throw Error("uso: anyps2 recomp <arquivo.elf> -o <diretório> [--name NOME]");
    const auto f = elf::ElfFile::loadFromFile(*path);
    if (!f.isR5900()) {
        std::cerr << "anyps2: aviso: " << f.name() << " não declara a máquina R5900 em e_flags\n";
    }
    const auto model = anyps2::analysis::analyze(f, aopt);
    anyps2::codegen::GeneratorOptions gopt;
    gopt.outputDir = *out;
    gopt.anyps2Root = root;
    gopt.projectName = sanitizeName(name ? *name : std::filesystem::path(*path).stem().string());
    gopt.recompileVu = !noVu;
    if (vuDumps) gopt.vuDumps = listDumps(*vuDumps);
    std::vector<elf::ElfFile> extraElfs;
    std::vector<anyps2::analysis::ProgramModel> extraModels;
    extraElfs.reserve(extras.size());
    extraModels.reserve(extras.size());
    for (const auto& x : extras) {
        extraElfs.push_back(elf::ElfFile::loadFromFile(x));
        extraModels.push_back(anyps2::analysis::analyze(extraElfs.back(), aopt));
        gopt.extraCode.push_back({&extraElfs.back(), &extraModels.back()});
    }
    const auto report = anyps2::codegen::generateProject(f, model, gopt);

    std::cout << "Projeto gerado em " << *out << " (alvo '" << gopt.projectName << "')\n";
    std::cout << "  funções:     " << report.functions << "\n";
    std::cout << "  instruções:  " << report.instructions << "\n";
    std::cout << "  arquivos C++:" << " " << report.sourceFiles << "\n";
    std::cout << "  microcódigo de VU: " << report.vuBlocks << " bloco(s), " << report.vuPairs << " pares\n";
    if (report.invalidWords) {
        std::cout << "  palavras inválidas dentro de funções: " << report.invalidWords
                  << " (lançam erro se executadas)\n";
    }
    if (!report.unsupported.empty()) {
        std::cout << "  instruções ainda não suportadas (lançam erro se executadas):\n";
        for (const auto& [mnemonic, count] : report.unsupported) {
            std::cout << "    " << mnemonic << ": " << count << "\n";
        }
    }
    for (const auto& w : report.warnings) {
        std::cout << "  aviso " << hex(w.address) << ": " << w.message << "\n";
    }
    std::cout << "Compile com:\n  cmake -S " << *out << " -B " << *out << "/build -DCMAKE_BUILD_TYPE=Release\n"
              << "  cmake --build " << *out << "/build\n";
    return 0;
}

int cmdDisc(const std::vector<std::string_view>& args) {
    if (args.size() != 1) throw Error("uso: anyps2 disc <imagem.iso>");
    anyps2::disc::print(anyps2::disc::triage(std::string(args[0])), std::cout);
    return 0;
}

int run(int argc, char** argv) {
    if (argc < 2) {
        printUsage();
        return 1;
    }
    const std::string_view cmd = argv[1];
    std::vector<std::string_view> args(argv + 2, argv + argc);
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        printUsage();
        return 0;
    }
    if (cmd == "--version") {
        std::cout << "anyps2 " << kVersion << "\n";
        return 0;
    }
    if (cmd == "info") return cmdInfo(args);
    if (cmd == "symbols") return cmdSymbols(args);
    if (cmd == "disasm") return cmdDisasm(args);
    if (cmd == "disasm-bin") return cmdDisasmBin(args);
    if (cmd == "recomp") return cmdRecomp(args);
    if (cmd == "vu") return cmdVu(args);
    if (cmd == "vu-gen") return cmdVuGen(args);
    if (cmd == "disc") return cmdDisc(args);
    if (cmd == "ram2elf") return cmdRam2Elf(args);
    throw Error("comando desconhecido '" + std::string(cmd) + "' (use --help)");
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);  // mensagens em português (UTF-8)
#endif
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::fflush(stdout);
        std::cerr << "anyps2: erro: " << e.what() << "\n";
        return 2;
    }
}
