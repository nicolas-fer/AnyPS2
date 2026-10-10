// ps2float.h: as versões sem desvio de add/sub/mul têm de dar, bit a bit, o
// mesmo resultado das versões com desvio que elas substituíram (copiadas aqui).

#include <cstdint>
#include <string>

#include "anyps2/runtime/ps2float.h"
#include "anyps2/runtime/vu/vu_core.h"
#include "minitest.h"

namespace ps2f = anyps2::rt::ps2f;

namespace {

// Versões de referência: as originais, com desvios.
namespace ref {
inline float add(float a, float b) {
    const float s = a + b;
    if (!std::isfinite(s)) return s;
    const float bb = s - a;
    const float err = (a - (s - bb)) + (b - bb);
    if (err != 0.0f && ((err < 0.0f) != (s < 0.0f))) return ps2f::towardZero(s);
    return s;
}
inline float sub(float a, float b) { return add(a, -b); }
inline float mul(float a, float b) {
    const double exact = static_cast<double>(a) * static_cast<double>(b);
    float r = static_cast<float>(exact);
    if (!std::isfinite(r)) return r;
    if (std::fabs(static_cast<double>(r)) > std::fabs(exact)) r = ps2f::towardZero(r);
    return r;
}
inline float madd(float a, float x, float y) {
    const ps2f::Out p = ps2f::out(ref::mul(x, y));
    return ref::add(a, ps2f::in(p.bits));
}
inline float msub(float a, float x, float y) {
    const ps2f::Out p = ps2f::out(ref::mul(x, y));
    return ref::sub(a, ps2f::in(p.bits));
}
}  // namespace ref

struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::uint32_t u32() { return static_cast<std::uint32_t>(next() >> 16); }
};

// Valores de borda do formato do PS2 (e alguns que só o host produz).
constexpr std::uint32_t kEdges[] = {
    0x00000000u, 0x80000000u,  // ±0
    0x00000001u, 0x80000001u,  // menores denormais
    0x007FFFFFu, 0x807FFFFFu,  // maiores denormais
    0x00800000u, 0x80800000u,  // menores normais
    0x00800001u, 0x80800001u,
    0x3F800000u, 0xBF800000u,  // ±1
    0x3F7FFFFFu, 0x3F800001u, 0xBF7FFFFFu, 0xBF800001u,
    0x33800000u, 0x34000000u,  // 2^-24, 2^-23
    0x7F7FFFFFu, 0xFF7FFFFFu,  // ±Fmax
    0x7F7FFFFEu, 0x7F000000u, 0x7E800000u, 0x7F800000u, 0xFF800000u,  // expoentes extremos, ±inf
    0x7FC00000u, 0xFFC00000u, 0x7F800001u,                            // NaN
};
constexpr std::uint32_t kNumEdges = sizeof(kEdges) / sizeof(kEdges[0]);

std::uint32_t randomBits(Rng& g) {
    switch (g.u32() % 8) {
        case 0: return kEdges[g.u32() % kNumEdges];
        case 1: return kEdges[g.u32() % kNumEdges] + (g.u32() % 5) - 2;            // vizinhos de uma borda
        case 2: return (g.u32() & 0x807FFFFFu) | ((g.u32() % 12 + 120) << 23);     // expoentes perto de 1
        case 3: return (g.u32() & 0x807FFFFFu) | ((g.u32() % 6) << 23);            // pequenos
        case 4: return (g.u32() & 0x807FFFFFu) | ((250 + g.u32() % 6) << 23);      // grandes
        case 5: return (g.u32() & 0x80000000u) | ((g.u32() & 0x00FFFFFFu) << 7);   // mantissas curtas
        default: return g.u32();
    }
}

