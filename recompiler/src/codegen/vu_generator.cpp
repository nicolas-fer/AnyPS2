#include "anyps2/codegen/vu_generator.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <sstream>

#include "anyps2/common/error.h"
#include "anyps2/vu/isa.h"

namespace anyps2::codegen {

namespace {

std::string hex32(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08Xu", v);
    return buf;
}

std::string off16(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%04Xu", v);
    return buf;
}

std::string label(std::uint32_t off) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "p_%04X", off);
    return buf;
}

// Inicializador constexpr de vu::Instr (mesma ordem dos campos de isa.h).
std::string instrInit(const vu::Instr& in) {
    const vu::Upper& u = in.upper;
    const vu::Lower& l = in.lower;
    std::ostringstream s;
    auto b = [](bool v) { return v ? "true" : "false"; };
    s << "{" << hex32(in.lowerWord) << ", " << hex32(in.upperWord) << ", "
      << "Upper{static_cast<U>(" << static_cast<unsigned>(u.op) << "), " << unsigned{u.dest} << ", "
      << unsigned{u.ft} << ", " << unsigned{u.fs} << ", " << unsigned{u.fd} << ", " << unsigned{u.bc} << ", "
      << b(u.canonical) << "}, "
      << "Lower{static_cast<L>(" << static_cast<unsigned>(l.op) << "), " << unsigned{l.dest} << ", "
      << unsigned{l.ft} << ", " << unsigned{l.fs} << ", " << unsigned{l.fd} << ", " << unsigned{l.fsf} << ", "
      << unsigned{l.ftf} << ", " << l.imm << ", " << b(l.canonical) << "}, " << b(in.i) << ", " << b(in.e)
      << ", " << b(in.m) << ", " << b(in.d) << ", " << b(in.t) << "}";
    return s.str();
}

// Texto seguro para comentário de linha.
std::string comment(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    return s;
}

std::string blockSymbol(const std::string& table, std::size_t i) {
    return table + "_block_" + std::to_string(i);
}
std::string wordsSymbol(const std::string& table, std::size_t i) {
    return table + "_words_" + std::to_string(i);
}

// O bloco é dividido em trechos de até kChunkPairs pares. Cada trecho é uma
// função com um rótulo por par e fall-through entre pares consecutivos
// (desvios voltam ao switch do trecho); o bloco passa de um trecho a outro
// por uma tabela. Funções de tamanho limitado mantêm o tempo de compilação
// linear no tamanho do microprograma (uma função única com milhares de pares
// leva minutos no GCC).
constexpr std::uint32_t kChunkPairs = 64;

void emitBlock(std::ostringstream& o, const vu::MicroBlob& blob, const std::string& table, std::size_t index) {
    const std::uint32_t pairs = static_cast<std::uint32_t>(blob.words.size() / 2);
    const std::uint32_t chunks = (pairs + kChunkPairs - 1) / kChunkPairs;
    const std::string prefix = blockSymbol(table, index);
    o << "// Bloco " << index << ": " << describeBlob(blob) << ", " << pairs << " pares.\n";
    o << "extern const std::uint32_t " << wordsSymbol(table, index) << "[] = {";
    for (std::size_t w = 0; w < blob.words.size(); ++w) {
        o << (w % 8 == 0 ? "\n    " : " ") << hex32(blob.words[w]) << ",";
    }
    o << "\n};\n\nnamespace {\n";
    for (std::uint32_t c = 0; c < chunks; ++c) {
        const std::uint32_t first = c * kChunkPairs, last = std::min(pairs, first + kChunkPairs);
        o << "void " << prefix << "_c" << c
          << "(::anyps2::rt::Vu& vu, std::uint32_t base, ::anyps2::rt::VuCursor& cur) {\n"
             "dispatch:\n"
             "    switch (cur.pc - base) {\n";
        for (std::uint32_t p = first; p < last; ++p) {
            o << "        case " << off16(p * 8) << ": goto " << label(p * 8) << ";\n";
        }
        o << "        default: return;  // fora deste trecho\n    }\n";
        for (std::uint32_t p = first; p < last; ++p) {
            const std::uint32_t off = p * 8;
            const std::uint32_t lo = blob.words[p * 2], up = blob.words[p * 2 + 1];
            const vu::Instr in = vu::decode(lo, up);
            o << label(off) << ": {  // " << comment(vu::disassemble(in, off)) << "\n"
              << "    if (!vu.pairIs(base + " << off16(off) << ", " << hex32(lo) << ", " << hex32(up)
              << ")) return;\n"
              << "    static constexpr Instr k" << instrInit(in) << ";\n"
              << "    vu.step<static_cast<U>(" << static_cast<unsigned>(in.upper.op) << "), static_cast<L>("
              << static_cast<unsigned>(in.lower.op) << ")>(k, base + " << off16(off) << ", cur);\n"
              << "    if (cur.done) return;\n";
            if (p + 1 < last) {
                o << "    if (cur.pc != base + " << off16(off + 8) << ") goto dispatch;\n";
            } else {
                o << "    goto dispatch;\n";
            }
            o << "}\n";
        }
        o << "}\n\n";
    }
    o << "}  // namespace\n\n"
      << "void " << prefix << "(::anyps2::rt::Vu& vu, std::uint32_t base, ::anyps2::rt::VuCursor& cur) {\n"
      << "    using ChunkFn = void (*)(::anyps2::rt::Vu&, std::uint32_t, ::anyps2::rt::VuCursor&);\n"
      << "    static constexpr ChunkFn kChunks[] = {";
    for (std::uint32_t c = 0; c < chunks; ++c) {
        o << (c % 4 == 0 ? "\n        " : " ") << "&" << prefix << "_c" << c << ",";
    }
    o << "\n    };\n"
      << "    // Volta ao runtime ao sair do bloco, num par alterado ou no fim.\n"
      << "    for (;;) {\n"
      << "        const std::uint32_t off = cur.pc - base;\n"
      << "        if (off >= " << off16(pairs * 8) << ") return;\n"
      << "        const std::uint32_t before = cur.pc;\n"
      << "        kChunks[off / " << off16(kChunkPairs * 8) << "](vu, base, cur);\n"
      << "        if (cur.done || cur.pc == before) return;\n"
      << "    }\n}\n\n";
}

