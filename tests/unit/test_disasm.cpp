// Testes de formatação do disassembler (casos especiais da sintaxe GNU).

#include <string>

#include "anyps2/r5900/decoder.h"
#include "anyps2/r5900/disassembler.h"
#include "anyps2/r5900/registers.h"
#include "minitest.h"

using namespace anyps2::r5900;

namespace {
std::string d(std::uint32_t word, std::uint32_t addr = 0x00100000) {
    return disassembleWord(word, addr);
}
}  // namespace

TEST_CASE(disasm, basic_forms) {
    CHECK_EQ(d(0x00000000), std::string("sll\tzero,zero,0x0"));
    CHECK_EQ(d(0x27BDFFE0), std::string("addiu\tsp,sp,-32"));
    CHECK_EQ(d(0x8FBF0010), std::string("lw\tra,16(sp)"));
    CHECK_EQ(d(0x3C020012), std::string("lui\tv0,0x12"));
    CHECK_EQ(d(0x3442FFFF), std::string("ori\tv0,v0,0xffff"));
    CHECK_EQ(d(0x03E00008), std::string("jr\tra"));
    CHECK_EQ(d(0x78A40010), std::string("lq\ta0,16(a1)"));
    CHECK_EQ(d(0x7CA4FFF0), std::string("sq\ta0,-16(a1)"));
    CHECK_EQ(d(0x70641488), std::string("pextlw\tv0,v1,a0"));
    CHECK_EQ(d(0x706416E8), std::string("qfsrv\tv0,v1,a0"));
}

TEST_CASE(disasm, gnu_special_cases) {
    // jalr com rd = ra omite o destino
    CHECK_EQ(d(0x0320F809), std::string("jalr\tt9"));
    CHECK_EQ(d(0x03203009), std::string("jalr\ta2,t9"));
    // mult/madd de 3 operandos omitem rd quando é zero
    CHECK_EQ(d(0x00850018), std::string("mult\ta0,a1"));
    CHECK_EQ(d(0x00851018), std::string("mult\tv0,a0,a1"));
    CHECK_EQ(d(0x70850000), std::string("madd\ta0,a1"));
    CHECK_EQ(d(0x70641018), std::string("mult1\tv0,v1,a0"));
    // div sempre mostra "zero" como destino
    CHECK_EQ(d(0x0085001A), std::string("div\tzero,a0,a1"));
    // syscall / break com e sem código
    CHECK_EQ(d(0x0000000C), std::string("syscall"));
    CHECK_EQ(d(0x00000F0C), std::string("syscall\t0x3c"));
    CHECK_EQ(d(0x0000000D), std::string("break"));
    CHECK_EQ(d(0x0007000D), std::string("break\t0x7"));
    CHECK_EQ(d(0x0022190D), std::string("break\t0x22,0x64"));
    // traps com e sem código
    CHECK_EQ(d(0x00850034), std::string("teq\ta0,a1"));
    CHECK_EQ(d(0x00851934), std::string("teq\ta0,a1,0x64"));
    // sync / sync.p
    CHECK_EQ(d(0x0000000F), std::string("sync"));
    CHECK_EQ(d(0x0000040F), std::string("sync.p"));
    // cache/pref
    CHECK_EQ(d(0xBC980040), std::string("cache\t0x18,64(a0)"));
}

TEST_CASE(disasm, branch_and_jump_targets) {
    CHECK_EQ(d(0x10850010, 0x00100000), std::string("beq\ta0,a1,0x100044"));
    CHECK_EQ(d(0x1000FFFF, 0x00100020), std::string("beq\tzero,zero,0x100020"));
    CHECK_EQ(d(0x0C048D14, 0x00100000), std::string("jal\t0x123450"));
    CHECK_EQ(d(0x45010004, 0x00100000), std::string("bc1t\t0x100014"));

    DisasmOptions opt;
    opt.symbolize = [](std::uint32_t addr) -> std::string {
        return addr == 0x00123450 ? "main" : "";
    };
    CHECK_EQ(disassemble(decode(0x0C048D14, 0x00100000), opt).str(),
             std::string("jal\t0x123450 <main>"));
    CHECK_EQ(disassemble(decode(0x10850010, 0x00100000), opt).str(),
             std::string("beq\ta0,a1,0x100044"));
}

