// Semântica dos VUs: aritmética do PS2, flags, modo macro (COP2), modo micro
// (pipeline, desvios, bit E), XGKICK e MSCAL pelo VIF1. Valores esperados
// calculados à mão.

#include <cfenv>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/ops.h"
#include "anyps2/runtime/ps2float.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/vif.h"
#include "anyps2/runtime/vu/vu.h"
#include "anyps2/vu/isa.h"
#include "minitest.h"

using namespace anyps2::rt;
namespace ps2f = anyps2::rt::ps2f;

namespace {

// ---- Codificação (formas canônicas conferidas com o dvp-objdump) -----------
constexpr std::uint32_t kLNop = 0x8000033Cu, kUNop = 0x000002FFu;
constexpr std::uint32_t X = 8, Y = 4, Z = 2, XYZW = 15;  // máscaras dest (W = 1 não é usada)

std::uint32_t up(std::uint32_t funct, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs, std::uint32_t fd) {
    return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
}
std::uint32_t upx(std::uint32_t idx, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs) {
    return (dest << 21) | (ft << 16) | (fs << 11) | ((idx >> 2) << 6) | 0x3C | (idx & 3);
}
std::uint32_t lo(std::uint32_t op, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs, std::int32_t imm11) {
    return (op << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (static_cast<std::uint32_t>(imm11) & 0x7FF);
}
std::uint32_t losp(std::uint32_t funct, std::uint32_t ft, std::uint32_t fs, std::uint32_t fd) {
    return (0x40u << 25) | (ft << 16) | (fs << 11) | (fd << 6) | funct;
}
std::uint32_t lox(std::uint32_t idx, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs) {
    return (0x40u << 25) | (dest << 21) | (ft << 16) | (fs << 11) | ((idx >> 2) << 6) | 0x3C | (idx & 3);
}
// fsf/ftf no campo dest (DIV/SQRT/RSQRT/MTIR...)
std::uint32_t comps(std::uint32_t fsf, std::uint32_t ftf) { return fsf | (ftf << 2); }

std::uint32_t fb(float f) { return ps2f::toBits(f); }
float bf(std::uint32_t b) { return ps2f::fromBits(b); }

void setVf(Reg128& r, float x, float y, float z, float w) {
    r.uw[0] = fb(x);
    r.uw[1] = fb(y);
    r.uw[2] = fb(z);
    r.uw[3] = fb(w);
}

struct Rig {
    ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    Runtime rt{info, RuntimeOptions{}};
    Context& c = rt.context();
    void macroU(std::uint32_t upper) { ops::vu0Macro(&c, kLNop, upper, 0x100); }
    void macroL(std::uint32_t lower) { ops::vu0Macro(&c, lower, kUNop, 0x100); }
    // Escreve um microprograma (pares lower, upper) no VU1 a partir de 0.
    void program1(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& pairs) {
        std::uint8_t* m = rt.vu().micro1.get();
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            std::memcpy(m + i * 8, &pairs[i].first, 4);
            std::memcpy(m + i * 8 + 4, &pairs[i].second, 4);
        }
    }
    const vucore::Regs& r1() { return rt.vu1().regs(); }
};

// Referência independente para o truncamento: a FPU do host em FE_TOWARDZERO.
template <typename Fn>
float rtzRef(Fn fn) {
    const int old = std::fegetround();
    std::fesetround(FE_TOWARDZERO);
    volatile float r = fn();
    std::fesetround(old);
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------
// Aritmética do PS2
// ---------------------------------------------------------------------------

TEST_CASE(vu, ps2float_truncation_matches_host_round_to_zero) {
    std::mt19937 rng(5900);
    std::uniform_int_distribution<std::uint32_t> any;
    auto randomFloat = [&] {
        for (;;) {
            // Expoentes próximos geram cancelamentos e casos de borda.
            std::uint32_t b = any(rng);
            const std::uint32_t e = 100 + (any(rng) % 56);
            b = (b & 0x807FFFFFu) | (e << 23);
            return bf(b);
        }
    };
    int bad = 0;
    for (int i = 0; i < 200000; ++i) {
        volatile float a = randomFloat(), b = randomFloat();
        const float va = a, vb = b;
        if (ps2f::add(va, vb) != rtzRef([&] { return a + b; })) ++bad;
        if (ps2f::sub(va, vb) != rtzRef([&] { return a - b; })) ++bad;
        if (ps2f::mul(va, vb) != rtzRef([&] { return a * b; })) ++bad;
        if (ps2f::div(va, vb) != rtzRef([&] { return a / b; })) ++bad;
        const float sq = std::fabs(va);
        if (ps2f::sqrt(sq) != rtzRef([&] { return std::sqrt(std::fabs(a)); })) ++bad;
    }
    CHECK_EQ(bad, 0);
    // Exemplo clássico: 1/3 trunca para 0x3EAAAAAA (o arredondamento daria ...AB).
    CHECK_EQ(fb(ps2f::div(1.0f, 3.0f)), 0x3EAAAAAAu);
    // Saturação e flush
    const ps2f::Out o = ps2f::out(ps2f::mul(bf(ps2f::kFmax), 2.0f));
    CHECK_EQ(o.bits, ps2f::kFmax);
    CHECK(o.overflow);
    CHECK_EQ(fb(ps2f::in(0x00000001u)), 0u);       // denormal → 0
    CHECK_EQ(fb(ps2f::in(0x7F800000u)), ps2f::kFmax);  // "Inf" → Fmax
}

// ---------------------------------------------------------------------------
// Modo macro (COP2)
// ---------------------------------------------------------------------------

TEST_CASE(vu, macro_fmac_and_flags) {
    Rig t;
    setVf(t.c.vf[1], 1.5f, 2.0f, -3.0f, 4.0f);
    setVf(t.c.vf[2], 0.5f, -2.0f, 1.0f, 0.0f);
    t.macroU(up(0x28, XYZW, 2, 1, 3));  // vadd.xyzw vf3, vf1, vf2
    CHECK_EQ(bf(t.c.vf[3].uw[0]), 2.0f);
    CHECK_EQ(t.c.vf[3].uw[1], 0u);
    CHECK_EQ(bf(t.c.vf[3].uw[2]), -2.0f);
    CHECK_EQ(bf(t.c.vf[3].uw[3]), 4.0f);
    // MAC: Z em y (bit 2), S em z (bit 4+1); status Z|S + sticky
    CHECK_EQ(ops::cfc2(&t.c, 17), 0x0024u);
    CHECK_EQ(ops::cfc2(&t.c, 16), 0x00C3u);
    // vmulx.yz vf4, vf1, vf2x (broadcast de vf2.x = 0.5); x e w preservados
    setVf(t.c.vf[4], 9, 9, 9, 9);
    t.macroU(up(0x18, Y | Z, 2, 1, 4));
    CHECK_EQ(bf(t.c.vf[4].uw[0]), 9.0f);
    CHECK_EQ(bf(t.c.vf[4].uw[1]), 1.0f);
    CHECK_EQ(bf(t.c.vf[4].uw[2]), -1.5f);
    // ACC: vmula.xyzw ACC, vf1, vf2 ; vmadd.xyzw vf5, vf1, vf1 → ACC + vf1²
    t.macroU(upx(0x2A, XYZW, 2, 1));
    t.macroU(up(0x29, XYZW, 1, 1, 5));
    CHECK_EQ(bf(t.c.vf[5].uw[0]), 0.75f + 2.25f);
    CHECK_EQ(bf(t.c.vf[5].uw[1]), -4.0f + 4.0f);
    CHECK_EQ(bf(t.c.vf[5].uw[2]), -3.0f + 9.0f);
    // vf0 é constante
    t.macroU(up(0x28, XYZW, 1, 1, 0));
    CHECK_EQ(t.c.vf[0].uw[3], 0x3F800000u);
    CHECK_EQ(t.c.vf[0].uw[0], 0u);
    // Overflow: Fmax * 2 satura e marca O
    t.c.vf[6].uw[0] = ps2f::kFmax;
    setVf(t.c.vf[7], 2, 2, 2, 2);
    t.macroU(up(0x2A, X, 7, 6, 8));
    CHECK_EQ(t.c.vf[8].uw[0], ps2f::kFmax);
    CHECK_EQ(ops::cfc2(&t.c, 17) & 0xF000u, 0x8000u);
    CHECK((ops::cfc2(&t.c, 16) & 0x208u) == 0x208u);  // O e OS
}

TEST_CASE(vu, macro_misc_upper) {
    Rig t;
    // MAX/MINI com negativos
    setVf(t.c.vf[1], -1.0f, 3.0f, -0.0f, 5.0f);
    setVf(t.c.vf[2], -2.0f, 4.0f, 0.0f, -5.0f);
    t.macroU(up(0x2B, XYZW, 2, 1, 3));  // vmax
    CHECK_EQ(bf(t.c.vf[3].uw[0]), -1.0f);
    CHECK_EQ(bf(t.c.vf[3].uw[1]), 4.0f);
    CHECK_EQ(bf(t.c.vf[3].uw[3]), 5.0f);
    t.macroU(up(0x2F, XYZW, 2, 1, 3));  // vmini
    CHECK_EQ(bf(t.c.vf[3].uw[0]), -2.0f);
    CHECK_EQ(bf(t.c.vf[3].uw[3]), -5.0f);
    // FTOI4 / ITOF4 / FTOI0 (trunca) / ABS
    setVf(t.c.vf[4], 1.5f, -2.7f, 100.25f, -0.03f);
    t.macroU(upx(0x15, XYZW, 5, 4));  // vftoi4 vf5, vf4
    CHECK_EQ(t.c.vf[5].sw[0], 24);
    CHECK_EQ(t.c.vf[5].sw[1], -43);
    CHECK_EQ(t.c.vf[5].sw[2], 1604);
    t.macroU(upx(0x11, XYZW, 6, 5));  // vitof4 vf6, vf5
    CHECK_EQ(bf(t.c.vf[6].uw[0]), 1.5f);
    CHECK_EQ(bf(t.c.vf[6].uw[2]), 100.25f);
    t.macroU(upx(0x14, XYZW, 7, 4));  // vftoi0
    CHECK_EQ(t.c.vf[7].sw[1], -2);
    t.macroU(upx(0x1D, XYZW, 8, 4));  // vabs vf8, vf4
    CHECK_EQ(bf(t.c.vf[8].uw[1]), 2.7f);
    // CLIP: (2, −0.5, −3) contra |w| = 1 → +x (bit 0) e −z (bit 5)
    setVf(t.c.vf[9], 2.0f, -0.5f, -3.0f, 0.0f);
    setVf(t.c.vf[10], 0, 0, 0, -1.0f);
    t.macroU(upx(0x1F, 0xE, 10, 9));
    CHECK_EQ(ops::cfc2(&t.c, 18), 0x21u);
    t.macroU(upx(0x1F, 0xE, 10, 9));
    CHECK_EQ(ops::cfc2(&t.c, 18), (0x21u << 6) | 0x21u);
    // Produto vetorial com OPMULA/OPMSUB: (1,0,0) × (0,1,0) = (0,0,1)
    setVf(t.c.vf[11], 1, 0, 0, 0);
    setVf(t.c.vf[12], 0, 1, 0, 0);
    t.macroU(upx(0x2E, 0xE, 12, 11));      // vopmula.xyz ACC, vf11, vf12
    t.macroU(up(0x2E, 0xE, 11, 12, 13));   // vopmsub.xyz vf13, vf12, vf11
    CHECK_EQ(t.c.vf[13].uw[0], 0u);
    CHECK_EQ(t.c.vf[13].uw[1], 0u);
    CHECK_EQ(bf(t.c.vf[13].uw[2]), 1.0f);
}

TEST_CASE(vu, macro_lower_ops) {
    Rig t;
    setVf(t.c.vf[1], 1.0f, 3.0f, 0.0f, -4.0f);
    // vdiv Q, vf1x, vf1y → 1/3 truncado
    t.macroL(lox(0x38, comps(0, 1), 1, 1));
    CHECK_EQ(ops::cfc2(&t.c, 22), 0x3EAAAAAAu);
    // vsqrt Q, vf1w (negativo: usa |x| e marca I)
    t.macroL(lox(0x39, comps(0, 3), 1, 0));
    CHECK_EQ(bf(ops::cfc2(&t.c, 22)), 2.0f);
    CHECK((ops::cfc2(&t.c, 16) & 0x410u) == 0x410u);
    // vdiv por zero: ±Fmax e D
    t.macroL(lox(0x38, comps(0, 2), 1, 1));
    CHECK_EQ(ops::cfc2(&t.c, 22), ps2f::kFmax);
    CHECK((ops::cfc2(&t.c, 16) & 0x820u) == 0x820u);
    // vmulq.xyzw vf2, vf1, Q
    t.macroL(lox(0x38, comps(1, 0), 1, 1));  // Q = 3/1
    t.macroU(up(0x1C, XYZW, 0, 1, 2));
    CHECK_EQ(bf(t.c.vf[2].uw[1]), 9.0f);
    // Inteiros: viaddi vi1, vi0, 7 ; viadd vi2, vi1, vi1 ; visub vi3, vi1, vi2
    t.macroL(losp(0x32, 1, 0, 7));
    t.macroL(losp(0x30, 1, 1, 2));
    t.macroL(losp(0x31, 2, 1, 3));
    CHECK_EQ(ops::cfc2(&t.c, 1), 7u);
    CHECK_EQ(ops::cfc2(&t.c, 2), 14u);
    CHECK_EQ(ops::cfc2(&t.c, 3), 0xFFF9u);  // −7 em 16 bits
    // vmfir.xyzw vf4, vi3 (estende o sinal) ; vmtir vi4, vf1y
    t.macroL(lox(0x3D, XYZW, 4, 3));
    CHECK_EQ(t.c.vf[4].sw[2], -7);
    setVf(t.c.vf[5], 0, bf(0x12345678u), 0, 0);
    t.macroL(lox(0x3C, comps(1, 0), 4, 5));
    CHECK_EQ(ops::cfc2(&t.c, 4), 0x5678u);
    // vmr32.xyzw vf6, vf1 → (y, z, w, x)
    t.macroL(lox(0x31, XYZW, 6, 1));
    CHECK_EQ(bf(t.c.vf[6].uw[0]), 3.0f);
    CHECK_EQ(bf(t.c.vf[6].uw[3]), 1.0f);
    // vsqi/vlqi na memória do VU0: vi5 = 10
    t.macroL(losp(0x32, 5, 0, 10));
    t.macroL(lox(0x35, XYZW, 5, 1));  // vsqi.xyzw vf1, (vi5++)
    CHECK_EQ(ops::cfc2(&t.c, 5), 11u);
    t.macroL(lox(0x36, XYZW, 7, 5));  // vlqd.xyzw vf7, (--vi5)
    CHECK_EQ(bf(t.c.vf[7].uw[3]), -4.0f);
    // CTC2: status só aceita os sticky; MAC é só leitura
    ops::ctc2(&t.c, 16, 0xFFFF, 0);
    CHECK_EQ(ops::cfc2(&t.c, 16) & 0xFC0u, 0xFC0u);
    // R: vrinit + vrnext são determinísticos
    setVf(t.c.vf[8], bf(0x12345678u), 0, 0, 0);
    t.macroL(lox(0x42, comps(0, 0), 0, 8));
    CHECK_EQ(ops::cfc2(&t.c, 20), 0x3F800000u | (0x12345678u & 0x7FFFFFu));
    t.macroL(lox(0x40, XYZW, 9, 0));
    CHECK_EQ(t.c.vf[9].uw[0], vucore::advanceR(0x3F800000u | (0x12345678u & 0x7FFFFFu)));
}

// ---------------------------------------------------------------------------
// Modo micro (VU1)
// ---------------------------------------------------------------------------

TEST_CASE(vu, micro_loop_branch_delay_slot_and_e_bit) {
    Rig t;
    // vi1 = Σ 5..1 = 15 com ibne e delay slot
    t.program1({
        {lo(0x08, 0, 2, 0, 5), kUNop},             // 00 iaddiu vi2, vi0, 5
        {losp(0x30, 2, 1, 1), kUNop},               // 08 iadd vi1, vi1, vi2 (loop)
        {lo(0x09, 0, 2, 2, 1), kUNop},             // 10 isubiu vi2, vi2, 1
        {lo(0x29, 0, 2, 0, -3), kUNop},            // 18 ibne vi2, vi0, 0x08
        {losp(0x32, 3, 3, 1), kUNop},              // 20 iaddi vi3, vi3, 1 (delay slot: roda 5x)
        {kLNop, kUNop | (1u << 30)},               // 28 nop[e]
        {losp(0x32, 4, 0, 9), kUNop},              // 30 iaddi vi4, vi0, 9 (delay do E: roda)
        {losp(0x32, 5, 0, 9), kUNop},              // 38 não roda
    });
    // conferência das codificações
    const anyps2::vu::Instr chk = anyps2::vu::decode(lo(0x29, 0, 2, 0, -3), kUNop);
    CHECK_EQ(anyps2::vu::lowerText(chk, 0x18), std::string("ibne vi02,vi00,0x8"));
    t.rt.vu1().interpret(0, 0);
    CHECK_EQ(t.r1().vi[1], 15u);
    CHECK_EQ(t.r1().vi[3], 5u);
    CHECK_EQ(t.r1().vi[4], 9u);
    CHECK_EQ(t.r1().vi[5], 0u);
    CHECK_EQ(t.r1().vi[vucore::reg::TPC], 0x38u / 8);
}

TEST_CASE(vu, micro_bal_jr) {
    Rig t;
    t.program1({
        {lo(0x21, 0, 15, 0, 3), kUNop},             // 00 bal vi15, 0x20
        {losp(0x32, 1, 0, 1), kUNop},               // 08 iaddi vi1, vi0, 1 (delay)
        {kLNop, kUNop | (1u << 30)},                // 10 nop[e]  (retorno cai aqui)
        {kLNop, kUNop},                             // 18
        {losp(0x32, 2, 0, 2), kUNop},               // 20 iaddi vi2, vi0, 2
        {lo(0x24, 0, 0, 15, 0), kUNop},             // 28 jr vi15
        {kLNop, kUNop},                             // 30 delay
    });
    t.rt.vu1().interpret(0, 0);
    CHECK_EQ(t.r1().vi[15], 2u);  // (0 + 16) / 8
    CHECK_EQ(t.r1().vi[1], 1u);
    CHECK_EQ(t.r1().vi[2], 2u);
}

TEST_CASE(vu, micro_flag_latency_and_stalls) {
    Rig t;
    setVf(t.r1().vf[2], 1, 1, 1, 1);
    setVf(t.r1().vf[3], -1, -1, -1, -1);
    t.r1().vi[2] = 0xFFFF;
    t.program1({
        {kLNop, up(0x28, XYZW, 3, 2, 1)},           // 00 add vf1, vf2, vf3 → 0 (Z em xyzw)
        {lo(0x1A, 0, 1, 2, 0), kUNop},              // 08 fmand vi1, vi2 → MAC antigo
        {kLNop, kUNop},
        {kLNop, kUNop},
        {lo(0x1A, 0, 4, 2, 0), kUNop},              // 20 fmand vi4, vi2 → 4 ciclos depois: novo
        {kLNop, kUNop | (1u << 30)},
        {kLNop, kUNop},
    });
    t.rt.vu1().interpret(0, 0);
    CHECK_EQ(t.r1().vi[1], 0u);
    CHECK_EQ(t.r1().vi[4], 0x000Fu);

    // Com dependência, o stall adianta a visibilidade das flags.
    Rig s;
    setVf(s.r1().vf[2], 1, 1, 1, 1);
    setVf(s.r1().vf[3], -1, -1, -1, -1);
    s.r1().vi[2] = 0xFFFF;
    s.program1({
        {kLNop, up(0x28, X, 3, 2, 1)},              // add.x vf1, vf2, vf3 → Z(x)
        {kLNop, up(0x28, X, 3, 1, 4)},              // add.x vf4, vf1, vf3 → espera vf1 (stall até o ciclo 4)
        {lo(0x1A, 0, 1, 2, 0), kUNop},              // fmand: vê o MAC do primeiro add
        {kLNop, kUNop | (1u << 30)},
        {kLNop, kUNop},
    });
    s.rt.vu1().interpret(0, 0);
    CHECK_EQ(s.r1().vi[1], 0x0008u);
    CHECK_EQ(bf(s.r1().vf[4].uw[0]), -1.0f);
    CHECK_EQ(s.r1().vi[vucore::reg::Mac], 0x0080u);  // fim: pipeline concluído (S em x)
}

TEST_CASE(vu, micro_q_latency_and_waitq) {
    Rig t;
    setVf(t.r1().vf[1], 6, 0, 0, 0);
    setVf(t.r1().vf[2], 2, 0, 0, 0);
    setVf(t.r1().vf[4], 1, 1, 1, 1);
    t.r1().vi[vucore::reg::Q] = fb(10.0f);
    t.program1({
        {lox(0x38, comps(0, 0), 2, 1), kUNop},      // div Q, vf1x, vf2x (pronto no ciclo 7)
        {kLNop, up(0x1C, X, 0, 4, 3)},              // mulq.x vf3, vf4, Q → Q antigo (10)
        {lox(0x3B, 0, 0, 0), kUNop},                // waitq
        {kLNop, up(0x1C, X, 0, 4, 5)},              // mulq.x vf5, vf4, Q → 3
        {kLNop, kUNop | (1u << 30)},
        {kLNop, kUNop},
    });
    t.rt.vu1().interpret(0, 0);
    CHECK_EQ(bf(t.r1().vf[3].uw[0]), 10.0f);
    CHECK_EQ(bf(t.r1().vf[5].uw[0]), 3.0f);
}

TEST_CASE(vu, micro_upper_lower_parallel_and_loi) {
    Rig t;
    setVf(t.r1().vf[1], 1, 2, 3, 4);
    setVf(t.r1().vf[2], 10, 20, 30, 40);
    t.program1({
        // add vf3, vf1, vf2  |  move vf1, vf2 : o upper lê o vf1 antigo
        {lox(0x30, XYZW, 1, 2), up(0x28, XYZW, 2, 1, 3)},
        // muli.x vf4, vf1, I com LOI 0.5: o upper usa o I antigo (2.0)
        {fb(0.5f), up(0x1E, X, 0, 1, 4) | (1u << 31)},
        // muli.x vf5, vf1, I agora com I = 0.5
        {kLNop, up(0x1E, X, 0, 1, 5)},
        // mesmo destino no upper e no lower: vale o upper
        {lox(0x30, XYZW, 6, 2), up(0x28, XYZW, 1, 1, 6)},
        // o lower lê o vf7 antigo mesmo com o upper do par escrevendo nele
        {lox(0x30, XYZW, 8, 7), up(0x28, XYZW, 1, 1, 7)},
        {kLNop, kUNop | (1u << 30)},
        {kLNop, kUNop},
    });
    t.r1().vi[vucore::reg::I] = fb(2.0f);
    setVf(t.r1().vf[7], 7.5f, 0, 0, 0);
    t.rt.vu1().interpret(0, 0);
    CHECK_EQ(bf(t.r1().vf[3].uw[3]), 44.0f);
    CHECK_EQ(bf(t.r1().vf[1].uw[0]), 10.0f);
    CHECK_EQ(bf(t.r1().vf[4].uw[0]), 20.0f);
    CHECK_EQ(bf(t.r1().vf[5].uw[0]), 5.0f);
    CHECK_EQ(bf(t.r1().vf[6].uw[0]), 20.0f);  // vf1 (=vf2 agora) * 2
    CHECK_EQ(bf(t.r1().vf[8].uw[0]), 7.5f);   // valor antigo de vf7
    CHECK_EQ(bf(t.r1().vf[7].uw[0]), 20.0f);
}

TEST_CASE(vu, micro_memory_and_efu) {
    Rig t;
    setVf(t.r1().vf[1], 3, 4, 0, 0);
    t.program1({
        {lo(0x08, 0, 1, 0, 0x20), kUNop},           // iaddiu vi1, vi0, 0x20
        {lo(0x01, XYZW, 1, 1, 2), kUNop},           // sq vf1, 2(vi1) → qword 0x22
        {lo(0x00, Y, 2, 1, 2), kUNop},              // lq.y vf2, 2(vi1)
        {lo(0x05, Z, 1, 1, 5), kUNop},              // isw.z vi1, 5(vi1) → qword 0x25.z = 0x20
        {lo(0x04, Z, 3, 1, 5), kUNop},              // ilw.z vi3, 5(vi1)
        {lox(0x72, 0xE, 0, 1), kUNop},              // eleng P, vf1 → 5
        {lox(0x7B, 0, 0, 0), kUNop},                // waitp
        {lox(0x64, X, 4, 0), kUNop},                // mfp.x vf4, P
        {kLNop, kUNop | (1u << 30)},
        {kLNop, kUNop},
    });
    t.rt.vu1().interpret(0, 0);
    CHECK_EQ(bf(t.r1().vf[2].uw[1]), 4.0f);
    CHECK_EQ(t.r1().vi[3], 0x20u);
    std::uint32_t m;
    std::memcpy(&m, t.rt.vu().data1.get() + 0x22 * 16, 4);
    CHECK_EQ(bf(m), 3.0f);
    CHECK_EQ(bf(t.r1().vf[4].uw[0]), 5.0f);
}

TEST_CASE(vu, micro_errors_are_explicit) {
    Rig t;
    t.program1({{lo(0x02, 0, 0, 0, 0), kUNop}, {kLNop, kUNop | (1u << 30)}, {kLNop, kUNop}});
    CHECK_THROWS_WITH(t.rt.vu1().interpret(0, 0x1234), "instrução lower inválida");
    // Laço infinito vira erro
    Rig u;
    u.program1({{lo(0x20, 0, 0, 0, -1), kUNop}, {kLNop, kUNop}});
    CHECK_THROWS_WITH(u.rt.vu1().interpret(0, 0), "não terminou");
}

TEST_CASE(vu, xgkick_and_vif1_mscal_double_buffer) {
    Rig t;
    // Pacote GIF (A+D FINISH, EOP) no qword 0x10 da memória do VU1.
    const std::uint64_t pkt[4] = {1ull | (1ull << 15) | (1ull << 60), 0xE, 0, anyps2::rt::gs::FINISH};
    std::memcpy(t.rt.vu().data1.get() + 0x10 * 16, pkt, sizeof(pkt));
    t.program1({
        {lox(0x68, 0, 2, 0), kUNop},                // xtop vi2
        {lo(0x08, 0, 1, 0, 0x10), kUNop},           // iaddiu vi1, vi0, 0x10
        {lox(0x6C, 0, 0, 1), kUNop},                // xgkick vi1
        {lo(0x05, X, 2, 0, 0), kUNop},              // isw.x vi2, 0(vi0)
        {kLNop, kUNop | (1u << 30)},
        {kLNop, kUNop},
    });
    // VIF1: BASE 0x100, OFFSET 0x80, MSCAL 0 duas vezes → TOP 0x100 e depois 0x180
    const std::uint32_t vifcodes[] = {0x03000100u, 0x02000080u, 0x14000000u};
    t.rt.vif1().transfer(reinterpret_cast<const std::uint8_t*>(vifcodes), sizeof(vifcodes), 0);
    t.rt.gs().processEvents(~std::uint64_t{0});  // FINISH chega depois do trabalho do GS
    CHECK_EQ(t.rt.gs().csr() & 2, 2ull);
    std::uint32_t top;
    std::memcpy(&top, t.rt.vu().data1.get(), 4);
    CHECK_EQ(top, 0x100u);
    const std::uint32_t again[] = {0x14000000u};
    t.rt.vif1().transfer(reinterpret_cast<const std::uint8_t*>(again), sizeof(again), 0);
    std::memcpy(&top, t.rt.vu().data1.get(), 4);
    CHECK_EQ(top, 0x180u);
    // CTC2 em CMSAR1 também inicia o VU1
    t.rt.gs().writePrivileged(0x12001000, 2, 0);
    ops::ctc2(&t.c, 31, 0, 0);
    t.rt.gs().processEvents(~std::uint64_t{0});
    CHECK_EQ(t.rt.gs().csr() & 2, 2ull);
    CHECK_EQ(t.rt.vu1().interpretedRuns(), 3u);
}