std::string fileHeader() {
    return "// Gerado pelo AnyPS2 (microcódigo dos VUs) — não edite.\n"
           "#include <cstddef>\n#include <cstdint>\n\n"
           "#include \"anyps2/runtime/vu/vu_exec.h\"\n\n"
           "using ::anyps2::vu::Instr;\nusing ::anyps2::vu::L;\nusing ::anyps2::vu::Lower;\n"
           "using ::anyps2::vu::U;\nusing ::anyps2::vu::Upper;\n\n";
}

}  // namespace

std::string describeBlob(const vu::MicroBlob& blob) {
    char buf[96];
    if (blob.loadAddress != vu::kUnknownLoad) {
        std::snprintf(buf, sizeof(buf), "%s em 0x%08X -> micro 0x%04X", blob.fromMpg ? "MPG" : "dump",
                      blob.address, blob.loadAddress);
    } else {
        std::snprintf(buf, sizeof(buf), "dados em 0x%08X", blob.address);
    }
    return buf;
}

std::vector<vu::MicroBlob> findElfMicrocode(const elf::ElfFile& elf) {
    std::vector<vu::MicroBlob> out;
    for (const auto& s : elf.segments()) {
        if (!s.isLoad() || s.filesz == 0) continue;
        const auto data = elf.segmentData(s);
        vu::appendUnique(out, vu::findMicrocode(data.data(), data.size(), s.vaddr));
    }
    return out;
}

std::vector<vu::MicroBlob> findDumpMicrocode(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) throw anyps2::Error("não foi possível ler " + file.string());
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() != 0x1000 && data.size() != 0x4000) {
        throw anyps2::Error(file.string() + ": dump da micro memória deve ter 4 KB (VU0) ou 16 KB (VU1), tem " +
                            std::to_string(data.size()) + " bytes");
    }
    auto blobs = vu::findMicrocode(data.data(), data.size(), 0);
    for (auto& b : blobs) b.loadAddress = b.address;
    return blobs;
}

std::vector<VuSourceFile> generateVuSources(const std::vector<vu::MicroBlob>& blobs, const std::string& table,
                                            std::size_t pairsPerFile) {
    std::vector<VuSourceFile> files;
    std::ostringstream cur;
    std::size_t curPairs = 0;
    auto flush = [&] {
        char name[32];
        std::snprintf(name, sizeof(name), "vu_%03zu.cpp", files.size());
        files.push_back({name, fileHeader() + cur.str()});
        cur.str("");
        cur.clear();
        curPairs = 0;
    };
    for (std::size_t i = 0; i < blobs.size(); ++i) {
        if (blobs[i].words.size() < 2 || blobs[i].words.size() % 2) {
            throw anyps2::Error("bloco de microcódigo com tamanho ímpar: " + describeBlob(blobs[i]));
        }
        emitBlock(cur, blobs[i], table, i);
        curPairs += blobs[i].words.size() / 2;
        if (curPairs >= pairsPerFile) flush();
    }
    if (curPairs > 0) flush();

    std::ostringstream t;
    t << "// Gerado pelo AnyPS2 (tabela do microcódigo dos VUs) — não edite.\n"
         "#include <cstddef>\n#include <cstdint>\n\n#include \"anyps2/runtime/vu/vu.h\"\n\n";
    for (std::size_t i = 0; i < blobs.size(); ++i) {
        t << "extern const std::uint32_t " << wordsSymbol(table, i) << "[];\n"
          << "void " << blockSymbol(table, i)
          << "(::anyps2::rt::Vu& vu, std::uint32_t base, ::anyps2::rt::VuCursor& cur);\n";
    }
    t << "\nextern const ::anyps2::rt::VuProgramEntry " << table << "[] = {\n";
    for (std::size_t i = 0; i < blobs.size(); ++i) {
        const auto& b = blobs[i];
        t << "    {" << (b.loadAddress == vu::kUnknownLoad ? std::string("::anyps2::rt::kNoLoadHint")
                                                           : hex32(b.loadAddress))
          << ", " << wordsSymbol(table, i) << ", " << b.words.size() << "u, &" << blockSymbol(table, i) << ", \""
          << describeBlob(b) << "\"},\n";
    }
    if (blobs.empty()) t << "    {::anyps2::rt::kNoLoadHint, nullptr, 0u, nullptr, \"\"},\n";
    t << "};\nextern const std::size_t " << table << "Count = " << blobs.size() << ";\n";
    files.push_back({"vu_programs.cpp", t.str()});
    return files;
}

}  // namespace anyps2::codegen
