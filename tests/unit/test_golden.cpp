// Teste diferencial contra o GNU objdump (-m mips:5900 -M no-aliases).
//
// O arquivo tests/data/r5900_golden.txt foi gerado por
// tests/scripts/objdump_oracle.py. Para cada palavra:
//
//  * objdump decodificou   -> nós também, de forma canônica e com texto idêntico
//                             (exceto as divergências documentadas abaixo);
//  * objdump recusou       -> nós dizemos Invalid, ou decodificamos marcando
//                             a palavra como não canônica (o hardware ignora
//                             bits reservados; o binutils é estrito).
//
// A variável de ambiente ANYPS2_GOLDEN_FILE troca o arquivo (usado pelo
// modo "check" do script, que gera centenas de milhares de palavras).

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>

#include "anyps2/r5900/decoder.h"
#include "anyps2/r5900/disassembler.h"
#include "minitest.h"

using namespace anyps2::r5900;

namespace {

struct GoldenLine {
    std::uint32_t address;
    std::uint32_t word;
    std::string text;
};

std::string goldenPath() {
    if (const char* env = std::getenv("ANYPS2_GOLDEN_FILE")) return env;
    return std::string(ANYPS2_TEST_DATA_DIR) + "/r5900_golden.txt";
}

std::vector<GoldenLine> loadGolden(const std::string& path) {
    std::ifstream in(path);
    std::vector<GoldenLine> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        GoldenLine g;
        g.address = static_cast<std::uint32_t>(std::stoul(line.substr(0, 8), nullptr, 16));
        g.word = static_cast<std::uint32_t>(std::stoul(line.substr(9, 8), nullptr, 16));
        g.text = line.substr(18);
        lines.push_back(std::move(g));
    }
    return lines;
}

std::string mnemonicOf(const std::string& text) {
    return text.substr(0, text.find('\t'));
}

// O objdump rejeita a palavra (".word" ou a forma genérica "cN 0x...").
bool gnuRejects(const std::string& text) {
    if (text.rfind(".word", 0) == 0) return true;
    return text.size() > 3 && text[0] == 'c' && text[1] >= '0' && text[1] <= '3' &&
           text[2] == '\t';
}

// Instruções que o binutils aceita em modo mips:5900 mas que NÃO existem no
// R5900 (o binutils herda do MIPS III/MIPS I sem excluí-las para o EE).
// Referência: EE Core Instruction Set Manual, mapas de opcode.
const char* gnuOnlyReason(std::uint32_t word) {
    const std::uint32_t op = word >> 26;
    const std::uint32_t rs = (word >> 21) & 0x1F;
    const std::uint32_t funct = word & 0x3F;
    if (op == 0x1D) return "JALX (MIPS16) não existe no R5900";
    if (op == 0x10 && (rs == 0x02 || rs == 0x06)) return "CFC0/CTC0 não existem no R5900";
    if (op == 0x10 && rs == 0x10 && funct == 0x20) return "WAIT não existe no R5900";
    if (op == 0x11 && rs == 0x10) return "formato S do COP1 sem equivalente no R5900";
    if (op == 0x11 && rs >= 0x11) return "formatos D/L do COP1 não existem no R5900 (FPU só simples)";
    return nullptr;
}

