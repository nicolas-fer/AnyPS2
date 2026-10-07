// Testes da análise de funções e do gerador de C++ sobre o fixture
// hello_r5900.elf (GNU as -march=r5900).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "anyps2/analysis/analyzer.h"
#include "anyps2/codegen/generator.h"
#include "anyps2/common/bytes.h"
#include "anyps2/r5900/decoder.h"
#include "anyps2/elf/elf_file.h"
#include "elf_builder.h"
#include "minitest.h"

using namespace anyps2;

namespace {

elf::ElfFile fixtureElf() {
    return elf::ElfFile::loadFromFile(std::string(ANYPS2_TEST_FIXTURES_DIR) + "/hello_r5900.elf");
}

bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

std::string hexU(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08Xu", v);
    return buf;
}

}  // namespace

TEST_CASE(codegen, discovers_functions_from_symbols) {
    const auto f = fixtureElf();
    const auto model = analysis::analyze(f);
    REQUIRE(model.functions.size() == 4);
    const char* names[] = {"_start", "main", "strlen_simple", "callback"};
    for (std::size_t i = 0; i < 4; ++i) {
        const auto* sym = f.findSymbol(names[i]);
        REQUIRE(sym != nullptr);
        CHECK_EQ(model.functions[i].name, std::string(names[i]));
        CHECK_EQ(model.functions[i].start, sym->value);
        CHECK_EQ(model.functions[i].end, sym->value + sym->size);
        CHECK(model.functions[i].fromSymbol);
    }
    CHECK_EQ(model.entry, f.entry());
    CHECK(model.functionContaining(f.findSymbol("main")->value + 8) == &model.functions[1]);
    CHECK(model.functionContaining(0x00200000) == nullptr);
}

TEST_CASE(codegen, labels_and_entries) {
    const auto f = fixtureElf();
    const auto model = analysis::analyze(f);
    const auto& main = model.functions[1];
    const auto& strlenFn = model.functions[2];
    // Retornos de chamadas viram pontos de entrada (reentrada pelo despachante).
    std::size_t returnSites = 0;
    for (std::uint32_t a = main.start; a < main.end; a += 4) {
        const auto insn = r5900::decode(f.read32(a), a);
        if (insn.op == r5900::Op::JAL || insn.op == r5900::Op::JALR) {
            CHECK_MSG(main.entries.count(a + 8) == 1, "retorno de chamada sem ponto de entrada");
            ++returnSites;
        }
    }
    CHECK_EQ(returnSites, 2u);
    // O laço de strlen_simple tem rótulos para o início do laço e a saída.
    CHECK(strlenFn.labels.size() >= 2);
    for (std::uint32_t l : strlenFn.labels) CHECK(strlenFn.contains(l) || l == strlenFn.end);
}

TEST_CASE(codegen, function_body) {
    const auto f = fixtureElf();
    const auto model = analysis::analyze(f);
    codegen::GenerationReport report;
    const std::string mainCode = codegen::generateFunction(f, model, model.functions[1], report);
    const std::uint32_t strlenAddr = model.functions[2].start;
    // jal strlen_simple: chamada direta + verificação do retorno
    CHECK(contains(mainCode, "fn_" + hexU(strlenAddr).substr(2, 8) + "(c);"));
    CHECK(contains(mainCode, ") [[unlikely]] return;"));
    // jalr t9: despacho dinâmico
    CHECK(contains(mainCode, "c->rt->call(c);"));
    // lq/sq/pextlw/FPU viram chamadas para o runtime
    CHECK(contains(mainCode, "LQ(c, 9, 8, 0, "));
    CHECK(contains(mainCode, "PEXTLW(c, 10, 9, 9);"));
    CHECK(contains(mainCode, "ADD_S(c, 2, 1, 1);"));
    // A macroinstrução do VU0 ainda não é suportada: vira erro em execução.
    CHECK(contains(mainCode, "unsupported(c, "));
    CHECK(contains(mainCode, "Fase 5"));
    CHECK_EQ(report.unsupported["vadd"], 1u);
    // Reentrada no meio da função pelo switch de entrada.
    CHECK(contains(mainCode, "switch (c->pc)"));
    CHECK(contains(mainCode, "badEntry(c, " + hexU(model.functions[1].start) + ");"));

    const std::string strlenCode = codegen::generateFunction(f, model, model.functions[2], report);
    CHECK(contains(strlenCode, "goto L_"));
    CHECK(contains(strlenCode, "if (taken) {"));  // beql: delay slot só no caminho tomado
    CHECK(contains(strlenCode, "LBU(c, 8, 4, 0, "));
}

TEST_CASE(codegen, project_files_and_image) {
    namespace fs = std::filesystem;
    const auto f = fixtureElf();
    const auto model = analysis::analyze(f);
    const fs::path out = fs::temp_directory_path() / "anyps2_codegen_test";
    fs::remove_all(out);
    codegen::GeneratorOptions opt;
    opt.outputDir = out;
    opt.anyps2Root = ANYPS2_TEST_FIXTURES_DIR;
    opt.projectName = "fixture";
    const auto report = codegen::generateProject(f, model, opt);
    CHECK_EQ(report.functions, 4u);
    CHECK(fs::exists(out / "CMakeLists.txt"));
    CHECK(fs::exists(out / "src" / "program.cpp"));
    CHECK(fs::exists(out / "src" / "functions.h"));
    CHECK(fs::exists(out / "src" / "functions_000.cpp"));
    // Imagem: cabeçalho + um segmento PT_LOAD com a .text/.data
    std::ifstream in(out / "fixture.image", std::ios::binary);
    std::vector<std::uint8_t> img((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(img.size() > 32);
    CHECK_EQ(std::string(img.begin(), img.begin() + 6), std::string("AP2IMG"));
    CHECK_EQ(anyps2::readLE32(img, 12), f.entry());
    CHECK_EQ(anyps2::readLE32(img, 16), 1u);
    CHECK_EQ(anyps2::readLE32(img, 20), f.segments()[0].vaddr);
    std::ifstream prog(out / "src" / "program.cpp");
    std::string programCpp((std::istreambuf_iterator<char>(prog)), std::istreambuf_iterator<char>());
    CHECK(contains(programCpp, "\"strlen_simple\""));
    CHECK(contains(programCpp, "runProgram(kProgram, argc, argv)"));
    fs::remove_all(out);
}

TEST_CASE(codegen, gaps_without_symbols_become_functions) {
    // Código sem símbolos: o ponto de entrada e os alvos de JAL viram funções.
    auto b = testutil::typicalExecutable();
    b.symbols.clear();
    const auto f = elf::ElfFile::loadFromMemory(b.build(), "stripped.elf");
    const auto model = analysis::analyze(f);
    REQUIRE(model.functions.size() >= 2);
    CHECK_EQ(model.functions[0].start, 0x00100000u);           // entrada
    CHECK(model.functionContaining(0x00100020) != nullptr);    // alvo do jal
    CHECK_EQ(model.functionContaining(0x00100020)->start, 0x00100020u);
    CHECK_EQ(model.functions[0].name, std::string("sub_00100000"));
}
