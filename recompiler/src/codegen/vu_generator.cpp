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
// função com um rótulo por par; o bloco passa de um trecho a outro por uma tabela.
// Funções de tamanho limitado mantêm o tempo de compilação linear no tamanho do
// microprograma (uma função única com milhares de pares leva minutos no GCC).
constexpr std::uint32_t kChunkPairs = 64;

// Fluxo estático dentro do trecho. O cursor (VuCursor) e o contador de pares
// não passam pelo objeto a cada par: o que o gerador sabe em tempo de
// compilação vira código, e só o que depende dos dados vira variável local.
//  * Par sem desvio e sem bit E: cai no rótulo seguinte, sem olhar o pc.
//  * B/BAL/IBxx/JR/JALR e o bit E criam um contexto para o par seguinte (o
//    delay slot). Esse par é emitido de novo, inline, logo depois do par que
//    cria o contexto (cópia do delay slot): ali o destino é conhecido (alvo
//    constante de B/BAL/IBxx, `taken` local do IBxx, alvo dinâmico do JR) e o
//    desvio vira goto direto (ou saída com pc constante, se o alvo está em
//    outro trecho). O rótulo do par continua com o código do fluxo comum,
//    que é o que JR, desvios de fora e o início da execução alcançam.
//  * Desvio ou bit E dentro de um delay slot (contexto aninhado) não tem
//    código estático: o trecho grava o cursor completo e sai, e o próximo
//    par roda no interpretador (stepOne, na entrada do trecho seguinte).
//  * Toda saída grava o cursor completo (pc, hasDelayed, delayedTarget,
//    ending, done) e soma os pares locais ao Vu (addPairs). O pairIs de cada
//    par fica: o findBlock só valida 32 pares adiante. Se ele falha, o
//    cursor gravado é o do contexto daquele par, e o interpretador retoma ali.
//  * O limite de pares (laço infinito) é conferido nas arestas de volta
//    (goto para um rótulo anterior ou igual, JR) e a cada volta do bloco.
struct ChunkEmitter {
    std::ostringstream& o;
    const vu::MicroBlob& blob;
    std::uint32_t pairs;        // pares do bloco
    std::uint32_t first, last;  // pares deste trecho: [first, last)
    bool usesDispatch = false;  // algum par volta ao switch de entrada (JR): só então o rótulo existe (-Wunused-label)

    vu::Instr instrAt(std::uint32_t p) const { return vu::decode(blob.words[p * 2], blob.words[p * 2 + 1]); }

    static std::string ops(const vu::Instr& in) {
        return "<static_cast<U>(" + std::to_string(static_cast<unsigned>(in.upper.op)) + "), static_cast<L>(" +
               std::to_string(static_cast<unsigned>(in.lower.op)) + ")>";
    }

    // Endereço (expressão) do par p do bloco, e do que vem depois dele em sequência (com a volta da
    // micro memória, se o par é o último do bloco, que pode terminar no fim da memória).
    static std::string absPc(std::uint32_t p) { return "base + " + off16(p * 8); }
    std::string nextPc(std::uint32_t p) const {
        return p + 1 < pairs ? absPc(p + 1) : "vu.wrap(base + " + off16(pairs * 8) + ")";
    }
    // Alvo relativo ao início do bloco (em bytes, pode ficar fora do bloco ou negativo): dentro do bloco o
    // findBlock garante que base + alvo cabe na micro memória, fora dele o endereço dá a volta como no runtime.
    std::string targetPc(std::int64_t t) const {
        if (t >= 0 && t < static_cast<std::int64_t>(pairs) * 8) return "base + " + off16(static_cast<std::uint32_t>(t));
        return "vu.wrap(base + " + hex32(static_cast<std::uint32_t>(t)) + ")";
    }
    bool inChunk(std::int64_t t) const {
        return t >= static_cast<std::int64_t>(first) * 8 && t < static_cast<std::int64_t>(last) * 8;
    }

    static std::string exitStr(const std::string& pc, bool done = false) {
        return "cur.pc = " + pc + "; " + (done ? "cur.done = true; " : "") + "vu.addPairs(n); return;";
    }
    void exitTo(const std::string& pc, bool done = false) { o << exitStr(pc, done); }
    // Vai para o alvo t (relativo ao bloco) depois do par `from`: goto no trecho (com o limite de pares nas
    // arestas de volta) ou saída com pc constante.
    void jumpTo(std::int64_t t, std::uint32_t from, const char* indent) {
        if (inChunk(t)) {
            const auto idx = static_cast<std::uint32_t>(t / 8);
            if (idx <= from) {
                o << indent << "if (vu.pairsOver(n)) vu.pairsOverflow(n, " << absPc(idx) << ");\n";
            }
            o << indent << "goto " << label(idx * 8) << ";\n";
        } else {
            o << indent;
            exitTo(targetPc(t));
            o << "\n";
        }
    }
    void goNext(std::uint32_t p, const char* indent) {  // continua no par p+1 (fluxo comum)
        if (p + 1 < last) {
            o << indent << "goto " << label((p + 1) * 8) << ";\n";
        } else {
            o << indent;
            exitTo(nextPc(p));
            o << "\n";
        }
    }

    void pairHeader(std::uint32_t p, const vu::Instr& in, const char* indent, const char* kName, const std::string& failExit) {
        o << indent << "if (!vu.pairIs(" << absPc(p) << ", " << hex32(in.lowerWord) << ", " << hex32(in.upperWord)
          << ")) { " << failExit << " }\n"
          << indent << "static constexpr Instr " << kName << instrInit(in) << ";\n";
    }