TEST_CASE(disasm, coprocessor_registers) {
    CHECK_EQ(d(0x40026000), std::string("mfc0\tv0,c0_sr"));
    CHECK_EQ(d(0x40827000), std::string("mtc0\tv0,c0_epc"));
    CHECK_EQ(d(0x40023800), std::string("mfc0\tv0,$7"));
    CHECK_EQ(d(0x4002C000), std::string("mfbpc\tv0"));
    CHECK_EQ(d(0x4002C800), std::string("mfps\tv0,0"));
    CHECK_EQ(d(0x44420000), std::string("cfc1\tv0,c1_fir"));
    CHECK_EQ(d(0x44C2F800), std::string("ctc1\tv0,c1_fcsr"));
    CHECK_EQ(d(0x44021800), std::string("mfc1\tv0,$f3"));
    CHECK_EQ(d(0xC4A20008), std::string("lwc1\t$f2,8(a1)"));
    CHECK_EQ(d(0x46020834), std::string("c.lt.s\t$f1,$f2"));
    CHECK_EQ(d(0x46001064), std::string("cvt.w.s\t$f1,$f2"));
    CHECK_EQ(d(0x46030044), std::string("sqrt.s\t$f1,$f3"));  // R5900: operando em ft
    CHECK_EQ(d(0x48241800), std::string("qmfc2\ta0,$vf3"));
    CHECK_EQ(d(0x48241801), std::string("qmfc2.i\ta0,$vf3"));
    CHECK_EQ(d(0x48448001), std::string("cfc2.i\ta0,$vi16"));
    CHECK_EQ(d(0xD8A30010), std::string("lqc2\t$vf3,16(a1)"));
}

TEST_CASE(disasm, vu0_macro_syntax) {
    CHECK_EQ(d(0x4BC31068), std::string("vadd.xyz\t$vf1xyz,$vf2xyz,$vf3xyz"));
    CHECK_EQ(d(0x4A231040), std::string("vaddx.w\t$vf1w,$vf2w,$vf3x"));
    CHECK_EQ(d(0x4A000838), std::string("vcallms\t0x100"));
    CHECK_EQ(d(0x4A820BBC), std::string("vdiv\t$Q,$vf1x,$vf2y"));
    CHECK_EQ(d(0x4BC209FF), std::string("vclipw.xyz\t$vf1xyz,$vf2w"));
    CHECK_EQ(d(0x4A011772), std::string("viaddi\t$vi1,$vi2,-3"));
    CHECK_EQ(d(0x4BE1137C), std::string("vlqi.xyzw\t$vf1xyzw,($vi2++)"));
    CHECK_EQ(d(0x4A0002FF), std::string("vnop"));
    CHECK_EQ(d(0x4A0003BF), std::string("vwaitq"));
    // VADDA/VMSUBA: mantemos ACC,fs,ft (o binutils inverte só nessas duas)
    CHECK_EQ(d(0x4BE21ABC), std::string("vadda.xyzw\t$ACCxyzw,$vf3xyzw,$vf2xyzw"));
    CHECK_EQ(d(0x4BE21AFC), std::string("vsuba.xyzw\t$ACCxyzw,$vf3xyzw,$vf2xyzw"));

    CHECK_EQ(vuDestString(0xF), std::string("xyzw"));
    CHECK_EQ(vuDestString(0x9), std::string("xw"));
    CHECK_EQ(vuDestString(0x0), std::string(""));
}

TEST_CASE(disasm, invalid_word) {
    CHECK_EQ(d(0xC0000000), std::string(".word\t0xc0000000"));
    const auto t = disassemble(decode(0x0000001C));
    CHECK_EQ(t.mnemonic, std::string(".word"));
    CHECK_EQ(t.operands, std::string("0x1c"));
    CHECK_EQ(t.str(' '), std::string(".word 0x1c"));
}

TEST_CASE(disasm, register_names) {
    CHECK_EQ(gprName(0), std::string_view("zero"));
    CHECK_EQ(gprName(29), std::string_view("sp"));
    CHECK_EQ(gprName(30), std::string_view("s8"));
    CHECK_EQ(gprName(31), std::string_view("ra"));
    CHECK_EQ(cop0Name(12), std::string_view("c0_sr"));
    CHECK_EQ(cop0Name(17), std::string_view(""));
}
