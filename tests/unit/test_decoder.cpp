// Testes por instrução do decodificador R5900.

#include <cstdio>
#include <set>
#include <string>

#include "anyps2/r5900/decoder.h"
#include "anyps2/r5900/disassembler.h"
#include "minitest.h"

using namespace anyps2::r5900;

namespace {

struct RefInsn {
    Op op;
    std::uint32_t word;
    std::uint32_t address;
    const char* text;
};

// Uma codificação de referência para cada instrução (ver
// tests/scripts/gen_instruction_table.py).
const RefInsn kReference[] = {
#include "instruction_table.inc"
};

// Codificadores mínimos (campo a campo) para os casos escritos à mão.
constexpr std::uint32_t R(std::uint32_t op, std::uint32_t rs, std::uint32_t rt, std::uint32_t rd,
                          std::uint32_t sa, std::uint32_t funct) {
    return (op << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct;
}
constexpr std::uint32_t I(std::uint32_t op, std::uint32_t rs, std::uint32_t rt, std::uint32_t imm) {
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF);
}

Instruction dec(std::uint32_t word, std::uint32_t addr = 0x00100000) {
    return decode(word, addr);
}

}  // namespace

// ---------------------------------------------------------------------------
// Tabela de referência: uma entrada por instrução
// ---------------------------------------------------------------------------

TEST_CASE(decoder, every_instruction_reference) {
    for (const auto& ref : kReference) {
        const Instruction insn = decode(ref.word, ref.address);
        const std::string name(opInfo(ref.op).name);
        CHECK_MSG(insn.op == ref.op, name + ": decodificou como " + std::string(insn.info().name));
        CHECK_MSG(insn.canonical, name + ": deveria ser canônica");
        CHECK_MSG(disassemble(insn).str() == ref.text,
                  name + ": texto \"" + disassemble(insn).str() + "\" != \"" + ref.text + "\"");
    }
}

TEST_CASE(decoder_coverage, all_ops_have_reference) {
    std::set<Op> covered;
    for (const auto& ref : kReference) covered.insert(ref.op);
    for (const auto& info : opTable()) {
        if (info.op == Op::Invalid) continue;
        CHECK_MSG(covered.count(info.op) == 1,
                  std::string(info.name) + " não tem codificação de referência");
    }
    CHECK_EQ(covered.size(), kOpCount - 1);
}

TEST_CASE(decoder_coverage, op_table_is_consistent) {
    std::set<std::string_view> names;
    for (std::size_t i = 0; i < kOpCount; ++i) {
        const OpInfo& info = opTable()[i];
        CHECK_EQ(static_cast<std::size_t>(info.op), i);
        CHECK(!info.mnemonic.empty());
        CHECK_MSG(names.insert(info.name).second, "nome duplicado " + std::string(info.name));

        const bool control = (info.flags & (Flag::Branch | Flag::Jump | Flag::JumpReg)) != 0;
        const bool delay = (info.flags & Flag::DelaySlot) != 0;
        CHECK_MSG(control == delay,
                  std::string(info.name) + ": desvio/salto se e somente se tem delay slot");
        if (info.flags & Flag::Likely) {
            CHECK_MSG((info.flags & Flag::Branch) != 0, std::string(info.name) + ": likely sem branch");
        }
        const bool mem = (info.flags & (Flag::Load | Flag::Store)) != 0;
        CHECK_MSG(mem == (info.memSize != 0),
                  std::string(info.name) + ": memSize deve existir exatamente para loads/stores");
        if (info.flags & Flag::VuMemory) CHECK(info.memSize == 0);
    }
}

// ---------------------------------------------------------------------------
// Semântica de controle de fluxo
// ---------------------------------------------------------------------------

