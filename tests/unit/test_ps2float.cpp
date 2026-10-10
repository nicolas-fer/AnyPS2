// ps2float.h: as versões sem desvio de add/sub/mul têm de dar, bit a bit, o
// mesmo resultado das versões com desvio que elas substituíram (copiadas aqui).

#include <cstdint>
#include <string>

#include "anyps2/runtime/ps2float.h"
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