// Segundo operando relacionado ao primeiro: cancelamento, metade de ulp, mesmo expoente.
std::uint32_t relatedBits(Rng& g, std::uint32_t a) {
    const int e = static_cast<int>((a >> 23) & 0xFF);
    switch (g.u32() % 6) {
        case 0: return a ^ 0x80000000u;                        // cancelamento exato
        case 1: return (a ^ 0x80000000u) + (g.u32() % 3) - 1;  // cancelamento quase total
        case 2: {                                              // 22..26 expoentes abaixo: erro de meio ulp
            const int ne = e - 22 - static_cast<int>(g.u32() % 5);
            if (ne < 1) return randomBits(g);
            return (g.u32() & 0x807FFFFFu) | (static_cast<std::uint32_t>(ne) << 23);
        }
        case 3: return (g.u32() & 0x80000000u) | (static_cast<std::uint32_t>(e) << 23) | (g.u32() & 0x7FFFFFu);
        case 4: return (a & 0x80000000u) | ((a + (g.u32() % 3)) & 0x7FFFFFFFu);
        default: return randomBits(g);
    }
}

bool same(float a, float b) { return ps2f::toBits(a) == ps2f::toBits(b); }

}  // namespace

TEST_CASE(runtime_ops, ps2float_branchless_matches_branching) {
    Rng g{0x9E3779B97F4A7C15ull};
    constexpr std::uint32_t kIters = 4'000'000;
    unsigned bad = 0;
    auto report = [&](const char* op, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        if (bad++ < 5) {
            ::minitest::reportFailure(__FILE__, __LINE__,
                                      std::string(op) + " difere: " + minitest::show(a) + ", " +
                                          minitest::show(b) + ", " + minitest::show(c));
        }
    };
    // Todas as combinações das bordas entre si, depois sorteios.
    for (std::uint32_t i = 0; i < kNumEdges * kNumEdges + kIters; ++i) {
        std::uint32_t ab, bb, cb;
        if (i < kNumEdges * kNumEdges) {
            ab = kEdges[i / kNumEdges];
            bb = kEdges[i % kNumEdges];
            cb = kEdges[(i * 7u) % kNumEdges];
        } else {
            ab = randomBits(g);
            bb = (g.u32() & 1) ? relatedBits(g, ab) : randomBits(g);
            cb = (g.u32() & 1) ? relatedBits(g, bb) : randomBits(g);
        }
        // Valores brutos do host (com denormais, infinito e NaN) e valores do PS2 (via in()).
        for (int raw = 0; raw < 2; ++raw) {
            const float a = raw ? ps2f::fromBits(ab) : ps2f::in(ab);
            const float b = raw ? ps2f::fromBits(bb) : ps2f::in(bb);
            const float c = raw ? ps2f::fromBits(cb) : ps2f::in(cb);
            // NaN de entrada fica de fora: o PS2 não tem NaN (in() o troca por Fmax) e, com dois
            // NaN, qual carga útil sai depende da ordem dos operandos que o compilador escolheu.
            if (a != a || b != b || c != c) continue;
            if (!same(ps2f::add(a, b), ref::add(a, b))) report("add", ab, bb, 0);
            if (!same(ps2f::sub(a, b), ref::sub(a, b))) report("sub", ab, bb, 0);
            if (!same(ps2f::mul(a, b), ref::mul(a, b))) report("mul", ab, bb, 0);
            const ps2f::Out pm = ps2f::out(ps2f::mul(b, c));
            if (!same(ps2f::add(a, ps2f::in(pm.bits)), ref::madd(a, b, c))) report("madd", ab, bb, cb);
            if (!same(ps2f::sub(a, ps2f::in(pm.bits)), ref::msub(a, b, c))) report("msub", ab, bb, cb);
        }
    }
    CHECK_EQ(bad, 0u);
}