TEST_CASE(decoder, branch_targets) {
    // beq a0,a1,+0x10 palavras em 0x100000 -> 0x100004 + 0x40
    auto b = dec(I(4, 4, 5, 0x0010), 0x00100000);
    CHECK_EQ(b.op, Op::BEQ);
    CHECK(b.isBranch());
    CHECK(b.hasDelaySlot());
    CHECK(!b.isLikely());
    CHECK_EQ(b.branchTarget(), 0x00100044u);
    CHECK_EQ(b.staticTarget(), 0x00100044u);

    // Deslocamento negativo: bne com -1 -> o próprio endereço (laço de 1 instrução)
    auto loop = dec(I(5, 4, 0, 0xFFFF), 0x00100020);
    CHECK_EQ(loop.branchTarget(), 0x00100020u);

    // Likely e link
    auto bl = dec(I(20, 4, 5, 2));
    CHECK(bl.isLikely());
    auto bal = dec(I(1, 4, 17, 0xFFFE));  // bgezal
    CHECK_EQ(bal.op, Op::BGEZAL);
    CHECK(bal.isLink());
    CHECK(bal.isBranch());
    CHECK_EQ(bal.branchTarget(), 0x00100004u - 8u);
    auto ball = dec(I(1, 4, 19, 1));  // bgezall
    CHECK(ball.isLink() && ball.isLikely());

    // COPz
    CHECK(dec(I(17, 8, 1, 4)).isBranch());               // bc1t
    CHECK(dec(I(17, 8, 3, 4)).isLikely());               // bc1tl
    CHECK_EQ(dec(I(18, 8, 0, 4)).op, Op::BC2F);
    CHECK_EQ(dec(I(16, 8, 2, 4)).op, Op::BC0FL);
}

TEST_CASE(decoder, jump_targets) {
    auto j = dec((2u << 26) | (0x00123450u >> 2), 0x00100000);
    CHECK_EQ(j.op, Op::J);
    CHECK(j.isJump());
    CHECK(!j.isLink());
    CHECK_EQ(j.jumpTarget(), 0x00123450u);

    auto jal = dec((3u << 26) | 0x40, 0x00100000);
    CHECK(jal.isLink());
    CHECK_EQ(jal.jumpTarget(), 0x00000100u);

    // Os 4 bits altos vêm do PC do delay slot, não da própria instrução.
    auto edge = dec((2u << 26) | 0x10, 0x0FFFFFFC);
    CHECK_EQ(edge.jumpTarget(), 0x10000040u);
    auto kseg = dec((2u << 26) | 0x10, 0x80001000);
    CHECK_EQ(kseg.jumpTarget(), 0x80000040u);

    auto jr = dec(R(0, 31, 0, 0, 0, 8));
    CHECK(jr.isJumpRegister());
    CHECK(jr.isReturn());
    CHECK(jr.hasDelaySlot());
    CHECK_EQ(jr.staticTarget(), 0u);
    CHECK(!dec(R(0, 25, 0, 0, 0, 8)).isReturn());  // jr t9 não é retorno

    auto jalr = dec(R(0, 25, 0, 31, 0, 9));
    CHECK(jalr.isJumpRegister() && jalr.isLink());
}

TEST_CASE(decoder, unconditional_branches) {
    CHECK(dec(I(4, 0, 0, 3)).isUnconditionalBranch());   // beq zero,zero (b)
    CHECK(dec(I(4, 7, 7, 3)).isUnconditionalBranch());   // beq t?,t? também
    CHECK(!dec(I(4, 7, 8, 3)).isUnconditionalBranch());
    CHECK(dec(I(1, 0, 1, 3)).isUnconditionalBranch());   // bgez zero
    CHECK(dec(I(1, 0, 17, 3)).isUnconditionalBranch());  // bgezal zero (bal)
    CHECK(dec(I(6, 0, 0, 3)).isUnconditionalBranch());   // blez zero
    CHECK(!dec(I(5, 0, 0, 3)).isUnconditionalBranch());  // bne zero,zero nunca desvia
    CHECK(!dec(I(1, 0, 0, 3)).isUnconditionalBranch());  // bltz zero nunca desvia
}

