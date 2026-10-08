// Testes por instrução da semântica do runtime (ops.h), executando as mesmas
// funções que o código gerado chama.

#include <cstring>

#include "anyps2/runtime/context.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/memory.h"
#include "anyps2/runtime/ops.h"
#include "anyps2/runtime/runtime.h"
#include "minitest.h"

using namespace anyps2::rt;
using namespace anyps2::rt::ops;

namespace {

struct Cpu {
    Memory mem;
    Context c{};
    Cpu() {
        std::memset(static_cast<void*>(&c), 0, sizeof(Context));
        c.mem = &mem;
        c.fcr31 = 0x01000001u;
    }
    void set(unsigned r, std::uint64_t lo, std::uint64_t hi = 0) {
        c.r[r].ud[0] = lo;
        c.r[r].ud[1] = hi;
    }
    std::uint64_t g(unsigned r) const { return c.r[r].ud[0]; }
    std::uint64_t gh(unsigned r) const { return c.r[r].ud[1]; }
};

std::uint32_t fbits(float f) {
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

}  // namespace

TEST_CASE(runtime_ops, arithmetic_and_sign_extension) {
    Cpu x;
    x.set(1, 0x7FFFFFFF, 0xAAAAAAAAAAAAAAAAull);
    ADDIU(&x.c, 2, 1, 1);  // 0x80000000 estendido com sinal
    CHECK_EQ(x.g(2), 0xFFFFFFFF80000000ull);
    CHECK_EQ(x.gh(2), 0ull);  // altos de rd preservados (eram 0)
    x.set(3, 5, 0x1234);
    ADDIU(&x.c, 3, 3, -10);
    CHECK_EQ(x.g(3), 0xFFFFFFFFFFFFFFFBull);
    CHECK_EQ(x.gh(3), 0x1234ull);  // instruções não-MMI preservam os 64 bits altos
    ADDIU(&x.c, 0, 1, 5);
    CHECK_EQ(x.g(0), 0ull);  // $zero nunca muda
    LUI(&x.c, 4, 0x8001);
    CHECK_EQ(x.g(4), 0xFFFFFFFF80010000ull);
    ORI(&x.c, 4, 4, 0xFFFF);
    CHECK_EQ(x.g(4), 0xFFFFFFFF8001FFFFull);
    x.set(5, 0xFFFFFFFFFFFFFFFFull);
    SLTIU(&x.c, 6, 5, -1);  // compara sem sinal com o imediato estendido
    CHECK_EQ(x.g(6), 0ull);
    SLTI(&x.c, 6, 5, 0);
    CHECK_EQ(x.g(6), 1ull);
    DADDIU(&x.c, 7, 5, 2);
    CHECK_EQ(x.g(7), 1ull);
}

TEST_CASE(runtime_ops, overflow_exceptions) {
    Cpu x;
    x.set(1, 0x7FFFFFFF);
    CHECK_THROWS_WITH(ADDI(&x.c, 2, 1, 1, 0x100), "overflow");
    CHECK_THROWS_WITH(ADD(&x.c, 2, 1, 1, 0x100), "PC 0x00000100");
    x.set(3, 0x8000000000000000ull);
    x.set(4, 1);
    CHECK_THROWS_WITH(DSUB(&x.c, 5, 3, 4, 0x104), "overflow");
    ADDU(&x.c, 2, 1, 1);  // sem exceção
    CHECK_EQ(x.g(2), 0xFFFFFFFFFFFFFFFEull);
    CHECK_THROWS_WITH(TEQ(&x.c, 0, 0, 0x108), "trap");
    TNE(&x.c, 0, 0, 0x108);
}

TEST_CASE(runtime_ops, shifts) {
    Cpu x;
    x.set(1, 0x80000001u);
    SRA(&x.c, 2, 1, 4);
    CHECK_EQ(x.g(2), 0xFFFFFFFFF8000000ull);
    SRL(&x.c, 2, 1, 4);
    CHECK_EQ(x.g(2), 0x08000000ull);
    SLL(&x.c, 2, 1, 31);
    CHECK_EQ(x.g(2), 0xFFFFFFFF80000000ull);
    x.set(3, 35);  // SLLV usa só 5 bits
    SLLV(&x.c, 2, 1, 3);
    CHECK_EQ(x.g(2), 0x00000008ull);
    x.set(4, 0x8000000000000000ull);
    DSRA32(&x.c, 5, 4, 31);
    CHECK_EQ(x.g(5), 0xFFFFFFFFFFFFFFFFull);
    DSRL32(&x.c, 5, 4, 31);
    CHECK_EQ(x.g(5), 1ull);
    x.set(6, 65);
    DSLLV(&x.c, 5, 6, 6);  // 65 & 63 = 1
    CHECK_EQ(x.g(5), 130ull);
}

TEST_CASE(runtime_ops, multiply_divide) {
    Cpu x;
    x.set(1, static_cast<std::uint64_t>(-7));
    x.set(2, 3);
    MULT(&x.c, 3, 1, 2);
    CHECK_EQ(x.g(3), static_cast<std::uint64_t>(-21));
    CHECK_EQ(x.c.hi.ud[0], 0xFFFFFFFFFFFFFFFFull);
    MULTU(&x.c, 3, 1, 2);  // 0xFFFFFFF9 * 3
    CHECK_EQ(x.c.lo.ud[0], 0xFFFFFFFFFFFFFFEBull);
    CHECK_EQ(x.c.hi.ud[0], 2ull);
    DIV(&x.c, 1, 2);
    CHECK_EQ(x.c.lo.sd[0], -2);
    CHECK_EQ(x.c.hi.sd[0], -1);
    // Divisão por zero: LO = -1 se rs >= 0 (1 se negativo), HI = rs
    x.set(4, 0);
    DIV(&x.c, 1, 4);
    CHECK_EQ(x.c.lo.sd[0], 1);
    CHECK_EQ(x.c.hi.sd[0], -7);
    DIVU(&x.c, 2, 4);
    CHECK_EQ(x.c.lo.sd[0], -1);
    CHECK_EQ(x.c.hi.sd[0], 3);
    // INT_MIN / -1
    x.set(5, 0x80000000u);
    x.set(6, 0xFFFFFFFFu);
    DIV(&x.c, 5, 6);
    CHECK_EQ(x.c.lo.sd[0], static_cast<std::int64_t>(INT32_MIN));
    CHECK_EQ(x.c.hi.sd[0], 0);
    // Pipeline 1 não afeta HI/LO
    x.c.lo.ud[0] = 0x1111;
    MULT1(&x.c, 7, 1, 2);
    CHECK_EQ(x.c.lo.ud[0], 0x1111ull);
    CHECK_EQ(x.c.lo.sd[1], -21);
    CHECK_EQ(x.g(7), static_cast<std::uint64_t>(-21));
    // MADD acumula em HI:LO
    x.c.hi.ud[0] = 0;
    x.c.lo.ud[0] = 100;
    MADD(&x.c, 8, 1, 2);
    CHECK_EQ(x.g(8), 79ull);
}

TEST_CASE(runtime_ops, loads_stores_and_partial) {
    Cpu x;
    const std::uint32_t base = 0x00100000;
    for (unsigned i = 0; i < 64; ++i) x.mem.ram()[base + i] = static_cast<std::uint8_t>(0x10 + i);
    x.set(1, base);
    LB(&x.c, 2, 1, 0x30, 0);
    CHECK_EQ(x.g(2), 0x40ull);
    x.mem.ram()[base + 0x31] = 0x80;
    LB(&x.c, 2, 1, 0x31, 0);
    CHECK_EQ(x.g(2), 0xFFFFFFFFFFFFFF80ull);
    LBU(&x.c, 2, 1, 0x31, 0);
    CHECK_EQ(x.g(2), 0x80ull);
    LW(&x.c, 3, 1, 4, 0);
    CHECK_EQ(x.g(3), 0x17161514ull);
    // LWL/LWR em todos os deslocamentos = leitura desalinhada
    for (std::int32_t off = 0; off < 4; ++off) {
        x.set(4, 0);
        LWL(&x.c, 4, 1, off + 3, 0);
        LWR(&x.c, 4, 1, off, 0);
        std::uint32_t expected;
        std::memcpy(&expected, x.mem.ram() + base + off, 4);
        CHECK_EQ(static_cast<std::uint32_t>(x.g(4)), expected);
    }
    for (std::int32_t off = 0; off < 8; ++off) {
        LDL(&x.c, 5, 1, off + 7, 0);
        LDR(&x.c, 5, 1, off, 0);
        std::uint64_t expected;
        std::memcpy(&expected, x.mem.ram() + base + off, 8);
        CHECK_EQ(x.g(5), expected);
    }
    x.set(6, 0xA1B2C3D4u);
    SWL(&x.c, 6, 1, 0x23, 0);
    SWR(&x.c, 6, 1, 0x20, 0);
    std::uint32_t w;
    std::memcpy(&w, x.mem.ram() + base + 0x20, 4);
    CHECK_EQ(w, 0xA1B2C3D4u);
    // LQ ignora os 4 bits baixos do endereço
    LQ(&x.c, 7, 1, 0x1F, 0);
    CHECK_EQ(x.g(7), 0x2726252423222120ull);  // bytes base+0x10..0x17
    CHECK_EQ(x.gh(7), 0x2F2E2D2C2B2A2928ull);
    CHECK_THROWS_WITH(LW(&x.c, 3, 1, 2, 0x200), "desalinhada");
    x.set(8, 0x02000000);  // logo após os 32 MB
    CHECK_THROWS_WITH(LW(&x.c, 3, 8, 0, 0x204), "não mapeado 0x02000000");
}

TEST_CASE(runtime_ops, memory_map_mirrors) {
    Memory m;
    m.write<std::uint32_t>(0x00012340, 0xCAFEBABE, 0);
    CHECK_EQ(m.read<std::uint32_t>(0x80012340, 0), 0xCAFEBABEu);  // kseg0
    CHECK_EQ(m.read<std::uint32_t>(0xA0012340, 0), 0xCAFEBABEu);  // kseg1
    CHECK_EQ(m.read<std::uint32_t>(0x20012340, 0), 0xCAFEBABEu);  // uncached
    CHECK_EQ(m.read<std::uint32_t>(0x30012340, 0), 0xCAFEBABEu);  // uncached acelerado
    m.write<std::uint16_t>(0x70003FFE, 0xBEEF, 0);                // scratchpad
    CHECK_EQ(m.scratchpad()[0x3FFE], 0xEF);
    CHECK_THROWS_WITH(m.read<std::uint8_t>(0x70004000, 0x300), "não mapeado");
    CHECK_THROWS_WITH(m.write<std::uint32_t>(0x10000000, 1, 0x304), "não mapeado");  // sem dispositivo
    CHECK_EQ(Memory::physical(0xA0001000), 0x00001000u);
    m.copyToGuest(0x00012400, "abc", 4, 0);
    CHECK_EQ(m.readCString(0x00012400, 16, 0), std::string("abc"));
    CHECK_THROWS_WITH(m.readCString(0x00012400, 2, 0x308), "sem terminador");
}

TEST_CASE(runtime_ops, mmi_lanes) {
    Cpu x;
    x.c.r[1].sh[0] = 32000;
    x.c.r[2].sh[0] = 1000;
    x.c.r[1].sh[1] = -32000;
    x.c.r[2].sh[1] = -1000;
    PADDSH(&x.c, 3, 1, 2);
    CHECK_EQ(x.c.r[3].sh[0], 32767);
    CHECK_EQ(x.c.r[3].sh[1], -32768);
    PADDH(&x.c, 3, 1, 2);
    CHECK_EQ(x.c.r[3].uh[0], static_cast<std::uint16_t>(33000));
    x.c.r[1].ub[5] = 250;
    x.c.r[2].ub[5] = 10;
    PADDUB(&x.c, 3, 1, 2);
    CHECK_EQ(x.c.r[3].ub[5], 255);
    PSUBUB(&x.c, 3, 2, 1);
    CHECK_EQ(x.c.r[3].ub[5], 0);
    // PEXTLW / PPACW são inversas
    x.set(4, 0x1111111122222222ull, 0x3333333344444444ull);
    x.set(5, 0x5555555566666666ull, 0x7777777788888888ull);
    PEXTLW(&x.c, 6, 4, 5);
    CHECK_EQ(x.c.r[6].uw[0], 0x66666666u);
    CHECK_EQ(x.c.r[6].uw[1], 0x22222222u);
    CHECK_EQ(x.c.r[6].uw[2], 0x55555555u);
    CHECK_EQ(x.c.r[6].uw[3], 0x11111111u);
    PCPYUD(&x.c, 7, 4, 5);
    CHECK_EQ(x.c.r[7].ud[0], 0x3333333344444444ull);
    CHECK_EQ(x.c.r[7].ud[1], 0x7777777788888888ull);
    // QFSRV com SA = 4 bytes
    MTSAB(&x.c, 0, 4);
    QFSRV(&x.c, 8, 4, 5);
    CHECK_EQ(x.c.r[8].uw[0], 0x55555555u);
    CHECK_EQ(x.c.r[8].uw[3], 0x22222222u);
    // PABSW satura INT_MIN
    x.c.r[9].uw[0] = 0x80000000u;
    x.c.r[9].sw[1] = -5;
    PABSW(&x.c, 10, 9);
    CHECK_EQ(x.c.r[10].uw[0], 0x7FFFFFFFu);
    CHECK_EQ(x.c.r[10].sw[1], 5);
    // PLZCW
    x.set(11, 0x00000000FFFFFFFFull);
    PLZCW(&x.c, 12, 11);
    CHECK_EQ(x.c.r[12].uw[0], 31u);  // 0xFFFFFFFF: 32 uns -> 31
    CHECK_EQ(x.c.r[12].uw[1], 31u);  // 0: 32 zeros -> 31
    // Escrita em $zero é descartada mesmo em MMI
    PADDW(&x.c, 0, 4, 5);
    CHECK_EQ(x.c.r[0].ud[0], 0ull);
    CHECK_EQ(x.c.r[0].ud[1], 0ull);
}

TEST_CASE(runtime_ops, mmi_hilo) {
    Cpu x;
    x.c.hi.uw[0] = 0x00000001;
    x.c.lo.uw[0] = 0x00000000;  // HI:LO = 2^32 -> satura em INT32_MAX
    x.c.hi.uw[2] = 0xFFFFFFFF;
    x.c.lo.uw[2] = 0xFFFFFFFE;  // -2
    PMFHL_SLW(&x.c, 1);
    CHECK_EQ(x.c.r[1].sd[0], static_cast<std::int64_t>(INT32_MAX));
    CHECK_EQ(x.c.r[1].sd[1], -2);
    x.set(2, 0x0000000400000003ull, 0x0000000200000001ull);
    PMTHL_LW(&x.c, 2);
    CHECK_EQ(x.c.lo.uw[0], 3u);
    CHECK_EQ(x.c.hi.uw[0], 4u);
    CHECK_EQ(x.c.lo.uw[2], 1u);
    CHECK_EQ(x.c.hi.uw[2], 2u);
    x.c.r[3].sw[0] = 100;
    x.c.r[3].sw[2] = -9;
    x.c.r[4].sw[0] = 7;
    x.c.r[4].sw[2] = 0;
    PDIVW(&x.c, 3, 4);
    CHECK_EQ(x.c.lo.sd[0], 14);
    CHECK_EQ(x.c.hi.sd[0], 2);
    CHECK_EQ(x.c.lo.sd[1], 1);  // divisão por zero com dividendo negativo
    CHECK_EQ(x.c.hi.sd[1], -9);
}

TEST_CASE(runtime_ops, fpu_ps2_semantics) {
    Cpu x;
    x.c.f[1] = fbits(1.0f);
    x.c.f[2] = 0;
    DIV_S(&x.c, 3, 1, 2);
    CHECK_EQ(x.c.f[3], 0x7F7FFFFFu);           // +Fmax, não infinito
    CHECK((x.c.fcr31 & fcr::D) != 0);
    x.c.f[4] = 0x7F7FFFFFu;
    ADD_S(&x.c, 5, 4, 4);                      // overflow satura
    CHECK_EQ(x.c.f[5], 0x7F7FFFFFu);
    CHECK((x.c.fcr31 & fcr::O) != 0);
    x.c.f[6] = 0x7F800000u;                    // "Inf" do IEEE é um número no PS2
    MUL_S(&x.c, 7, 6, 1);
    CHECK_EQ(x.c.f[7], 0x7F7FFFFFu);
    x.c.f[8] = 0x00000001u;                    // denormal = zero
    ADD_S(&x.c, 9, 8, 8);
    CHECK_EQ(x.c.f[9], 0u);
    x.c.f[10] = fbits(-9.0f);
    SQRT_S(&x.c, 11, 10);                      // raiz de |x|, com flag I
    CHECK_EQ(x.c.f[11], fbits(3.0f));
    CHECK((x.c.fcr31 & fcr::I) != 0);
    x.c.f[12] = fbits(-2.5e10f);
    CVT_W_S(&x.c, 13, 12);
    CHECK_EQ(x.c.f[13], 0x80000000u);
    x.c.f[12] = fbits(-7.9f);
    CVT_W_S(&x.c, 13, 12);
    CHECK_EQ(x.c.f[13], static_cast<std::uint32_t>(-7));
    x.c.f[14] = fbits(-1.0f);
    x.c.f[15] = fbits(-2.0f);
    MAX_S(&x.c, 16, 14, 15);
    CHECK_EQ(x.c.f[16], fbits(-1.0f));
    MIN_S(&x.c, 16, 14, 15);
    CHECK_EQ(x.c.f[16], fbits(-2.0f));
    C_LT_S(&x.c, 15, 14);
    CHECK(fpuCondition(&x.c));
    C_F_S(&x.c, 0, 0);
    CHECK(!fpuCondition(&x.c));
    x.c.f[17] = fbits(2.0f);
    x.c.f[18] = fbits(5.0f);
    MULA_S(&x.c, 17, 18);
    MADD_S(&x.c, 19, 17, 17);
    CHECK_EQ(x.c.f[19], fbits(14.0f));
    // CFC1/CTC1
    CFC1(&x.c, 20, 0);
    CHECK_EQ(x.g(20), 0x2E00ull);
    x.set(21, 0xFFFFFFFFu);
    CTC1(&x.c, 21, 31);
    CHECK_EQ(x.c.fcr31, 0x0083C078u | 0x01000001u);
}

TEST_CASE(runtime_ops, hardware_registers) {
    // Runtime sem funções: só para ter o hardware mapeado.
    const ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    Runtime rt(info, RuntimeOptions{});
    Memory& m = rt.memory();
    // Registradores conhecidos guardam o valor.
    m.write<std::uint32_t>(0x12001010, 0x7F00, 0);  // GS_IMR
    CHECK_EQ(m.read<std::uint32_t>(0x12001010, 0), 0x7F00u);
    m.write<std::uint32_t>(0x10000010, 0x80, 0);    // T0_MODE
    CHECK_EQ(m.read<std::uint32_t>(0x10000010, 0), 0x80u);
    // INTC_MASK inverte bits ao escrever 1; INTC_STAT limpa
    m.write<std::uint32_t>(0x1000F010, 0x5, 0);
    m.write<std::uint32_t>(0x1000F010, 0x1, 0);
    CHECK_EQ(m.read<std::uint32_t>(0x1000F010, 0), 0x4u);
    // Desconhecido: erro com endereço
    CHECK_THROWS_WITH(m.read<std::uint32_t>(0x1000A700, 0x500), "desconhecido 0x1000A700");
    CHECK_THROWS_WITH(m.write<std::uint32_t>(0x1000F7F0, 1, 0x504), "0x1000F7F0");
    // DMA em canal ainda não suportado (IPU): erro dizendo o canal
    CHECK_THROWS_WITH(m.write<std::uint32_t>(0x1000B000, 0x100, 0x508), "fromIPU");
    // Espelho uncached dos registradores (0xB0000000 = kseg1)
    CHECK_EQ(m.read<std::uint32_t>(0xB2001010, 0), 0x7F00u);
}
