// VIF separado em decodificação (EE) e execução (fatia 3 do plano do VU1 em outra thread): o Vif
// novo tem de ser bit a bit igual ao de antes da separação. O oráculo é uma CÓPIA CONGELADA do Vif
// antigo (vif_frozen.h), alimentada com os mesmos fluxos de VIFcodes e as mesmas quebras de DMA.
// Compara, a cada chamada: bytes consumidos, exceções, STAT/FQC/MARK/CYCLE/MODE/MASK/ITOPS/BASE/
// OFST/TOPS/ITOP/TOP, ROW e COL, INTC, as memórias do VU0 e do VU1 inteiras, os registradores dos
// VUs depois de cada MSCAL/MSCALF/MSCNT e o que o GIF entregou ao GS (registradores gerais e VRAM,
// pelos pacotes do DIRECT) e o estado do PATH3 mascarado.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/vif.h"
#include "anyps2/runtime/vu/vu.h"
#include "anyps2/runtime/vu1_stream.h"
#include "minitest.h"
#include "vif_frozen.h"

using namespace anyps2::rt;

namespace {

constexpr std::uint32_t kLNop = 0x8000033Cu, kUNop = 0x000002FFu, kEnd = 1u << 30;
constexpr std::uint32_t X = 8;

std::uint32_t lo(std::uint32_t op, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs, std::int32_t imm11) {
    return (op << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (static_cast<std::uint32_t>(imm11) & 0x7FF);
}
std::uint32_t lox(std::uint32_t idx, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs) {
    return (0x40u << 25) | (dest << 21) | (ft << 16) | (fs << 11) | ((idx >> 2) << 6) | 0x3C | (idx & 3);
}

using Pairs = std::vector<std::pair<std::uint32_t, std::uint32_t>>;

// Microprograma do VU1: guarda XTOP e XITOP na memória de dados e incrementa um contador que
// o UNPACK também pode sobrescrever (a execução depende do que o VIF gravou).
Pairs program1() {
    return {
        {lox(0x68, 0, 2, 0), kUNop},        // xtop vi2
        {lox(0x69, 0, 3, 0), kUNop},        // xitop vi3
        {lo(0x05, X, 2, 0, 1000), kUNop},   // isw.x vi2, 1000(vi0)
        {lo(0x05, X, 3, 0, 1001), kUNop},   // isw.x vi3, 1001(vi0)
        {lo(0x04, X, 5, 0, 1002), kUNop},   // ilw.x vi5, 1002(vi0)
        {lo(0x08, 0, 5, 5, 1), kUNop},      // iaddiu vi5, vi5, 1
        {lo(0x05, X, 5, 0, 1002), kUNop},   // isw.x vi5, 1002(vi0)
        {kLNop, kUNop | kEnd},
        {kLNop, kUNop},
    };
}
Pairs program0() {
    return {
        {lo(0x08, 0, 1, 0, 0x2A), kUNop},  // iaddiu vi1, vi0, 0x2A
        {lo(0x05, X, 1, 0, 250), kUNop},   // isw.x vi1, 250(vi0)
        {lo(0x04, X, 5, 0, 251), kUNop},   // ilw.x vi5, 251(vi0)
        {lo(0x08, 0, 5, 5, 1), kUNop},     // iaddiu vi5, vi5, 1
        {lo(0x05, X, 5, 0, 251), kUNop},   // isw.x vi5, 251(vi0)
        {kLNop, kUNop | kEnd},
        {kLNop, kUNop},
    };
}

// Posições (em bytes) do microprograma e da área onde o MPG pode gravar lixo sem atingi-lo.
constexpr std::uint32_t kProg1 = 0x1000, kProg0 = 0x400;
constexpr std::uint32_t kJunk1Lo = 0x2000, kJunk1Hi = 0x3800, kJunk0Lo = 0x800, kJunk0Hi = 0xF00;

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    std::uint32_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return static_cast<std::uint32_t>((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    std::uint32_t below(std::uint32_t n) { return n == 0 ? 0 : next() % n; }
    bool chance(std::uint32_t percent) { return below(100) < percent; }
};

// Um runtime (virtual, com os VUs preparados) com a mesma memória de microprogramas nos dois lados.
struct Side {
    ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    RuntimeOptions options = [] {
        RuntimeOptions o;
        o.virtualClock = true;
        return o;
    }();
    Runtime rt{info, options};

    Side() {
        auto fill = [](std::uint8_t* m, std::uint32_t size, const Pairs& prog, std::uint32_t at) {
            // Todo o resto da micro memória é "fim de programa": MSCNT que cai em qualquer lugar termina.
            for (std::uint32_t o = 0; o + 8 <= size; o += 8) {
                const std::uint32_t pair[2] = {kLNop, kUNop | kEnd};
                std::memcpy(m + o, pair, 8);
            }
            for (const std::uint32_t base : {0u, at}) {
                for (std::size_t i = 0; i < prog.size(); ++i) {
                    std::memcpy(m + base + i * 8, &prog[i].first, 4);
                    std::memcpy(m + base + i * 8 + 4, &prog[i].second, 4);
                }
            }
        };
        fill(rt.vu().micro1.get(), VuMemory::kVu1Size, program1(), kProg1);
        fill(rt.vu().micro0.get(), VuMemory::kVu0Size, program0(), kProg0);
    }
};

// Pacote do GIF montado pelo gerador (lista de quadwords).
using Qw = std::pair<std::uint64_t, std::uint64_t>;

struct GifGen {
    Rng& r;
    std::vector<Qw> queue;  // quadwords de PATH2 ainda não entregues a algum DIRECT
    static constexpr std::uint8_t kRegs[] = {0x01, 0x03, 0x18, 0x08, 0x3B, 0x3D, 0x40, 0x42, 0x46, 0x47, 0x49, 0x4A, 0x45};
    void packet() {
        if (r.chance(55)) {
            const std::uint32_t n = 1 + r.below(12);  // A+D
            queue.push_back({n | (1ull << 15) | (1ull << 60), 0xE});
            for (std::uint32_t i = 0; i < n; ++i) {
                const std::uint64_t v = (static_cast<std::uint64_t>(r.next()) << 32) | r.next();
                queue.push_back({v, kRegs[r.below(static_cast<std::uint32_t>(sizeof(kRegs)))]});
            }
        } else {
            const std::uint32_t n = 1 + r.below(30);  // IMAGE
            queue.push_back({n | (1ull << 15) | (2ull << 58), 0});
            for (std::uint32_t i = 0; i < n; ++i) {
                queue.push_back({(static_cast<std::uint64_t>(r.next()) << 32) | r.next(),
                                 (static_cast<std::uint64_t>(r.next()) << 32) | r.next()});
            }
        }
    }
};

// Um caso: o fluxo de palavras de VIFcode e o que o oráculo precisa saber.
struct Stream {
    std::vector<std::uint32_t> words;
    bool bad = false;  // termina em VIFcode inválido de propósito
};

Stream makeStream(Rng& r, unsigned unit) {
    Stream s;
    GifGen gif{r, {}};
    std::uint32_t cycle = 0;  // CYCLE que o decodificador terá
    const bool vif1 = unit == 1;
    const unsigned count = 8 + r.below(48);
    auto put = [&](std::uint32_t w) { s.words.push_back(w); };
    auto rnd = [&] { return r.next(); };
    auto code = [&](std::uint32_t cmd, std::uint32_t num, std::uint32_t imm) {
        std::uint32_t c = cmd;
        if (r.chance(4)) c |= 0x80;  // bit I
        put((c << 24) | (num << 16) | imm);
    };
    for (unsigned n = 0; n < count; ++n) {
        switch (r.below(22)) {
            case 0: code(0x00, 0, 0); break;
            case 1: case 2: {
                // STCYCL: pulo (CL > WL), igual, preenchimento (CL < WL) e zeros
                const std::uint32_t cl = r.chance(15) ? 0 : r.below(9);
                const std::uint32_t wl = r.chance(15) ? 0 : r.below(9);
                cycle = cl | (wl << 8);
                code(0x01, 0, cycle);
                break;
            }
            case 3:
                if (vif1) code(0x02, 0, r.below(0x400)); else code(0x04, 0, r.below(0x400));
                break;
            case 4:
                if (vif1) code(0x03, 0, r.below(0x400)); else code(0x05, 0, r.below(4));
                break;
            case 5: code(0x04, 0, r.below(0x400)); break;
            case 6: code(0x05, 0, r.below(4)); break;  // STMOD 0..3 (3 = como 0 no VIF)
            case 7:
                if (vif1) code(0x06, 0, r.chance(50) ? 0x8000 : 0); else code(0x07, 0, r.below(0x10000));
                break;
            case 8: code(0x07, 0, r.below(0x10000)); break;
            case 9:
                code(vif1 ? (r.chance(50) ? 0x11 : 0x13) : 0x10, 0, 0);  // FLUSH/FLUSHA/FLUSHE
                break;
            case 10: code(0x20, 0, 0); put(rnd()); break;  // STMASK
            case 11: code(0x30, 0, 0); for (int i = 0; i < 4; ++i) put(r.chance(50) ? rnd() : r.below(8)); break;
            case 12: code(0x31, 0, 0); for (int i = 0; i < 4; ++i) put(r.chance(50) ? rnd() : r.below(8)); break;
            case 13: case 14: {  // MSCAL / MSCALF / MSCNT
                const std::uint32_t cmd = r.below(3) == 0 ? 0x17 : (r.chance(50) ? 0x14 : 0x15);
                code(cmd, 0, (vif1 ? kProg1 : kProg0) / 8);
                break;
            }
            case 15: {  // MPG: reenvia o programa no lugar (inofensivo) ou lixo em área livre
                const bool junk = r.chance(60);
                if (junk) {
                    const std::uint32_t junkLo = unit ? kJunk1Lo : kJunk0Lo, junkHi = unit ? kJunk1Hi : kJunk0Hi;
                    const std::uint32_t words = 2 * (1 + r.below(24));
                    const std::uint32_t addr = junkLo + r.below((junkHi - junkLo - words * 4) / 8) * 8;
                    code(0x4A, words / 2, addr / 8);
                    for (std::uint32_t i = 0; i < words; ++i) put(rnd());
                } else {
                    const Pairs prog = vif1 ? program1() : program0();
                    code(0x4A, static_cast<std::uint32_t>(prog.size()), (vif1 ? kProg1 : kProg0) / 8);
                    for (const auto& p : prog) {
                        put(p.first);
                        put(p.second);
                    }
                }
                break;
            }
            case 16:
                if (!vif1) break;
                {  // DIRECT / DIRECTHL com um pedaço do fluxo do GIF
                    if (gif.queue.empty() || r.chance(30)) gif.packet();
                    const std::uint32_t k = std::min<std::uint32_t>(
                        static_cast<std::uint32_t>(gif.queue.size()), 1 + r.below(10));
                    code(r.chance(70) ? 0x50 : 0x51, 0, k);
                    for (std::uint32_t i = 0; i < k; ++i) {
                        const Qw q = gif.queue[i];
                        put(static_cast<std::uint32_t>(q.first));
                        put(static_cast<std::uint32_t>(q.first >> 32));
                        put(static_cast<std::uint32_t>(q.second));
                        put(static_cast<std::uint32_t>(q.second >> 32));
                    }
                    gif.queue.erase(gif.queue.begin(), gif.queue.begin() + k);
                }
                break;
            default: {  // UNPACK (a maioria dos casos)
                static constexpr unsigned kFormats[][2] = {{0, 0}, {0, 1}, {0, 2}, {1, 0}, {1, 1}, {1, 2}, {2, 0},
                                                           {2, 1}, {2, 2}, {3, 0}, {3, 1}, {3, 2}, {3, 3}};
                const unsigned* f = kFormats[r.below(13)];
                const unsigned vn = f[0], vl = f[1];
                std::uint32_t num = r.chance(4) ? 0 : 1 + r.below(r.chance(15) ? 40 : 12);
                std::uint32_t imm = r.below(0x400);
                if (r.chance(40)) imm |= 0x8000;  // FLG
                if (r.chance(40)) imm |= 0x4000;  // USN
                const std::uint32_t cmd = 0x60 | ((r.chance(35) ? 1u : 0u) << 4) | (vn << 2) | vl;
                code(cmd, num, imm);
                const std::uint32_t total = num == 0 ? 256 : num;
                const std::uint32_t cl = cycle & 0xFF, wl = (cycle >> 8) & 0xFF;
                const std::uint32_t readLeft =
                    wl > cl ? cl * (total / wl) + std::min<std::uint32_t>(total % wl, cl) : total;
                const std::uint32_t vbits = (vn == 3 && vl == 3) ? 16 : (vn + 1) * (32u >> vl);
                const std::uint32_t words = (readLeft * vbits + 31) / 32;
                for (std::uint32_t i = 0; i < words; ++i) put(r.chance(20) ? r.below(16) : rnd());
                break;
            }
        }
        if (r.chance(5) && !s.words.empty()) {
            // bit I já sai de `code`; aqui só espalha NOPs para variar o alinhamento no quadword
            for (unsigned i = r.below(4); i > 0; --i) put(0);
        }
    }
    // Esvazia o que sobrou do GIF para o pacote terminar.
    while (!gif.queue.empty() && vif1) {
        const std::uint32_t k = std::min<std::uint32_t>(static_cast<std::uint32_t>(gif.queue.size()), 8);
        code(0x50, 0, k);
        for (std::uint32_t i = 0; i < k; ++i) {
            const Qw q = gif.queue[i];
            put(static_cast<std::uint32_t>(q.first));
            put(static_cast<std::uint32_t>(q.first >> 32));
            put(static_cast<std::uint32_t>(q.second));
            put(static_cast<std::uint32_t>(q.second >> 32));
        }
        gif.queue.erase(gif.queue.begin(), gif.queue.begin() + k);
    }
    if (r.chance(3)) {
        s.bad = true;
        // VIFcode inexistente, ou UNPACK com vl=3 fora do V4-5, ou comando só do VIF1 no VIF0
        switch (r.below(3)) {
            case 0: put(0x09000000u | r.below(0x10000)); break;
            case 1: put(0x6C000000u | (0u << 2) | 3u); break;  // vn=0, vl=3
            default: put(vif1 ? 0x08000000u : 0x50000001u); break;
        }
    }
    return s;
}

// O que os dois lados têm de ter igual depois de cada chamada.
template <typename Old>
struct Pair {
    Side a, b;  // a: Vif novo (rt.vif0/vif1); b: Vif congelado
    Old old0, old1;
    Pair() : old0(&b.rt, 0, b.rt.vu(), &b.rt.gif()), old1(&b.rt, 1, b.rt.vu(), &b.rt.gif()) {
        // Com o VU1 em outra thread (ANYPS2_VU1_THREAD=1) o erro do consumidor sobe no próximo
        // fence ou enfileiramento; com o fence forçado a cada item ele sobe no mesmo item que o
        // lado síncrono, que é o que este diferencial compara. A thread sem o fence forçado (e os
        // erros que sobem depois) tem o seu diferencial em test_vu1_thread.cpp.
        a.rt.vu1Stream().setForceFence(true);
    }
    Vif& nv(unsigned u) { return u == 0 ? a.rt.vif0() : a.rt.vif1(); }
    Old& ov(unsigned u) { return u == 0 ? old0 : old1; }
};

template <typename T>
std::string hexs(T v) {
    return std::to_string(static_cast<unsigned long long>(v));
}

// Compara tudo; devolve a descrição da primeira diferença ("" se igual).
template <typename P>
std::string diff(P& p, unsigned unit, bool deep) {
    static constexpr std::uint32_t regs[] = {0x00, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0,
                                             0xD0, 0xE0, 0x100, 0x110, 0x120, 0x130, 0x140, 0x150, 0x160, 0x170};
    for (unsigned u = 0; u < 2; ++u) {
        for (const std::uint32_t off : regs) {
            const std::uint32_t x = p.nv(u).readRegister(off, 0), y = p.ov(u).readRegister(off, 0);
            if (x != y) return "VIF" + hexs(u) + " reg " + hexs(off) + ": " + hexs(x) + " != " + hexs(y);
        }
        if (p.nv(u).stalled() != p.ov(u).stalled()) return "stalled";
        if (p.nv(u).idle() != p.ov(u).idle()) return "idle";
    }
    if (p.a.rt.kernel().intcStat() != p.b.rt.kernel().intcStat()) return "INTC";
    if (p.a.rt.gif().path3Masked() != p.b.rt.gif().path3Masked()) return "PATH3 mascarado";
    const VuMemory& x = p.a.rt.vu();
    const VuMemory& y = p.b.rt.vu();
    if (std::memcmp(x.data1.get(), y.data1.get(), VuMemory::kVu1Size) != 0) {
        std::uint32_t o = 0;
        while (x.data1[o] == y.data1[o]) ++o;
        return "memória de dados do VU1 em " + hexs(o) + ": " + hexs(x.data1[o]) + " != " + hexs(y.data1[o]);
    }
    if (std::memcmp(x.micro1.get(), y.micro1.get(), VuMemory::kVu1Size) != 0) return "micro memória do VU1";
    if (std::memcmp(x.data0.get(), y.data0.get(), VuMemory::kVu0Size) != 0) return "memória de dados do VU0";
    if (std::memcmp(x.micro0.get(), y.micro0.get(), VuMemory::kVu0Size) != 0) {
        std::uint32_t o = 0;
        while (x.micro0[o] == y.micro0[o]) ++o;
        return "micro memória do VU0 em " + hexs(o) + ": " + hexs(x.micro0[o]) + " != " + hexs(y.micro0[o]);
    }
    for (unsigned v = 0; v < 2; ++v) {
        Vu& va = v == 0 ? p.a.rt.vu0() : p.a.rt.vu1();
        Vu& vb = v == 0 ? p.b.rt.vu0() : p.b.rt.vu1();
        if (va.compiledRuns() + va.interpretedRuns() != vb.compiledRuns() + vb.interpretedRuns()) return "execuções do VU";
        if (std::memcmp(va.regs().vf, vb.regs().vf, 32 * sizeof(Reg128)) != 0) return "VF do VU" + hexs(v);
        if (std::memcmp(va.regs().vi, vb.regs().vi, 32 * sizeof(std::uint32_t)) != 0) return "VI do VU" + hexs(v);
        if (std::memcmp(va.regs().acc, vb.regs().acc, sizeof(Reg128)) != 0) return "ACC do VU" + hexs(v);
    }
    if (deep) {
        const gs::Gs& ga = p.a.rt.gs();
        const gs::Gs& gb = p.b.rt.gs();
        for (unsigned r = 0; r < 0x63; ++r) {
            if (ga.reg(static_cast<std::uint8_t>(r)) != gb.reg(static_cast<std::uint8_t>(r))) return "registrador do GS " + hexs(r);
        }
        if (std::memcmp(ga.vram().data(), gb.vram().data(), 64 * 1024) != 0) return "VRAM do GS";
    }
    (void)unit;
    return "";
}

// Prepara o GS dos dois lados para os IMAGE do DIRECT: BITBLTBUF (DBW=1, PSMCT32, DBP=0), TRXPOS=0,
// TRXREG 64x64 e TRXDIR=0, por PATH2 direto no GIF.
void armTransfer(Runtime& rt) {
    rt.gif().reset();  // um caso que parou no meio de um pacote não contamina o seguinte
    const std::uint64_t qw[5][2] = {{4ull | (1ull << 15) | (1ull << 60), 0xE},
                                    {1ull << 48, 0x50},
                                    {0, 0x51},
                                    {64ull | (64ull << 32), 0x52},
                                    {0, 0x53}};
    rt.gif().transfer(2, reinterpret_cast<const std::uint8_t*>(qw), 5, 0);
}

struct Outcome {
    std::string err;
    std::size_t ret = 0;
};

template <typename V, typename F>
Outcome call(V& v, F&& f) {
    Outcome o;
    try {
        o.ret = f(v);
    } catch (const std::exception& e) {
        o.err = e.what();
    }
    return o;
}

// Cobertura do gerador: o teste confere que os caminhos importantes foram exercitados.
struct Tally {
    std::uint64_t calls = 0, stalls = 0, throws = 0, resumes = 0, midQw = 0, early = 0, fifoWrites = 0, tags = 0;
};

// Roda um caso nos dois lados, chamada por chamada. Devolve a descrição da primeira diferença.
template <typename T>
std::string runCase(Pair<T>& p, Rng& r, unsigned unit, const Stream& st, std::uint32_t pc, Tally& tally) {
    std::vector<std::uint8_t> bytes(st.words.size() * 4);
    std::memcpy(bytes.data(), st.words.data(), bytes.size());
    bool threw = false;
    // Aplica f aos dois lados e compara retorno, exceção e todo o estado.
    auto step = [&](auto&& f) -> std::string {
        // Com ANYPS2_VU1_THREAD=1 o Vif novo entrega ao consumidor em outra thread: o fence logo
        // depois da chamada (na mesma tentativa) traz o erro dele ao mesmo ponto do lado congelado,
        // e deixa o consumidor ocioso para as leituras do diff.
        auto fa = [&](auto& vif) {
            std::exception_ptr ep;
            std::size_t ret = 0;
            try {
                ret = f(vif);
            } catch (...) {
                ep = std::current_exception();
            }
            try {
                p.a.rt.vu1Stream().fence();
            } catch (...) {
                ep = std::current_exception();  // o erro do consumidor veio antes, no fluxo
            }
            if (ep) std::rethrow_exception(ep);
            return ret;
        };
        const Outcome oa = call(p.nv(unit), fa), ob = call(p.ov(unit), f);
        ++tally.calls;
        if (oa.err != ob.err) return "exceção: [" + oa.err + "] != [" + ob.err + "]";
        if (oa.ret != ob.ret) return "retorno " + hexs(oa.ret) + " != " + hexs(ob.ret);
        if (!oa.err.empty()) {
            threw = true;
            ++tally.throws;
        }
        const std::string dd = diff(p, unit, (tally.calls % 8) == 0 || threw);
        if (!dd.empty()) return dd + " [erro=" + oa.err + "; ret=" + hexs(oa.ret) + "; chamada=" + hexs(tally.calls) + "]";
        return dd;
    };
    auto reg = [&](std::uint32_t off, std::uint32_t v) {
        return step([&](auto& vif) {
            vif.writeRegister(off, v, pc);
            return std::size_t{0};
        });
    };

    // Começa de um VIF limpo nos dois lados (FBRST.RST), com ERR.MII sorteado.
    if (std::string d = reg(0x10, 1); !d.empty()) return d;
    armTransfer(p.a.rt);
    armTransfer(p.b.rt);
    if (std::string d = reg(0x20, r.chance(15) ? 1 : 0); !d.empty()) return d;

    std::size_t pos = 0;
    for (unsigned guard = 0; pos < bytes.size() && !threw && guard < 200000; ++guard) {
        std::string d;
        const bool stalled = p.nv(unit).stalled();
        if (stalled) ++tally.stalls;
        if (stalled && r.chance(85)) {
            ++tally.resumes;
            d = reg(0x10, 8);  // FBRST.STC
        } else if (r.chance(3)) {
            d = reg(0x10, 8);  // STC sem estar parado (só limpa INT/ER)
        } else if (r.chance(2)) {
            d = reg(0x30, r.below(0x10000));  // escrita do MARK pelo EE
        } else if (r.chance(1)) {
            d = reg(0x20, r.below(8));
        } else {
            const std::size_t remain = bytes.size() - pos;
            const unsigned mode = r.below(10);
            if (mode == 0 && pos % 16 == 0 && remain >= 16) {
                const std::size_t n =
                    16 * (1 + r.below(static_cast<std::uint32_t>(std::min<std::size_t>(remain / 16, 4))));
                ++tally.fifoWrites;
                d = step([&](auto& vif) {
                    vif.fifoWrite(bytes.data() + pos, n, pc);
                    return std::size_t{0};
                });
                pos += n;
            } else if (mode == 1 && remain >= 8 && !stalled) {
                std::uint64_t upper;
                std::memcpy(&upper, bytes.data() + pos, 8);
                ++tally.tags;
                d = step([&](auto& vif) {
                    vif.transferTag(upper, pc);
                    return std::size_t{0};
                });
                pos += 8;
            } else {
                std::size_t n = r.chance(50) ? 4 * (1 + r.below(40)) : 16 * (1 + r.below(20));
                n = std::min(n, remain);
                n &= ~std::size_t{3};
                if (n == 0) n = 4;
                std::size_t got = 0;
                d = step([&](auto& vif) { return got = vif.transfer(bytes.data() + pos, n, pc); });
                if (n % 16 != 0) ++tally.midQw;      // quebra do DMA no meio de um quadword
                if (got < n) ++tally.early;          // o VIF parou antes do fim do pedaço
                pos += got;
            }
        }
        if (!d.empty()) return d;
    }
    // Esvazia as paradas que sobraram.
    for (unsigned i = 0; i < 1000 && !threw && p.nv(unit).stalled(); ++i) {
        if (std::string d = reg(0x10, 8); !d.empty()) return d;
    }
    return diff(p, unit, true);
}

}  // namespace

TEST_CASE(vif_split, differential_against_frozen_vif) {
    Tally tally;
    std::uint64_t runs = 0, gsWrites = 0;
    constexpr unsigned kPairs = 40, kCasesPerPair = 300;  // 12000 casos
    for (unsigned pi = 0; pi < kPairs; ++pi) {
        Pair<frozen::FrozenVif> p;
        for (unsigned ci = 0; ci < kCasesPerPair; ++ci) {
            const unsigned id = pi * kCasesPerPair + ci;
            Rng r(1000 + id);
            const unsigned unit = r.chance(70) ? 1 : 0;
            const Stream st = makeStream(r, unit);
            const std::string d = runCase(p, r, unit, st, 0x00100000u + id, tally);
            if (!d.empty()) {
                minitest::reportFailure(__FILE__, __LINE__,
                                        "caso " + std::to_string(id) + " (VIF" + std::to_string(unit) + "): " + d);
                return;
            }
        }
        runs += p.a.rt.vu0().interpretedRuns() + p.a.rt.vu0().compiledRuns() + p.a.rt.vu1().interpretedRuns() +
                p.a.rt.vu1().compiledRuns();
        gsWrites += p.a.rt.gs().reg(0x01) != 0;
    }
    // O gerador exercita de verdade o que se quer comparar.
    std::fprintf(stderr, "[vif_split] chamadas=%llu execuções=%llu paradas=%llu retomadas=%llu paradas-no-pedaço=%llu meio-de-QW=%llu fifo=%llu tags=%llu exceções=%llu\n",
                 static_cast<unsigned long long>(tally.calls), static_cast<unsigned long long>(runs),
                 static_cast<unsigned long long>(tally.stalls), static_cast<unsigned long long>(tally.resumes),
                 static_cast<unsigned long long>(tally.early), static_cast<unsigned long long>(tally.midQw),
                 static_cast<unsigned long long>(tally.fifoWrites), static_cast<unsigned long long>(tally.tags),
                 static_cast<unsigned long long>(tally.throws));
    CHECK(tally.calls > 50000);
    CHECK(runs > 3000);
    CHECK(tally.early > 100);
    CHECK(tally.stalls > 300);
    CHECK(tally.resumes > 200);
    CHECK(tally.midQw > 300);
    CHECK(tally.fifoWrites > 300);
    CHECK(tally.tags > 300);
    CHECK(tally.throws > 50);
    CHECK(gsWrites > 0);
}

// O executor sozinho: cada item faz o que o Vif de antes fazia no mesmo ponto.
TEST_CASE(vif_split, executor_items) {
    Side s;
    VuMemory& vu = s.rt.vu();
    VifExec ex(&s.rt, 1, vu, &s.rt.gif());
    // Unpack: só os componentes liberados; o quadword de destino já vem reduzido.
    const VifQwWrite w[2] = {{5, 0x5, {1, 2, 3, 4}}, {1023, 0xF, {9, 8, 7, 6}}};
    VifItem un(VifItem::Kind::Unpack);
    un.writes = w;
    un.count = 2;
    std::uint32_t before[4];
    std::memcpy(before, vu.data1.get() + 5 * 16, 16);
    ex.apply(un);
    std::uint32_t got[4];
    std::memcpy(got, vu.data1.get() + 5 * 16, 16);
    CHECK_EQ(got[0], 1u);
    CHECK_EQ(got[1], before[1]);
    CHECK_EQ(got[2], 3u);
    CHECK_EQ(got[3], before[3]);
    std::memcpy(got, vu.data1.get() + 1023 * 16, 16);
    CHECK_EQ(got[3], 6u);
    // Mpg: dá a volta na micro memória como a palavra a palavra de antes.
    const std::uint32_t words[3] = {0x11111111u, 0x22222222u, 0x33333333u};
    VifItem mpg(VifItem::Kind::Mpg);
    mpg.words = words;
    mpg.count = 3;
    mpg.addr = VuMemory::kVu1Size - 8;
    ex.apply(mpg);
    std::uint32_t m;
    std::memcpy(&m, vu.micro1.get() + VuMemory::kVu1Size - 8, 4);
    CHECK_EQ(m, 0x11111111u);
    std::memcpy(&m, vu.micro1.get(), 4);
    CHECK_EQ(m, 0x33333333u);
    // Path3Mask e Reset
    VifItem mask(VifItem::Kind::Path3Mask);
    mask.masked = true;
    ex.apply(mask);
    CHECK(s.rt.gif().path3Masked());
    ex.apply(VifItem(VifItem::Kind::Reset));
    CHECK(!s.rt.gif().path3Masked());
    // Mscal: o VU1 vê o TOP/ITOP do item (XTOP/XITOP), não o estado do Vif.
    const Pairs prog = program1();
    for (std::size_t i = 0; i < prog.size(); ++i) {
        std::memcpy(vu.micro1.get() + kProg1 + i * 8, &prog[i].first, 4);
        std::memcpy(vu.micro1.get() + kProg1 + i * 8 + 4, &prog[i].second, 4);
    }
    VifItem ms(VifItem::Kind::Mscal);
    ms.start = kProg1;
    ms.top = 0x123;
    ms.itop = 0x2AB;
    ex.apply(ms);
    std::uint32_t top, itop;
    std::memcpy(&top, vu.data1.get() + 1000 * 16, 4);
    std::memcpy(&itop, vu.data1.get() + 1001 * 16, 4);
    CHECK_EQ(top, 0x123u);
    CHECK_EQ(itop, 0x2ABu);
    CHECK_EQ(s.rt.vif1().top(), 0u);  // o Vif não participou
}