TEST_CASE(decoder, exceptions_and_traps) {
    auto sc = dec(R(0, 0, 0, 0, 0, 12) | (0x3C << 6));
    CHECK_EQ(sc.op, Op::SYSCALL);
    CHECK(sc.has(Flag::Exception));
    CHECK_EQ(sc.syscallCode(), 0x3Cu);
    auto brk = dec(R(0, 0, 0, 0, 0, 13));
    CHECK(brk.has(Flag::Exception));
    auto teq = dec(R(0, 4, 5, 0, 7, 52));
    CHECK(teq.has(Flag::Trap));
    CHECK_EQ(teq.trapCode(), (0u << 4) | (0u) | 7u);
    CHECK(dec(I(1, 4, 12, 5)).has(Flag::Trap));  // teqi
    auto eret = dec(R(16, 16, 0, 0, 0, 24));
    CHECK_EQ(eret.op, Op::ERET);
    CHECK(eret.has(Flag::ExceptionReturn));
    CHECK(eret.has(Flag::Privileged));
    CHECK(dec(I(8, 4, 5, 1)).has(Flag::Overflow));    // addi
    CHECK(!dec(I(9, 4, 5, 1)).has(Flag::Overflow));   // addiu
}

// ---------------------------------------------------------------------------
// Acessos à memória
// ---------------------------------------------------------------------------

TEST_CASE(decoder, memory_access_sizes) {
    struct Case {
        std::uint32_t op;
        Op expected;
        std::uint8_t size;
        bool load;
        bool unaligned;
    };
    const Case cases[] = {
        {32, Op::LB, 1, true, false},   {36, Op::LBU, 1, true, false},
        {33, Op::LH, 2, true, false},   {37, Op::LHU, 2, true, false},
        {35, Op::LW, 4, true, false},   {39, Op::LWU, 4, true, false},
        {34, Op::LWL, 4, true, true},   {38, Op::LWR, 4, true, true},
        {55, Op::LD, 8, true, false},   {26, Op::LDL, 8, true, true},
        {27, Op::LDR, 8, true, true},   {30, Op::LQ, 16, true, false},
        {40, Op::SB, 1, false, false},  {41, Op::SH, 2, false, false},
        {43, Op::SW, 4, false, false},  {42, Op::SWL, 4, false, true},
        {46, Op::SWR, 4, false, true},  {63, Op::SD, 8, false, false},
        {44, Op::SDL, 8, false, true},  {45, Op::SDR, 8, false, true},
        {31, Op::SQ, 16, false, false}, {49, Op::LWC1, 4, true, false},
        {57, Op::SWC1, 4, false, false}, {54, Op::LQC2, 16, true, false},
        {62, Op::SQC2, 16, false, false},
    };
    for (const auto& c : cases) {
        const auto insn = dec(I(c.op, 29, 4, 0xFFF0));
        const std::string name(opInfo(c.expected).name);
        CHECK_MSG(insn.op == c.expected, name);
        CHECK_MSG(insn.info().memSize == c.size, name + ": tamanho");
        CHECK_MSG(insn.has(Flag::Load) == c.load, name + ": load");
        CHECK_MSG(insn.has(Flag::Store) == !c.load, name + ": store");
        CHECK_MSG(insn.has(Flag::Unaligned) == c.unaligned, name + ": unaligned");
        CHECK_EQ(insn.simm16(), -16);
        CHECK_EQ(insn.rs(), 29u);
    }
    // Loads/stores do VU0 em modo macro acessam a memória do VU, não a do EE.
    const auto vlqi = dec(0x4BE1137C);
    CHECK_EQ(vlqi.op, Op::VLQI);
    CHECK(vlqi.has(Flag::VuMemory));
    CHECK(!vlqi.has(Flag::Load));
}

// ---------------------------------------------------------------------------
// Instruções que não existem no R5900
// ---------------------------------------------------------------------------

