// Memória do VU1 pelo EE como dispositivo (fatia 2 do plano do VU1 em outra thread): o EE
// vê a mesma memória, nos mesmos endereços e espelhos, com o mesmo comportamento de antes
// (quando era RAM direta nas páginas do Memory), e todo acesso passa pelo gancho de fence.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/memory.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/vu1_mem.h"
#include "minitest.h"

using namespace anyps2::rt;

namespace {

constexpr std::uint32_t kHalf = VuMemory::kVu1Size;

// Referência: o comportamento antigo, as duas memórias mapeadas como RAM nas páginas. Os
// buffers ficam num bloco só com um vão entre eles, como acontece com as alocações do
// VuMemory (não contíguas): um acesso em bloco que cruza do micro para os dados falha.
struct Old {
    std::vector<std::uint8_t> blob = std::vector<std::uint8_t>(3 * kHalf, 0);
    Memory mem;
    Old() {
        mem.mapRam(0x11008000u, micro(), kHalf);
        mem.mapRam(0x1100C000u, data(), kHalf);
    }
    std::uint8_t* micro() { return blob.data(); }
    std::uint8_t* data() { return blob.data() + 2 * kHalf; }
};

// O novo caminho: dispositivo com fence.
struct New {
    VuMemory vu;
    Memory mem;
    Vu1Memory dev{vu.micro1.get(), vu.data1.get()};
    std::uint64_t fences = 0;
    New() {
        dev.attach(mem);
        dev.setFence([](void* c) { ++*static_cast<std::uint64_t*>(c); }, &fences);
    }
    std::uint8_t* micro() { return vu.micro1.get(); }
    std::uint8_t* data() { return vu.data1.get(); }
};

struct Outcome {
    std::string error;
    std::uint64_t lo = 0, hi = 0;
    bool operator==(const Outcome&) const = default;
};

template <typename F>
Outcome observe(F&& f) {
    Outcome o;
    try {
        f(o);
    } catch (const std::exception& e) {
        o.error = e.what();
    }
    return o;
}

// Uma operação do EE: leitura ou escrita de `size` bytes em `addr`.
template <typename M>
Outcome access(M& m, bool write, std::uint32_t addr, unsigned size, std::uint64_t v0, std::uint64_t v1) {
    return observe([&](Outcome& o) {
        Reg128 q{};
        switch (size) {
            case 1:
                if (write) m.template write<std::uint8_t>(addr, static_cast<std::uint8_t>(v0), 0x100);
                else o.lo = m.template read<std::uint8_t>(addr, 0x100);
                break;
            case 2:
                if (write) m.template write<std::uint16_t>(addr, static_cast<std::uint16_t>(v0), 0x100);
                else o.lo = m.template read<std::uint16_t>(addr, 0x100);
                break;
            case 4:
                if (write) m.template write<std::uint32_t>(addr, static_cast<std::uint32_t>(v0), 0x100);
                else o.lo = m.template read<std::uint32_t>(addr, 0x100);
                break;
            case 8:
                if (write) m.template write<std::uint64_t>(addr, v0, 0x100);
                else o.lo = m.template read<std::uint64_t>(addr, 0x100);
                break;
            default:
                // LQ/SQ ignoram os 4 bits baixos do endereço (ops.h).
                if (write) {
                    q.ud[0] = v0;
                    q.ud[1] = v1;
                    m.write128(addr & ~15u, q, 0x100);
                } else {
                    m.read128(addr & ~15u, q, 0x100);
                    o.lo = q.ud[0];
                    o.hi = q.ud[1];
                }
                break;
        }
    });
}

}  // namespace

