// Decodificador/disassembler de microcódigo do VU contra o golden do
// dvp-objdump (tests/data/vu_golden.txt, gerado por tests/scripts/vu_oracle.py).

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "anyps2/vu/isa.h"
#include "anyps2/vu/scan.h"
#include "minitest.h"

using namespace anyps2::vu;

namespace {
struct GoldenStats {
    int total = 0, mismatchUpper = 0, mismatchLower = 0, shown = 0;
};
}  // namespace

TEST_CASE(vu_isa, golden_dvp_objdump) {
    std::ifstream in(std::string(ANYPS2_TEST_DATA_DIR) + "/vu_golden.txt");
    REQUIRE(in.good());
    std::string line;
    GoldenStats st;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string addrS, loS, upS;
        ls >> addrS >> loS >> upS;
        const auto tab1 = line.find('\t');
        const auto tab2 = line.find('\t', tab1 + 1);
        const std::string refUpper = line.substr(tab1 + 1, tab2 - tab1 - 1);
        const std::string refLower = line.substr(tab2 + 1);
        const auto pc = static_cast<std::uint32_t>(std::stoul(addrS, nullptr, 16));
        const Instr ins = decode(static_cast<std::uint32_t>(std::stoul(loS, nullptr, 16)),
                                 static_cast<std::uint32_t>(std::stoul(upS, nullptr, 16)));
        ++st.total;
        // Referência inválida: nós devemos dizer inválido ou não canônico.
        auto check = [&](bool refInvalid, bool weInvalid, bool canonical, const std::string& ours,
                         const std::string& ref, int& counter, const char* what) {
            bool ok;
            if (refInvalid) ok = weInvalid || !canonical;
            else ok = canonical && ours == ref;
            if (!ok) {
                ++counter;
                if (st.shown++ < 25) {
                    std::printf("  %s %s %s %s: nosso [%s]%s, ref [%s]\n", what, addrS.c_str(), loS.c_str(),
                                upS.c_str(), ours.c_str(), canonical ? "" : " (não canônico)", ref.c_str());
                }
            }
        };
        check(refUpper == "*unknown*", ins.upper.op == U::INVALID, ins.upper.canonical, upperText(ins), refUpper,
              st.mismatchUpper, "upper");
        check(refLower == "*unknown*", ins.lower.op == L::INVALID, ins.lower.canonical, lowerText(ins, pc),
              refLower, st.mismatchLower, "lower");
    }
    CHECK(st.total > 10000);
    CHECK_EQ(st.mismatchUpper, 0);
    CHECK_EQ(st.mismatchLower, 0);
}

TEST_CASE(vu_isa, fields_and_branches) {
    // add.xyzw vf01,vf02,vf03 | iaddiu vi01,vi00,5
    Instr a = decode(0x10010005u, 0x01e31068u);
    CHECK(a.upper.op == U::ADD);
    CHECK_EQ(a.upper.dest, 0xF);
    CHECK_EQ(a.upper.fd, 1);
    CHECK_EQ(a.upper.fs, 2);
    CHECK_EQ(a.upper.ft, 3);
    CHECK(a.lower.op == L::IADDIU);
    CHECK_EQ(a.lower.imm, 5);
    // ibne vi01,vi02,-5 em pc 0x20 → 0x20 + 8 − 40 = 0
    Instr b = decode((0x29u << 25) | (1u << 16) | (2u << 11) | 0x7FBu, 0x000002FFu);
    CHECK(b.lower.op == L::IBNE);
    CHECK(isBranch(b.lower.op) && isConditionalBranch(b.lower.op));
    CHECK_EQ(branchTarget(b, 0x20), 0u);
    // [E] e LOI
    Instr c = decode(0x3fc00000u, 0xC0000000u | 0x000002FFu);
    CHECK(c.e && c.i);
    CHECK(c.lower.op == L::LOI);
    CHECK_EQ(lowerText(c, 0), std::string("loi 1.5"));
    CHECK_EQ(disassemble(decode(0x8000033Cu, 0x400002FFu), 0), std::string("nop[e] \tnop"));
}

// Localizador de microcódigo: pacotes MPG (com endereço de carga, inclusive
// vários MPG seguidos), blocos crus e rejeição de dados que só parecem código.
TEST_CASE(vu_isa, find_microcode) {
    using anyps2::vu::findMicrocode;
    constexpr std::uint32_t kL = 0x8000033Cu, kU = 0x000002FFu, kE = 1u << 30;
    // Programa: iaddiu vi1,vi0,3 / add.xyzw vf3,vf1,vf2 ; nop[e] ; nop
    const std::uint32_t prog[] = {0x10010003u, 0x01E208E8u, kL, kU | kE, kL, kU};
    std::vector<std::uint32_t> mem(64, 0xFFFFFFFFu);  // lixo não canônico
    auto put = [&](std::size_t word, const std::uint32_t* w, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) mem[word + i] = w[i];
    };
    // MPG em palavra ímpar: NUM=2 em 0x10 (unidades de 8 bytes), depois NOP e
    // um segundo MPG NUM=1 contíguo (0x12) → um bloco só.
    mem[1] = 0x4A020010u;
    put(2, prog, 4);
    mem[6] = 0;            // VIFcode NOP
    mem[7] = 0x4A010012u;
    put(8, prog + 4, 2);
    // Bloco cru (sem MPG), alinhado a 8 bytes.
    put(20, prog, 6);
    // Dados que decodificam mas não fazem nada (máscara vazia, bit E em todo
    // par): não podem virar bloco.
    const std::uint32_t junk[] = {0, 0x40842800u, 0, 0x40855000u, 0, 0x40861000u};
    put(40, junk, 6);
    const auto blobs =
        findMicrocode(reinterpret_cast<const std::uint8_t*>(mem.data()), mem.size() * 4, 0x00100000u);
    CHECK_EQ(blobs.size(), 1u);  // o bloco cru é idêntico ao do MPG: aparece uma vez
    if (blobs.size() == 1) {
        CHECK(blobs[0].fromMpg);
        CHECK_EQ(blobs[0].loadAddress, 0x80u);
        CHECK_EQ(blobs[0].address, 0x00100008u);
        CHECK_EQ(blobs[0].words.size(), 6u);
        CHECK_EQ(blobs[0].words[1], 0x01E208E8u);
    }
    // Só o bloco cru (outro programa): endereço de carga desconhecido.
    std::vector<std::uint32_t> raw(16, 0xFFFFFFFFu);
    for (std::size_t i = 0; i < 6; ++i) raw[4 + i] = prog[i];
    raw[4] = 0x10010004u;  // iaddiu vi1,vi0,4
    const auto r = findMicrocode(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size() * 4, 0);
    CHECK_EQ(r.size(), 1u);
    if (r.size() == 1) {
        CHECK(!r[0].fromMpg);
        CHECK_EQ(r[0].loadAddress, anyps2::vu::kUnknownLoad);
        CHECK_EQ(r[0].address, 16u);
    }
    // Sem bit E não é programa.
    for (std::size_t i = 0; i < 6; ++i) raw[4 + i] = i % 2 ? kU : 0x10010004u;
    CHECK_EQ(findMicrocode(reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size() * 4, 0).size(), 0u);
    // MPG cujo código não decodifica é ignorado.
    std::vector<std::uint32_t> bad = {0, 0x4A010000u, 0xFFFFFFFFu, 0xFFFFFFFFu};
    CHECK_EQ(findMicrocode(reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size() * 4, 0).size(), 0u);
}