    void emitPair(std::uint32_t p) {
        const vu::Instr in = instrAt(p);
        const vu::L op = in.lower.op;
        const bool branch = vu::isBranch(op);
        o << label(p * 8) << ": {  // " << comment(vu::disassemble(in, p * 8)) << "\n";
        pairHeader(p, in, "    ", "k", exitStr(absPc(p)));
        o << "    ::anyps2::rt::Vu::Flow flow;\n"
          << "    vu.execPairOp" << ops(in) << "(k, " << absPc(p) << ", flow);\n"
          << "    ++n;\n";
        if (!branch && !in.e) {
            if (p + 1 >= last) {
                o << "    ";
                exitTo(nextPc(p));
                o << "\n";
            }
            o << "}\n";
            return;
        }
        emitContext(p, in);
        o << "}\n";
    }

    // Par com desvio ou bit E: o par seguinte (delay slot) roda neste contexto.
    void emitContext(std::uint32_t p, const vu::Instr& in) {
        const vu::L op = in.lower.op;
        const bool branch = vu::isBranch(op);
        const bool isCond = vu::isConditionalBranch(op), isInd = vu::isIndirect(op);
        const std::uint32_t q = p + 1;
        const bool hasSlot = q < pairs;
        const std::int64_t target = (branch && !isInd) ? static_cast<std::int64_t>(p) * 8 + 8 + std::int64_t{in.lower.imm} * 8 : 0;
        if (isCond) o << "    const bool taken = flow.taken;\n";
        if (isInd) o << "    const std::uint32_t jt = flow.target;\n";
        const std::string takenExpr = isCond ? "taken" : "true";
        const std::string targetExpr = isInd ? "jt" : targetPc(target);

        // Saída com o cursor completo do contexto (o interpretador retoma no par seguinte).
        const auto cursorExit = [&](const std::string& pc) {
            std::string s = "cur.pc = " + pc + "; ";
            if (branch) s += "cur.hasDelayed = " + takenExpr + "; cur.delayedTarget = " + targetExpr + "; ";
            s += std::string("cur.ending = ") + (in.e ? "true" : "false") + "; vu.addPairs(n); return;";
            return s;
        };

        const vu::Instr slot = hasSlot ? instrAt(q) : vu::Instr{};
        if (!hasSlot || (branch && !in.e && (vu::isBranch(slot.lower.op) || slot.e))) {
            o << "    " << cursorExit(hasSlot ? absPc(q) : nextPc(p)) << "\n";
            return;
        }

        o << "    {  // delay slot de " << comment(vu::lowerText(in, p * 8)) << (in.e ? " (bit E)" : "") << "\n";
        pairHeader(q, slot, "        ", "ks", cursorExit(absPc(q)));
        o << "        ::anyps2::rt::Vu::Flow slotFlow;\n"
          << "        vu.execPairOp" << ops(slot) << "(ks, " << absPc(q) << ", slotFlow);\n"
          << "        ++n;\n";
        if (in.e) {
            // Depois do delay slot o microprograma termina; o pc fica no destino do desvio (ou no par seguinte).
            o << "        ";
            if (!branch) {
                exitTo(nextPc(q), true);
            } else if (isCond) {
                exitTo("taken ? (" + targetPc(target) + ") : (" + nextPc(q) + ")", true);
            } else {
                exitTo(targetExpr, true);
            }
            o << "\n    }\n";
            return;
        }
        if (isInd) {
            usesDispatch = true;
            o << "        pc = jt;\n"
              << "        if (vu.pairsOver(n)) vu.pairsOverflow(n, pc);\n"
              << "        goto dispatch;\n";
        } else if (isCond) {
            o << "        if (taken) {\n";
            jumpTo(target, q, "            ");
            o << "        }\n";
            goNext(q, "        ");
        } else {
            jumpTo(target, q, "        ");
        }
        o << "    }\n";
    }

    // Os pares são emitidos antes, para saber se o rótulo de entrada é usado.
    static void emitChunk(std::ostringstream& out, const vu::MicroBlob& blob, std::uint32_t pairs, std::uint32_t first,
                          std::uint32_t last, const std::string& name) {
        std::ostringstream body;
        ChunkEmitter e{body, blob, pairs, first, last};
        for (std::uint32_t p = first; p < last; ++p) e.emitPair(p);
        out << "void " << name << "(::anyps2::rt::Vu& vu, std::uint32_t base, ::anyps2::rt::VuCursor& cur) {\n"
            << "    // Delay slot ou bit E pendentes na entrada: contexto que o fluxo estático só alcança por dentro do\n"
            << "    // trecho. O interpretador executa um par e o bloco volta aqui.\n"
            << "    if (cur.hasDelayed || cur.ending) {\n"
            << "        vu.stepOne(cur);\n"
            << "        return;\n"
            << "    }\n"
            << "    std::uint64_t n = 0;  // pares executados neste trecho, somados ao Vu nas saídas\n"
            << "    std::uint32_t pc = cur.pc;\n"
            << (e.usesDispatch ? "dispatch:\n" : "") << "    switch (pc - base) {\n";
        for (std::uint32_t p = first; p < last; ++p) {
            out << "        case " << off16(p * 8) << ": goto " << label(p * 8) << ";\n";
        }
        out << "        default: cur.pc = pc; vu.addPairs(n); return;  // fora deste trecho\n    }\n"
            << body.str() << "}\n\n";
    }
};

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
        ChunkEmitter::emitChunk(o, blob, pairs, first, last, prefix + "_c" + std::to_string(c));
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
      << "        if (vu.pairsOver(0)) vu.pairsOverflow(0, cur.pc);  // laço que passa de um trecho a outro\n"
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