TEST_CASE(decoder, invalid_encodings) {
    const std::uint32_t invalid[] = {
        I(0x30, 4, 5, 0),             // LL
        I(0x38, 4, 5, 0),             // SC
        I(0x34, 4, 5, 0),             // LLD
        I(0x3C, 4, 5, 0),             // SCD
        I(0x35, 4, 2, 0),             // LDC1
        I(0x3D, 4, 2, 0),             // SDC1
        I(0x32, 4, 2, 0),             // LWC2
        I(0x3A, 4, 2, 0),             // SWC2
        I(0x13, 0, 0, 0),             // COP3
        I(0x1D, 0, 0, 0),             // JALX
        R(0, 4, 5, 0, 0, 0x1C),       // DMULT
        R(0, 4, 5, 0, 0, 0x1E),       // DDIV
        R(0, 4, 5, 6, 0, 0x01),       // SPECIAL reservado
        R(0, 4, 5, 6, 0, 0x05),
        R(0, 4, 5, 6, 0, 0x35),
        I(1, 4, 4, 0),                // REGIMM reservado
        I(1, 4, 0x1A, 0),
        R(28, 4, 5, 6, 0, 0x02),      // MMI reservado
        R(28, 4, 5, 6, 11, 0x08),     // MMI0 sa=11
        R(28, 4, 5, 6, 0, 0x28),      // MMI1 sa=0
        R(28, 4, 5, 6, 1, 0x09),      // MMI2 sa=1
        R(28, 4, 5, 6, 31, 0x29),     // MMI3 sa=31
        R(28, 0, 0, 6, 5, 0x30),      // PMFHL sa=5
        R(28, 4, 0, 0, 1, 0x31),      // PMTHL sa=1
        R(16, 2, 4, 5, 0, 0),         // CFC0
        R(16, 16, 0, 0, 0, 0x20),     // WAIT
        R(16, 0, 4, 24, 0, 1),        // MF0 debug, funct reservado
        I(16, 8, 4, 0),               // BC0 rt=4
        R(17, 17, 2, 3, 4, 0),        // COP1 formato D
        R(17, 21, 2, 3, 4, 32),       // COP1 formato L
        R(17, 16, 2, 3, 4, 0x0C),     // ROUND.W.S
        R(17, 16, 2, 3, 0, 0x31),     // C.UN.S
        R(17, 20, 0, 3, 4, 0x21),     // CVT.D.W
        R(18, 0, 4, 3, 0, 0),         // COP2 rs=0
        (18u << 26) | (1u << 25) | 0x33u,                         // Special1 0x33
        (18u << 26) | (1u << 25) | ((43u >> 2) << 6) | 0x3Cu | 3u, // Special2 43
        (18u << 26) | (1u << 25) | ((68u >> 2) << 6) | 0x3Cu,      // Special2 68
    };
    for (const auto w : invalid) {
        const auto insn = dec(w);
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08x", w);
        CHECK_MSG(!insn.valid(), std::string(buf) + " decodificou como " +
                                     std::string(insn.info().name));
        CHECK(!insn.canonical);
        CHECK_EQ(disassemble(insn).mnemonic, std::string(".word"));
    }
}

TEST_CASE(decoder, non_canonical_reserved_bits) {
    // sll com rs != 0: o hardware ignora rs, mas não é a codificação canônica.
    auto sll = dec(R(0, 3, 5, 6, 7, 0));
    CHECK_EQ(sll.op, Op::SLL);
    CHECK(!sll.canonical);
    // add com sa != 0
    auto add = dec(R(0, 4, 5, 6, 1, 32));
    CHECK_EQ(add.op, Op::ADD);
    CHECK(!add.canonical);
    // jr com rd != 0
    CHECK(!dec(R(0, 31, 0, 1, 0, 8)).canonical);
    // vclipw com dest != xyz
    auto clip = dec((18u << 26) | (1u << 25) | (0xFu << 21) | (2u << 16) | (3u << 11) |
                    ((31u >> 2) << 6) | 0x3Cu | 3u);
    CHECK_EQ(clip.op, Op::VCLIPw);
    CHECK(!clip.canonical);
    // vilwr com mais de um componente
    auto ilwr = dec((18u << 26) | (1u << 25) | (0x3u << 21) | (2u << 16) | (3u << 11) |
                    ((62u >> 2) << 6) | 0x3Cu | 2u);
    CHECK_EQ(ilwr.op, Op::VILWR);
    CHECK(!ilwr.canonical);
    // A palavra 0 é "sll zero,zero,0" (nop) canônica.
    auto nop = dec(0);
    CHECK_EQ(nop.op, Op::SLL);
    CHECK(nop.canonical);
}