// ---------------------------------------------------------------------------
// O caminho vetorial das instruções upper (vu_core.h) contra o laço escalar:
// valor de cada componente, flags MAC e máscara dest têm de ser iguais bit a bit.
// ---------------------------------------------------------------------------
namespace {

namespace vc = anyps2::rt::vucore;
using anyps2::rt::Reg128;
using anyps2::vu::U;

enum class Fn { Add, Sub, Mul, Madd, Msub };
enum class Kind { Vec, Bc, Q, I };
struct OpDesc {
    U op;
    Fn fn;
    Kind kind;
    bool toAcc;
};

// As instruções que ganharam o caminho vetorial.
constexpr OpDesc kOps[] = {
    {U::ADD, Fn::Add, Kind::Vec, false},    {U::SUB, Fn::Sub, Kind::Vec, false},
    {U::MUL, Fn::Mul, Kind::Vec, false},    {U::MADD, Fn::Madd, Kind::Vec, false},
    {U::MSUB, Fn::Msub, Kind::Vec, false},  {U::ADDbc, Fn::Add, Kind::Bc, false},
    {U::SUBbc, Fn::Sub, Kind::Bc, false},   {U::MULbc, Fn::Mul, Kind::Bc, false},
    {U::MADDbc, Fn::Madd, Kind::Bc, false}, {U::MSUBbc, Fn::Msub, Kind::Bc, false},
    {U::ADDq, Fn::Add, Kind::Q, false},     {U::SUBq, Fn::Sub, Kind::Q, false},
    {U::MULq, Fn::Mul, Kind::Q, false},     {U::MADDq, Fn::Madd, Kind::Q, false},
    {U::MSUBq, Fn::Msub, Kind::Q, false},   {U::ADDi, Fn::Add, Kind::I, false},
    {U::SUBi, Fn::Sub, Kind::I, false},     {U::MULi, Fn::Mul, Kind::I, false},
    {U::MADDi, Fn::Madd, Kind::I, false},   {U::MSUBi, Fn::Msub, Kind::I, false},
    {U::ADDA, Fn::Add, Kind::Vec, true},    {U::SUBA, Fn::Sub, Kind::Vec, true},
    {U::MULA, Fn::Mul, Kind::Vec, true},    {U::MADDA, Fn::Madd, Kind::Vec, true},
    {U::MSUBA, Fn::Msub, Kind::Vec, true},  {U::ADDAbc, Fn::Add, Kind::Bc, true},
    {U::SUBAbc, Fn::Sub, Kind::Bc, true},   {U::MULAbc, Fn::Mul, Kind::Bc, true},
    {U::MADDAbc, Fn::Madd, Kind::Bc, true}, {U::MSUBAbc, Fn::Msub, Kind::Bc, true},
    {U::ADDAq, Fn::Add, Kind::Q, true},     {U::SUBAq, Fn::Sub, Kind::Q, true},
    {U::MULAq, Fn::Mul, Kind::Q, true},     {U::MADDAq, Fn::Madd, Kind::Q, true},
    {U::MSUBAq, Fn::Msub, Kind::Q, true},   {U::ADDAi, Fn::Add, Kind::I, true},
    {U::SUBAi, Fn::Sub, Kind::I, true},     {U::MULAi, Fn::Mul, Kind::I, true},
    {U::MADDAi, Fn::Madd, Kind::I, true},   {U::MSUBAi, Fn::Msub, Kind::I, true},
};

// Referência: o laço escalar de antes do caminho vetorial, só com ps2f:: escalar.
vc::UpperResult refUpper(const OpDesc& d, const vc::Regs& R, const anyps2::vu::Upper& u, std::uint32_t qBits,
                         std::uint32_t iBits) {
    vc::UpperResult r;
    r.dest = u.dest;
    r.srcA = u.fs;
    r.srcB = u.ft;
    r.writes = true;
    r.toAcc = d.toAcc;
    r.reg = static_cast<std::uint8_t>(d.toAcc ? 0 : u.fd);
    r.setsFlags = true;
    r.readsAcc = d.fn == Fn::Madd || d.fn == Fn::Msub;
    if (d.kind != Kind::Vec && d.kind != Kind::Bc) r.srcB = 0;
    const Reg128& s = R.vf[u.fs];
    const Reg128& t = R.vf[u.ft];
    for (unsigned c = 0; c < 4; ++c) {
        if (!vc::hasComp(u.dest, c)) continue;
        const float x = ps2f::in(s.uw[c]);
        const float y = ps2f::in(d.kind == Kind::Vec ? t.uw[c]
                                 : d.kind == Kind::Bc ? t.uw[u.bc]
                                 : d.kind == Kind::Q  ? qBits
                                                      : iBits);
        const float a = ps2f::in(R.acc->uw[c]);
        switch (d.fn) {
            case Fn::Add: vc::setComp(r, c, ps2f::add(x, y)); break;
            case Fn::Sub: vc::setComp(r, c, ps2f::sub(x, y)); break;
            case Fn::Mul: vc::setComp(r, c, ps2f::mul(x, y)); break;
            case Fn::Madd: vc::setComp(r, c, ps2f::add(a, ps2f::in(ps2f::out(ps2f::mul(x, y)).bits))); break;
            case Fn::Msub: vc::setComp(r, c, ps2f::sub(a, ps2f::in(ps2f::out(ps2f::mul(x, y)).bits))); break;
        }
    }
    return r;
}

}  // namespace