// LB/LH/LW/LD/LQ e SB/SH/SW/SD/SQ em toda a faixa, nos espelhos que existem e fora deles,
// alinhados e desalinhados: o mesmo resultado, a mesma mensagem de erro e os mesmos bytes
// que a RAM direta de antes.
TEST_CASE(vu1_mem, ee_accesses_match_the_old_direct_ram) {
    Old o;
    New n;
    std::uint64_t seed = 0x1234567;
    auto rnd = [&] {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return seed >> 33;
    };
    // Bases: as duas metades, o espelho kseg1 (os dois que existem) e vizinhos que NÃO são
    // espelhos (kseg0, uncached, físico sem prefixo logo antes e depois da faixa).
    const std::uint32_t bases[] = {0x11008000u, 0x1100C000u, 0xB1008000u, 0xB100C000u, 0x91008000u,
                                   0x81008000u, 0x31008000u, 0x21008000u, 0x01008000u, 0x11007FF0u,
                                   0x11010000u, 0xB1007FF0u, 0xB1010000u, 0x1100BFF0u, 0x1100FFF0u,
                                   0xB100BFF0u, 0xB100FFF0u, 0xA1008000u, 0x51008000u};
    const unsigned sizes[] = {1, 2, 4, 8, 16};
    std::uint64_t okReads = 0, okWrites = 0, errors = 0;
    for (int i = 0; i < 200000; ++i) {
        const std::uint32_t base = bases[rnd() % (sizeof bases / sizeof bases[0])];
        const std::uint32_t off = static_cast<std::uint32_t>(rnd() % (2 * kHalf + 64));
        const unsigned size = sizes[rnd() % 5];
        const bool write = (rnd() & 1) != 0;
        const std::uint64_t v0 = rnd() * 0x9E3779B97F4A7C15ull, v1 = rnd() * 0xC2B2AE3D27D4EB4Full;
        const std::uint32_t addr = base + off;
        const Outcome a = access(o.mem, write, addr, size, v0, v1);
        const Outcome b = access(n.mem, write, addr, size, v0, v1);
        if (!(a == b)) {
            std::fprintf(stderr, "divergência em %08x tamanho %u %s: antigo [%s] %llx/%llx, novo [%s] %llx/%llx\n", addr,
                         size, write ? "escrita" : "leitura", a.error.c_str(),
                         static_cast<unsigned long long>(a.lo), static_cast<unsigned long long>(a.hi),
                         b.error.c_str(), static_cast<unsigned long long>(b.lo),
                         static_cast<unsigned long long>(b.hi));
        }
        CHECK(a == b);
        if (!a.error.empty()) ++errors;
        else if (write) ++okWrites;
        else ++okReads;
        if (i % 1024 == 0) {
            CHECK(std::memcmp(o.micro(), n.micro(), kHalf) == 0);
            CHECK(std::memcmp(o.data(), n.data(), kHalf) == 0);
        }
    }
    CHECK(std::memcmp(o.micro(), n.micro(), kHalf) == 0);
    CHECK(std::memcmp(o.data(), n.data(), kHalf) == 0);
    CHECK(okReads > 1000 && okWrites > 1000 && errors > 1000);  // o sorteio exercitou os três casos
    // O gancho foi chamado (a contagem exata por acesso está em fence_runs_before_every_access).
    CHECK(n.fences > 0);
}

// O desalinhamento continua sendo exceção de endereço (GuestError) com a mesma mensagem.
TEST_CASE(vu1_mem, unaligned_still_throws_guest_error) {
    New n;
    CHECK_THROWS_WITH(n.mem.read<std::uint32_t>(0x1100C002, 0x40), "leitura desalinhada de 32 bits");
    CHECK_THROWS_WITH(n.mem.write<std::uint16_t>(0xB1008001, 1, 0x40), "escrita desalinhada de 16 bits");
    CHECK_THROWS_WITH(n.mem.read<std::uint64_t>(0x11008004, 0x40), "leitura desalinhada de 64 bits");
    CHECK_EQ(n.fences, 0ull);  // o erro vem antes de chegar ao dispositivo
    // Um byte nunca é desalinhado.
    n.mem.write<std::uint8_t>(0x1100C003, 0x5A, 0);
    CHECK_EQ(n.data()[3], 0x5A);
}

// Os dois lados enxergam os mesmos bytes: o que o VU/VIF escrevem o EE lê, e vice-versa; o
// espelho kseg1 também.
TEST_CASE(vu1_mem, ee_and_vu_see_the_same_bytes) {
    const ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    RuntimeOptions opt;
    opt.virtualClock = true;
    Runtime rt(info, opt);
    Memory& m = rt.memory();
    const std::uint32_t v = 0xCAFEBABE;
    std::memcpy(rt.vu().data1.get() + 0x120, &v, 4);
    CHECK_EQ(m.read<std::uint32_t>(0x1100C120, 0), v);
    CHECK_EQ(m.read<std::uint32_t>(0xB100C120, 0), v);
    m.write<std::uint32_t>(0xB1008040, 0x11223344, 0);
    std::uint32_t got;
    std::memcpy(&got, rt.vu().micro1.get() + 0x40, 4);
    CHECK_EQ(got, 0x11223344u);
    // VU0 não mudou: continua RAM direta.
    m.write<std::uint32_t>(0x11004010, 0x55667788, 0);
    std::memcpy(&got, rt.vu().data0.get() + 0x10, 4);
    CHECK_EQ(got, 0x55667788u);
    // Fora da faixa de antes continua sem mapa (kseg0 do VU1).
    CHECK_THROWS_WITH(m.read<std::uint32_t>(0x9100C120, 0), "endereço não mapeado");
}