// ---------------------------------------------------------------------------
// Campos auxiliares
// ---------------------------------------------------------------------------

TEST_CASE(decoder, field_helpers) {
    auto addiu = dec(I(9, 29, 29, 0x8000));
    CHECK_EQ(addiu.simm16(), -32768);
    CHECK_EQ(addiu.imm16(), 0x8000);
    auto ori = dec(I(13, 4, 5, 0xFFFF));
    CHECK_EQ(ori.imm16(), 0xFFFF);

    // VIADDI com imm5 negativo (-3 = 0b11101)
    auto viaddi = dec((18u << 26) | (1u << 25) | (1u << 16) | (2u << 11) | (0x1Du << 6) | 50u);
    CHECK_EQ(viaddi.op, Op::VIADDI);
    CHECK_EQ(viaddi.vuImm5(), -3);
    CHECK_EQ(disassemble(viaddi).str(), std::string("viaddi\t$vi1,$vi2,-3"));

    // VCALLMS: imm15 em unidades de 8 bytes
    auto callms = dec((18u << 26) | (1u << 25) | (0x7FFFu << 6) | 56u);
    CHECK_EQ(callms.vuImm15(), 0x7FFFu);

    // Campos de VU: dest/bc/fsf/ftf
    auto vdiv = dec((18u << 26) | (1u << 25) | (((2u << 2) | 1u) << 21) | (3u << 16) |
                    (2u << 11) | ((56u >> 2) << 6) | 0x3Cu);
    CHECK_EQ(vdiv.op, Op::VDIV);
    CHECK_EQ(vdiv.vuFtf(), 2u);
    CHECK_EQ(vdiv.vuFsf(), 1u);
    CHECK_EQ(disassemble(vdiv).str(), std::string("vdiv\t$Q,$vf2y,$vf3z"));

    auto vaddw = dec((18u << 26) | (1u << 25) | (0xAu << 21) | (3u << 16) | (2u << 11) |
                     (1u << 6) | 3u);
    CHECK_EQ(vaddw.op, Op::VADDbc);
    CHECK_EQ(vaddw.vuBc(), 3u);
    CHECK_EQ(vaddw.vuDest(), 0xAu);
    CHECK_EQ(disassemble(vaddw).str(), std::string("vaddw.xz\t$vf1xz,$vf2xz,$vf3w"));

    // MFPS/MFPC: seletor nos bits 5..1, tipo no bit 0
    CHECK_EQ(dec(R(16, 0, 5, 25, 0, 0)).op, Op::MFPS);
    CHECK_EQ(dec(R(16, 0, 5, 25, 0, 1)).op, Op::MFPC);
    CHECK_EQ(disassemble(dec(R(16, 0, 5, 25, 0, 3))).str(), std::string("mfpc\ta1,1"));
}

TEST_CASE(decoder, never_throws_and_is_deterministic) {
    // Varre 2 milhões de palavras pseudoaleatórias + todos os 65536 valores
    // dos 16 bits altos: decode() deve ser total e determinístico.
    std::uint32_t x = 0x12345678u;
    std::size_t valid = 0;
    for (int i = 0; i < 2000000; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        const auto a = decode(x, 0x00100000);
        const auto b = decode(x, 0x00100000);
        if (a.op != b.op || a.canonical != b.canonical) {
            CHECK_MSG(false, "decodificação não determinística");
            break;
        }
        if (a.valid()) ++valid;
        (void)disassemble(a);
    }
    for (std::uint32_t hi = 0; hi < 0x10000; ++hi) {
        const auto insn = decode((hi << 16) | 0x1234u, 0);
        (void)insn.info();
        (void)disassemble(insn);
    }
    CHECK(valid > 500000);  // boa parte do espaço é válido
}