// Normaliza a saída do objdump para a nossa convenção. Cada regra é uma
// divergência conhecida e intencional:
//
//  1. Aliases que o binutils imprime mesmo com -M no-aliases:
//     dli rt,imm -> ori rt,zero,imm ; neg/negu/dneg/dnegu rd,rt -> sub/subu/
//     dsub/dsubu rd,zero,rt.
//  2. CVT.W.S: o binutils chama de trunc.w.s (no R5900 a conversão sempre
//     trunca); usamos o nome do manual da Sony.
//  3. VADDA e VMSUBA (sem broadcast): o binutils imprime ft antes de fs, ao
//     contrário de todas as outras instruções com ACC; mantemos ACC,fs,ft.
std::string normalizeGnu(std::string text) {
    const std::string m = mnemonicOf(text);
    std::string ops = text.size() > m.size() ? text.substr(m.size() + 1) : "";
    auto split = [](const std::string& str) {
        std::vector<std::string> parts;
        std::size_t start = 0;
        for (;;) {
            const std::size_t comma = str.find(',', start);
            parts.push_back(str.substr(start, comma - start));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        return parts;
    };
    if (m == "dli") {
        const auto p = split(ops);
        return "ori\t" + p[0] + ",zero," + p[1];
    }
    static const std::map<std::string, std::string> kNeg = {
        {"neg", "sub"}, {"negu", "subu"}, {"dneg", "dsub"}, {"dnegu", "dsubu"}};
    if (auto it = kNeg.find(m); it != kNeg.end()) {
        const auto p = split(ops);
        return it->second + "\t" + p[0] + ",zero," + p[1];
    }
    if (m == "trunc.w.s") return "cvt.w.s\t" + ops;
    if (m.rfind("vadda.", 0) == 0 || m.rfind("vmsuba.", 0) == 0) {
        const auto p = split(ops);
        return m + "\t" + p[0] + "," + p[2] + "," + p[1];
    }
    return text;
}

// Divergências em que o binutils discorda do hardware. Retorna true se a
// palavra foi tratada aqui (com `problem` preenchido em caso de erro).
//
//  * SQRT.S: o binutils lê o operando dos bits 15..11 (fs); o R5900 usa os
//    bits 20..16 (ft), conforme o manual do EE e o comportamento validado do
//    PCSX2 (SQRT_S lê _Ft_). Verificamos contra a nossa especificação.
//  * VSQRT: o binutils só aceita fsf == 1 (bits 22..21), mas o campo é
//    ignorado pelo hardware; não usamos o veredito dele para fsf.
bool hardwareDivergence(const Instruction& insn, const std::string& ours, std::string& problem) {
    const std::uint32_t w = insn.raw;
    char buf[96];
    if ((w >> 26) == 0x11 && ((w >> 21) & 0x1F) == 0x10 && (w & 0x3F) == 0x04) {
        std::snprintf(buf, sizeof(buf), "sqrt.s\t$f%u,$f%u", insn.fd(), insn.ft());
        if (insn.op != Op::SQRT_S) problem = "deveria ser SQRT.S";
        else if (ours != buf) problem = std::string("esperado \"") + buf + "\", obtido \"" + ours + "\"";
        else if (insn.canonical != (insn.fs() == 0)) problem = "canonicidade errada para SQRT.S";
        return true;
    }
    if (insn.op == Op::VSQRT) {
        static constexpr char kComp[] = "xyzw";
        std::snprintf(buf, sizeof(buf), "vsqrt\t$Q,$vf%u%c", insn.vuFt(), kComp[insn.vuFtf()]);
        if (ours != buf) problem = std::string("esperado \"") + buf + "\", obtido \"" + ours + "\"";
        else if (insn.canonical != (insn.vuFs() == 0)) problem = "canonicidade errada para VSQRT";
        return true;
    }
    return false;
}

}  // namespace

TEST_CASE(golden, objdump_r5900) {
    const std::string path = goldenPath();
    const auto lines = loadGolden(path);
    REQUIRE(!lines.empty());

    std::size_t checkedValid = 0, checkedInvalid = 0, gnuOnly = 0, divergences = 0, failures = 0;
    for (const auto& g : lines) {
        const Instruction insn = decode(g.word, g.address);
        const std::string ours = disassemble(insn).str();
        std::string problem;

        if (hardwareDivergence(insn, ours, problem)) {
            ++divergences;
        } else if (gnuRejects(g.text)) {
            ++checkedInvalid;
            if (insn.valid() && insn.canonical) {
                problem = "objdump rejeita, mas decodificamos como canônica: " + ours;
            }
        } else if (gnuOnlyReason(g.word) && (normalizeGnu(g.text) != ours || !insn.valid())) {
            ++gnuOnly;
            if (insn.valid()) {
                problem = std::string("deveria ser inválida (") + gnuOnlyReason(g.word) +
                          "), obtivemos: " + ours;
            }
        } else {
            ++checkedValid;
            const std::string expected = normalizeGnu(g.text);
            if (!insn.valid()) {
                problem = "objdump decodifica, nós não";
            } else if (!insn.canonical) {
                problem = "objdump decodifica, mas marcamos como não canônica: " + ours;
            } else if (ours != expected) {
                problem = "texto difere: obtido \"" + ours + "\"";
            }
        }

        if (!problem.empty()) {
            if (++failures <= 60) {
                char head[64];
                std::snprintf(head, sizeof(head), "%08x %08x ", g.address, g.word);
                CHECK_MSG(false, std::string(head) + "esperado \"" + g.text + "\": " + problem);
            }
        }
    }
    if (failures > 60) {
        CHECK_MSG(false, std::to_string(failures) + " divergências no total");
    }
    std::printf(
        "  golden: %zu palavras (%zu válidas, %zu inválidas, %zu só-binutils, %zu divergências "
        "documentadas) em %s\n",
        lines.size(), checkedValid, checkedInvalid, gnuOnly, divergences, path.c_str());
}