// Cópias em bloco do HLE (copyFromGuest/copyToGuest/readCString) e hostPointer.
TEST_CASE(vu1_mem, block_copies_and_host_pointer) {
    Old o;
    New n;
    std::uint8_t src[64];
    for (unsigned i = 0; i < sizeof src; ++i) src[i] = static_cast<std::uint8_t>(0x30 + i);
    const std::uint32_t addrs[] = {0x11008000u, 0x1100C010u, 0xB1008FF0u, 0x1100BFE0u,  // cruza o meio: falha
                                   0xB100BFE0u, 0x1100FFE0u, 0x1100FFF0u, 0x11010000u, 0x11007FE0u, 0x91008000u};
    for (std::uint32_t a : addrs) {
        for (std::uint32_t size : {0u, 1u, 7u, 16u, 33u, 64u}) {
            const Outcome w0 = observe([&](Outcome&) { o.mem.copyToGuest(a, src, size, 0x77); });
            const Outcome w1 = observe([&](Outcome&) { n.mem.copyToGuest(a, src, size, 0x77); });
            CHECK(w0 == w1);
            std::uint8_t b0[64] = {}, b1[64] = {};
            const Outcome r0 = observe([&](Outcome&) { o.mem.copyFromGuest(b0, a, size, 0x77); });
            const Outcome r1 = observe([&](Outcome&) { n.mem.copyFromGuest(b1, a, size, 0x77); });
            CHECK(r0 == r1);
            CHECK(std::memcmp(b0, b1, sizeof b0) == 0);
        }
    }
    CHECK(std::memcmp(o.micro(), n.micro(), kHalf) == 0);
    CHECK(std::memcmp(o.data(), n.data(), kHalf) == 0);
    // Strings.
    const char text[] = "VU1 texto";
    o.mem.copyToGuest(0x1100C100, text, sizeof text, 0);
    n.mem.copyToGuest(0x1100C100, text, sizeof text, 0);
    CHECK(o.mem.readCString(0x1100C100, 64, 0) == n.mem.readCString(0x1100C100, 64, 0));
    CHECK(n.mem.readCString(0xB100C100, 64, 0) == "VU1 texto");
    // hostPointer: a faixa não é mais RAM direta (quem precisa usa as cópias); o resto não muda.
    CHECK(o.mem.hostPointer(0x1100C100, 4) != nullptr);
    CHECK(n.mem.hostPointer(0x1100C100, 4) == nullptr);
    CHECK(n.mem.hostPointer(0xB1008000, 4) == nullptr);
    CHECK(n.mem.hostPointer(0x00100000, 4) != nullptr);
    CHECK(n.mem.isVirtualDevice(0x1100C100, 16));
    CHECK(!n.mem.isVirtualDevice(0x11010000, 4));
    CHECK(!n.mem.isVirtualDevice(0x91008000, 4));
    // O gancho foi chamado em toda cópia e em cada byte da string.
    CHECK(n.fences > 0);
}

// Custo do caminho lento (não afirma nada; imprime com ANYPS2_BENCH=1). O GT4 não acessa
// essa faixa na corrida, mas outros jogos podem.
TEST_CASE(vu1_mem, slow_path_cost) {
    const char* e = std::getenv("ANYPS2_BENCH");
    if (!e || !*e || std::string(e) == "0") return;
    Old o;
    New n;
    auto bench = [](Memory& mem, std::uint32_t base, const char* label) {
        constexpr int kN = 4000000;
        volatile std::uint64_t sink = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kN; ++i) {
            const std::uint32_t a = base + static_cast<std::uint32_t>(i & 0xFFF) * 4;
            mem.write<std::uint32_t>(a, static_cast<std::uint32_t>(i), 0);
            sink = sink + mem.read<std::uint32_t>(a, 0);
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "[bench] %s: %.1f ns por acesso (SW+LW)\n", label, static_cast<double>(ns) / (2.0 * kN));
    };
    bench(o.mem, 0x1100C000u, "antes (RAM direta)");
    bench(n.mem, 0x1100C000u, "depois (dispositivo, gancho no-op)");
}

// O gancho de fence roda uma vez antes de cada acesso do EE à faixa (nos dois espelhos, em
// todos os tamanhos e nas cópias em bloco) e em nenhum acesso fora dela.
TEST_CASE(vu1_mem, fence_runs_before_every_access) {
    New n;
    std::uint64_t expect = 0;
    n.mem.write<std::uint8_t>(0x11008000, 1, 0);
    n.mem.write<std::uint16_t>(0x11008002, 1, 0);
    n.mem.write<std::uint32_t>(0xB1008004, 1, 0);
    n.mem.write<std::uint64_t>(0x11008008, 1, 0);
    Reg128 q{};
    n.mem.write128(0x11008010, q, 0);
    n.mem.read128(0xB100C010, q, 0);
    n.mem.read<std::uint64_t>(0x1100C000, 0);
    expect += 7;
    CHECK_EQ(n.fences, expect);
    std::uint8_t buf[32] = {};
    n.mem.copyToGuest(0x1100C040, buf, sizeof buf, 0);
    n.mem.copyFromGuest(buf, 0x1100C040, sizeof buf, 0);
    expect += 2;
    CHECK_EQ(n.fences, expect);
    n.mem.write<std::uint32_t>(0x00100000, 1, 0);  // RAM
    n.mem.read<std::uint32_t>(0x00100000, 0);
    CHECK_EQ(n.fences, expect);
    try {
        n.mem.read<std::uint32_t>(0x11010000, 0);  // logo depois da faixa
    } catch (const std::exception&) {
    }
    CHECK_EQ(n.fences, expect);
}