TEST_CASE(runtime_ops, ps2float_vector_upper_matches_scalar) {
    Rng g{0xC0FFEE1234567ull};
    Reg128 vf[32] = {};
    Reg128 acc{};
    std::uint32_t vi[32] = {};
    const vc::Regs R{vf, vi, &acc};
    constexpr unsigned kIters = 1'500'000;
    unsigned bad = 0;
    // Valor do PS2: sempre a palavra crua (denormais e expoente 255 incluídos; in() os trata).
    auto word = [&]() { return (g.u32() % 11 == 0) ? kEdges[g.u32() % kNumEdges] : randomBits(g); };
    for (unsigned i = 0; i < kIters; ++i) {
        for (unsigned c = 0; c < 4; ++c) {
            const std::uint32_t a = word();
            vf[1].uw[c] = a;
            vf[2].uw[c] = (g.u32() & 1) ? relatedBits(g, a) : word();
            acc.uw[c] = (g.u32() & 1) ? relatedBits(g, a) : word();
        }
        const std::uint32_t q = word(), iw = (g.u32() & 3) == 0 ? q : word();
        for (const OpDesc& d : kOps) {
            anyps2::vu::Upper u;
            u.op = d.op;
            // Todas as 16 máscaras dest ao longo das iterações (e a cheia em metade delas).
            u.dest = static_cast<std::uint8_t>((i & 1) ? 0xF : (g.u32() & 0xF));
            u.fs = 1;
            u.ft = 2;
            u.fd = 3;
            u.bc = static_cast<std::uint8_t>(g.u32() & 3);
            const vc::UpperResult got = vc::computeUpper(R, u, q, iw);
            const vc::UpperResult want = refUpper(d, R, u, q, iw);
            const bool ok = got.writes == want.writes && got.toAcc == want.toAcc && got.reg == want.reg &&
                            got.dest == want.dest && got.setsFlags == want.setsFlags && got.mac == want.mac &&
                            got.srcA == want.srcA && got.srcB == want.srcB && got.readsAcc == want.readsAcc &&
                            std::memcmp(got.value.uw, want.value.uw, 16) == 0;
            if (!ok && bad++ < 5) {
                ::minitest::reportFailure(
                    __FILE__, __LINE__,
                    "upper vetorial difere (op " + std::to_string(static_cast<int>(d.op)) + ", dest " +
                        std::to_string(u.dest) + ", mac " + minitest::show(static_cast<unsigned>(got.mac)) + " x " +
                        minitest::show(static_cast<unsigned>(want.mac)) + ") fs=" + minitest::show(vf[1].uw[0]) +
                        " ft=" + minitest::show(vf[2].uw[0]));
            }
        }
    }
    CHECK_EQ(bad, 0u);
}