// Testes da análise de funções e do gerador de C++ sobre o fixture
// hello_r5900.elf (GNU as -march=r5900).

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include "anyps2/analysis/analyzer.h"
#include "anyps2/codegen/generator.h"
#include "anyps2/codegen/vu_generator.h"
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
    // Macroinstrução do VU0 (vadd.xyzw vf2, vf1, vf1 = 0x4BE108A8): vai para o
    // núcleo do VU como palavra upper do microcódigo.
    CHECK(contains(mainCode, "vu0Macro(c, 0x8000033C"));
    CHECK(contains(mainCode, "0x01E108A8"));
    CHECK_EQ(report.unsupported.count("vadd"), 0u);
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
    // No Windows não dá para apagar arquivos ainda abertos.
    in.close();
    prog.close();
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

// Microcódigo de VU: um bloco vira trechos com um rótulo por par, conferência
// do par na micro memória e o núcleo especializado pelas operações; a tabela
// leva o endereço de carga (se conhecido) e as palavras para o casamento.
TEST_CASE(codegen, vu_microcode) {
    anyps2::vu::MicroBlob b;
    b.address = 0x00123450;
    b.loadAddress = 0x80;
    b.fromMpg = true;
    // iaddiu vi1,vi0,3 / add.xyzw vf3,vf1,vf2 ; ibne vi1,vi0,-2 / nop ; nop[e] ; nop
    b.words = {0x10010003u, 0x01E208E8u, 0x520107FEu, 0x000002FFu, 0x8000033Cu, 0x400002FFu,
               0x8000033Cu, 0x000002FFu};
    const auto files = anyps2::codegen::generateVuSources({b}, "kT");
    CHECK_EQ(files.size(), 2u);
    if (files.size() != 2) return;
    const std::string& code = files[0].text;
    CHECK_EQ(files[0].name, std::string("vu_000.cpp"));
    CHECK(code.find("vu.pairIs(base + 0x0008u, 0x520107FEu, 0x000002FFu)") != std::string::npos);
    CHECK(code.find("vu.step<static_cast<U>(0), static_cast<L>(4)>") != std::string::npos);  // ADD + IADDIU
    CHECK(code.find("case 0x0018u: goto p_0018;") != std::string::npos);
    CHECK(code.find("void kT_block_0(") != std::string::npos);
    const std::string& table = files[1].text;
    CHECK_EQ(files[1].name, std::string("vu_programs.cpp"));
    CHECK(table.find("{0x00000080u, kT_words_0, 8u, &kT_block_0") != std::string::npos);
    CHECK(table.find("extern const std::size_t kTCount = 1;") != std::string::npos);
    // Sem microcódigo: tabela com entrada nula e Count = 0.
    const auto none = anyps2::codegen::generateVuSources({}, "kT");
    CHECK_EQ(none.size(), 1u);
    CHECK(none[0].text.find("kTCount = 0;") != std::string::npos);
}
