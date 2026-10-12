// Gera os microprogramas aleatórios do teste diferencial do VU:
//   vu_fuzz_data.h  — imagem da micro memória (16 KB) e a lista de programas
//   vu_fuzz_0..3.cpp — o C++ recompilado (generateVuSources), em 4 grupos
//
// Os pares vêm do golden do dvp-objdump (tests/data/vu_golden.txt): uppers e
// lowers canônicos, misturados ao acaso, mais desvios sintetizados (para a
// frente, laços contados com IBNE e JR com alvo absoluto) e pares LOI. Tudo
// determinístico (xorshift próprio, sem <random>).
//
// uso: vu_fuzz_gen <vu_golden.txt> <diretório de saída>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "anyps2/codegen/vu_generator.h"
#include "anyps2/vu/isa.h"

namespace {

using anyps2::vu::L;
using anyps2::vu::U;

constexpr std::uint32_t kLNop = 0x8000033Cu, kUNop = 0x000002FFu;
constexpr std::uint32_t kIBit = 1u << 31, kEBit = 1u << 30;
constexpr std::uint32_t kMicro = 0x4000;
constexpr unsigned kLoopReg = 14, kJumpReg = 15;  // reservados para os padrões de controle

struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    std::uint32_t below(std::uint32_t n) { return static_cast<std::uint32_t>(next() % n); }
};

std::uint32_t lo(std::uint32_t op, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs, std::uint32_t imm11) {
    return (op << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (imm11 & 0x7FF);
}
std::uint32_t iaddiu(unsigned it, unsigned is, std::uint32_t imm15) {
    return (0x08u << 25) | (((imm15 >> 11) & 0xF) << 21) | (it << 16) | (is << 11) | (imm15 & 0x7FF);
}
std::uint32_t isubiu(unsigned it, unsigned is, std::uint32_t imm15) {
    return (0x09u << 25) | (((imm15 >> 11) & 0xF) << 21) | (it << 16) | (is << 11) | (imm15 & 0x7FF);
}

// Upper FMAC: func (6 bits) com dest/ft/fs/fd; os "especiais" (0x3C..0x3F)
// levam o grupo no campo fd e o sub-código nos 2 bits baixos.
std::uint32_t up(std::uint32_t func, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs, std::uint32_t fd) {
    return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | func;
}
std::uint32_t upSpecial(std::uint32_t group, std::uint32_t sub, std::uint32_t dest, std::uint32_t ft,
                        std::uint32_t fs) {
    return up(0x3C + sub, dest, ft, fs, group);
}
// Lower especial (0x40 << 25): grupo no campo id (bits 10:6) e sub-código nos 2 bits baixos.
std::uint32_t loSpecial(std::uint32_t group, std::uint32_t sub, std::uint32_t dest, std::uint32_t ft,
                        std::uint32_t fs) {
    return (0x40u << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (group << 6) | (0x3C + sub);
}

bool touchesReserved(const anyps2::vu::Lower& l) {
    for (unsigned r : {unsigned{l.ft}, unsigned{l.fs}, unsigned{l.fd}}) {
        if (r == kLoopReg || r == kJumpReg) return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "uso: vu_fuzz_gen <vu_golden.txt> <saída>\n";
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in) {
        std::cerr << "não foi possível ler " << argv[1] << "\n";
        return 1;
    }
    std::vector<std::uint32_t> uppers, lowers;
    std::set<std::uint32_t> seenU, seenL;
    std::set<int> lowerOps;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string addr, los, ups;
        ls >> addr >> los >> ups;
        const auto lw = static_cast<std::uint32_t>(std::stoul(los, nullptr, 16));
        const auto uw = static_cast<std::uint32_t>(std::stoul(ups, nullptr, 16)) & 0x07FFFFFFu;  // sem I/E/M/D/T
        const auto du = anyps2::vu::decode(kLNop, uw);
        if (du.upper.op != U::INVALID && du.upper.canonical && seenU.insert(uw).second) uppers.push_back(uw);
        const auto dl = anyps2::vu::decode(lw, kUNop);
        const L op = dl.lower.op;
        if (op == L::INVALID || !dl.lower.canonical || anyps2::vu::isBranch(op) || op == L::XGKICK ||
            op == L::XTOP || op == L::XITOP || touchesReserved(dl.lower)) {
            continue;
        }
        if (seenL.insert(lw).second) {
            lowers.push_back(lw);
            lowerOps.insert(static_cast<int>(op));
        }
    }
    if (uppers.size() < 100 || lowers.size() < 100) {
        std::cerr << "golden com poucas instruções utilizáveis\n";
        return 1;
    }
    Rng rng{0x5900A11CEull};
    const float nice[] = {1.0f, 0.5f, -2.0f, 3.25f, 100.0f, 1e-3f, 1e30f, -1e-30f, 0.0f, -7.75f};

    struct Prog {
        std::uint32_t offset, pairs;
        bool hasJr;
        bool longLoop;  // laço longo de transformação de vértices (--bench-long)
    };
    std::vector<Prog> progs;
    std::vector<std::uint32_t> image(kMicro / 4, 0);
    std::vector<anyps2::vu::MicroBlob> blobs;
    std::uint32_t at = 0;
    // 16 programas (~800 pares): cobertura suficiente sem tornar caro compilar
    // o C++ gerado nos builds com sanitizers.
    constexpr std::size_t kPrograms = 16;
    while (progs.size() < kPrograms) {
        const std::uint32_t body = 40 + rng.below(17);
        if (at + (body + 2) * 8 > kMicro) break;
        std::vector<std::uint32_t> w;  // pares (lower, upper)
        bool hasJr = false;
        auto normal = [&] {
            w.push_back(lowers[rng.below(static_cast<std::uint32_t>(lowers.size()))]);
            w.push_back(uppers[rng.below(static_cast<std::uint32_t>(uppers.size()))]);
        };
        auto withUpper = [&](std::uint32_t lower) {
            w.push_back(lower);
            w.push_back(uppers[rng.below(static_cast<std::uint32_t>(uppers.size()))]);
        };
        auto index = [&] { return static_cast<std::uint32_t>(w.size() / 2); };
        while (index() < body) {
            const std::uint32_t i = index();
            const std::uint32_t r = rng.below(100);
            if (r < 6 && i + 2 < body) {
                // Desvio para a frente; o par seguinte (delay slot) é normal.
                const std::uint32_t target = i + 2 + rng.below(std::min<std::uint32_t>(5, body - i - 1));
                const std::uint32_t imm = target - i - 1;
                const unsigned a = 1 + rng.below(13), b = rng.below(14);
                static const std::uint32_t kOps[] = {0x20, 0x21, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F};
                const std::uint32_t op = kOps[rng.below(8)];
                std::uint32_t word = 0;
                if (op == 0x20) word = lo(op, 0, 0, 0, imm);
                else if (op == 0x21) word = lo(op, 0, a, 0, imm);           // BAL viA
                else if (op == 0x28 || op == 0x29) word = lo(op, 0, b, a, imm);  // IBEQ/IBNE
                else word = lo(op, 0, 0, a, imm);                           // IBLTZ...
                withUpper(word);
                normal();
            } else if (r < 8 && i + 3 < body) {
                // JR com alvo absoluto (o programa sempre roda na posição da imagem).
                hasJr = true;
                const std::uint32_t target = i + 3 + rng.below(std::min<std::uint32_t>(4, body - i - 2));
                withUpper(iaddiu(kJumpReg, 0, (at + target * 8) / 8));
                withUpper(lo(0x24, 0, 0, kJumpReg, 0));
                normal();
            } else if (r < 10 && i + 8 < body) {
                // Laço contado: vi14 = n; corpo; vi14 -= 1; ibne vi14, vi0, corpo.
                const std::uint32_t n = 2 + rng.below(3), k = 1 + rng.below(3);
                withUpper(iaddiu(kLoopReg, 0, n));
                const std::uint32_t start = index();
                for (std::uint32_t j = 0; j < k; ++j) normal();
                withUpper(isubiu(kLoopReg, kLoopReg, 1));
                const std::uint32_t back = static_cast<std::uint32_t>(static_cast<int>(start) - static_cast<int>(index()) - 1);
                withUpper(lo(0x29, 0, 0, kLoopReg, back));
                normal();
            } else if (r < 18) {
                // LOI: lower é um float.
                float f = nice[rng.below(10)];
                std::uint32_t bits;
                std::memcpy(&bits, &f, 4);
                if (rng.below(3) == 0) bits = static_cast<std::uint32_t>(rng.next());
                w.push_back(bits);
                w.push_back(uppers[rng.below(static_cast<std::uint32_t>(uppers.size()))] | kIBit);
            } else {
                normal();
            }
        }
        // Fim: par com bit E e o delay slot.
        normal();
        w.back() |= kEBit;
        normal();
        const auto pairs = static_cast<std::uint32_t>(w.size() / 2);
        for (std::size_t k = 0; k < w.size(); ++k) image[at / 4 + k] = w[k];
        anyps2::vu::MicroBlob b;
        b.address = at;
        b.loadAddress = at;
        b.words = w;
        blobs.push_back(b);
        progs.push_back({at, pairs, hasJr, false});
        at += pairs * 8;
    }

    // Laço longo típico de transformação de vértices (milhares de pares por
    // start): matriz em vf1..vf4, vértice por LQI, MULA/MADDA/MADD com
    // broadcast, CLIP, DIV com WAITQ, MULq, FTOI4, SQI, XGKICK por vértice e
    // IBNE de volta. Serve para separar o custo por start do custo por par e
    // entra na comparação com o interpretador como os demais programas.
    {
        constexpr std::uint32_t kVertices = 300;
        constexpr std::uint32_t kXyzw = 0xF, kXyz = 0xE;
        enum { X, Y, Z, W };
        std::vector<std::uint32_t> w;
        std::vector<L> wantL;
        std::vector<U> wantU;
        auto pair = [&](std::uint32_t lower, L lop, std::uint32_t upper, U uop) {
            w.push_back(lower);
            w.push_back(upper);
            wantL.push_back(lop);
            wantU.push_back(uop);
        };
        auto lower = [&](std::uint32_t word, L lop) { pair(word, lop, kUNop, U::NOP); };
        auto upper = [&](std::uint32_t word, U uop) { pair(kLNop, L::NOP, word, uop); };
        for (unsigned c = 0; c < 4; ++c) lower(lo(0x00, kXyzw, 1 + c, 0, c), L::LQ);  // lq vf1..vf4, c(vi0)
        lower(iaddiu(1, 0, 8), L::IADDIU);                                              // vi1: vértices
        lower(iaddiu(2, 0, 512), L::IADDIU);                                            // vi2: saída
        lower(iaddiu(3, 0, kVertices), L::IADDIU);                                      // vi3: contador
        const std::uint32_t loop = static_cast<std::uint32_t>(w.size() / 2);
        lower(loSpecial(0xD, 0, kXyzw, 5, 1), L::LQI);                                  // lqi vf5, (vi1++)
        lower(kLNop, L::NOP);
        upper(upSpecial(6, X, kXyzw, 5, 1), U::MULAbc);                                 // mulax ACC, vf1, vf5x
        upper(upSpecial(2, Y, kXyzw, 5, 2), U::MADDAbc);                                // madday ACC, vf2, vf5y
        upper(upSpecial(2, Z, kXyzw, 5, 3), U::MADDAbc);                                // maddaz ACC, vf3, vf5z
        upper(up(0x08 + W, kXyzw, 0, 4, 6), U::MADDbc);                                 // maddw vf6, vf4, vf0w
        upper(upSpecial(7, W, kXyz, 6, 6), U::CLIP);                                    // clipw.xyz vf6, vf6w
        lower((0x40u << 25) | (W << 23) | (W << 21) | (6u << 16) | (0xEu << 6) | 0x3C, L::DIV);  // div Q, vf0w, vf6w
        lower(loSpecial(0xE, 3, 0, 0, 0), L::WAITQ);
        upper(up(0x1C, kXyz, 0, 6, 7), U::MULq);                                        // mulq.xyz vf7, vf6, Q
        upper(upSpecial(5, 1, kXyzw, 8, 7), U::FTOI4);                                  // ftoi4 vf8, vf7
        pair(loSpecial(0xD, 1, kXyzw, 2, 8), L::SQI, up(0x18 + X, kXyzw, 6, 5, 9), U::MULbc);  // sqi vf8, (vi2++) | mulx vf9, vf5, vf6x
        lower(loSpecial(0x1B, 0, 0, 0, 2), L::XGKICK);                                  // xgkick vi2
        lower(isubiu(3, 3, 1), L::ISUBIU);
        {
            const std::uint32_t here = static_cast<std::uint32_t>(w.size() / 2);
            const std::uint32_t back = static_cast<std::uint32_t>(static_cast<int>(loop) - static_cast<int>(here) - 1);
            lower(lo(0x29, 0, 0, 3, back), L::IBNE);                                    // ibne vi3, vi0, loop
        }
        upper(up(0x2A, kXyzw, 6, 8, 10), U::MUL);                                       // delay slot: mul vf10, vf8, vf6
        upper(kUNop, U::NOP);
        w[w.size() - 1] |= kEBit;
        upper(kUNop, U::NOP);
        if (at + (w.size() / 2) * 8 > kMicro) {
            std::cerr << "sem espaço para o laço longo\n";
            return 1;
        }
        for (std::size_t k = 0; k < w.size() / 2; ++k) {
            const auto d = anyps2::vu::decode(w[k * 2], w[k * 2 + 1]);
            if (d.lower.op != wantL[k] || d.upper.op != wantU[k]) {
                std::cerr << "laço longo: par " << k << " não decodifica como o esperado: "
                          << anyps2::vu::disassemble(d, static_cast<std::uint32_t>(k * 8)) << "\n";
                return 1;
            }
        }
        const auto pairs = static_cast<std::uint32_t>(w.size() / 2);
        for (std::size_t k = 0; k < w.size(); ++k) image[at / 4 + k] = w[k];
        anyps2::vu::MicroBlob b;
        b.address = at;
        b.loadAddress = at;
        b.words = w;
        blobs.push_back(b);
        progs.push_back({at, pairs, false, true});
        at += pairs * 8;
    }

    // Programas de fluxo (fatia 2 do plano do VU1 enxuto): o gerador do código recompilado trata cada
    // contexto de desvio e de bit E por um caminho estático, e o sorteio acima quase não passa por
    // eles (delay slot sempre normal, programas de uma página, sem bit E fora do fim). Estes cobrem,
    // com a mesma estrutura de programa (termina em par E + delay slot), alvos nas fronteiras de 64 pares,
    // desvio no delay slot, bit E no desvio e no delay slot, JR entre trechos e laços infinitos. Entram na
    // mesma imagem, depois dos sorteados, e usam o gerador de números depois deles (os programas
    // sorteados não mudam). Ficam numa tabela à parte (kFuzzVuHand).
    struct HandProg {
        std::string name;
        std::uint32_t offset, pairs;
        bool hasJr, infinite, errors;
        std::vector<std::uint32_t> slots;  // índices dos pares que são delay slot (o teste os altera)
    };
    std::vector<HandProg> hands;
    std::vector<anyps2::vu::MicroBlob> handBlobs;
    {
        struct Asm {
            std::vector<std::uint32_t> w;
            std::vector<std::uint32_t> slots;
            Rng& rng;
            const std::vector<std::uint32_t>& lowers;
            const std::vector<std::uint32_t>& uppers;
            std::uint32_t index() const { return static_cast<std::uint32_t>(w.size() / 2); }
            std::uint32_t anyUpper() { return uppers[rng.below(static_cast<std::uint32_t>(uppers.size()))]; }
            void normal() {
                w.push_back(lowers[rng.below(static_cast<std::uint32_t>(lowers.size()))]);
                w.push_back(anyUpper());
            }
            void normalE() {
                normal();
                w.back() |= kEBit;
            }
            void fillTo(std::uint32_t i) {
                while (index() < i) normal();
            }
            void withUpper(std::uint32_t lower, bool e = false) {
                w.push_back(lower);
                w.push_back(anyUpper() | (e ? kEBit : 0u));
            }
            // Desvio no par atual para o par `target` (índice); o delay slot é o par seguinte.
            std::uint32_t rel(std::uint32_t target) const {
                return static_cast<std::uint32_t>(static_cast<int>(target) - static_cast<int>(index()) - 1);
            }
            void b(std::uint32_t target, bool e = false) {
                slots.push_back(index() + 1);
                withUpper(lo(0x20, 0, 0, 0, rel(target)), e);
            }
            void bal(unsigned link, std::uint32_t target, bool e = false) {
                slots.push_back(index() + 1);
                withUpper(lo(0x21, 0, link, 0, rel(target)), e);
            }
            void ibne(unsigned a, unsigned c, std::uint32_t target, bool e = false) {
                slots.push_back(index() + 1);
                withUpper(lo(0x29, 0, c, a, rel(target)), e);
            }
            void ibeq(unsigned a, unsigned c, std::uint32_t target, bool e = false) {
                slots.push_back(index() + 1);
                withUpper(lo(0x28, 0, c, a, rel(target)), e);
            }
            void ibltz(unsigned a, std::uint32_t target, bool e = false) {
                slots.push_back(index() + 1);
                withUpper(lo(0x2C, 0, 0, a, rel(target)), e);
            }
            void jr(unsigned reg, bool e = false) {
                slots.push_back(index() + 1);
                withUpper(lo(0x24, 0, 0, reg, 0), e);
            }
            void setJump(std::uint32_t base, std::uint32_t targetIndex) {  // vi15 = endereço do par (em pares)
                withUpper(iaddiu(kJumpReg, 0, (base + targetIndex * 8) / 8));
            }
            void end() {  // fim: par E + delay slot (o teste põe vi15 aqui)
                normalE();
                normal();
            }
        };
        auto place = [&](const std::string& name, bool hasJr, bool infinite, Asm& a, bool errors = false) {
            const auto pairs = static_cast<std::uint32_t>(a.w.size() / 2);
            if (at + pairs * 8 > kMicro) {
                std::cerr << "sem espaço para o programa de fluxo " << name << "\n";
                std::exit(1);
            }
            for (std::size_t k = 0; k < a.w.size(); ++k) image[at / 4 + k] = a.w[k];
            anyps2::vu::MicroBlob blob;
            blob.address = at;
            blob.loadAddress = at;
            blob.words = a.w;
            handBlobs.push_back(blob);
            hands.push_back({name, at, pairs, hasJr, infinite, errors, a.slots});
            at += pairs * 8;
        };

        // Fronteiras de trecho (64 pares): alvo no primeiro par do trecho seguinte, delay slot no outro trecho,
        // laço que atravessa a fronteira e desvio de um trecho para o anterior.
        {
            Asm a{{}, {}, rng, lowers, uppers};
            a.fillTo(10); a.b(12); a.normal();                       // B curto
            a.fillTo(20); a.ibne(1, 2, 24); a.normal();
            a.fillTo(30); a.ibeq(3, 4, 62); a.normal();              // salta o início do laço
            a.fillTo(58); a.withUpper(iaddiu(kLoopReg, 0, 3));       // contador do laço (59..101)
            a.fillTo(61); a.ibne(8, 9, 64); a.normal();              // alvo exatamente na fronteira (64)
            a.fillTo(63); a.ibne(3, 4, 70); a.normal();              // delay slot (64) no outro trecho
            a.fillTo(99); a.withUpper(isubiu(kLoopReg, kLoopReg, 1));
            a.ibne(kLoopReg, 0, 59); a.normal();                     // volta ao trecho anterior (aresta de volta)
            a.fillTo(120); a.b(124); a.normal();
            a.fillTo(126); a.ibltz(5, 128); a.normal();              // alvo = primeiro par do terceiro trecho
            a.fillTo(134); a.bal(5, 138); a.normal();
            a.fillTo(144);
            a.end();
            place("fronteiras", false, false, a);
        }
        // JR entre trechos, para a frente e para trás (o alvo é absoluto: não vale realocado).
        {
            Asm a{{}, {}, rng, lowers, uppers};
            a.fillTo(5); a.withUpper(iaddiu(kLoopReg, 0, 2));
            a.fillTo(19); a.setJump(at, 70); a.jr(kJumpReg); a.normal();   // trecho 0 -> trecho 1
            a.fillTo(70); a.setJump(at, 30);                                // o laço volta ao trecho 0
            a.withUpper(isubiu(kLoopReg, kLoopReg, 1));
            a.ibeq(kLoopReg, 0, 76); a.normal();
            a.jr(kJumpReg); a.normal();
            a.fillTo(76); a.setJump(at, 130); a.jr(kJumpReg); a.normal();   // trecho 1 -> trecho 2
            a.fillTo(130);
            a.end();
            place("jr entre trechos", true, false, a);
        }
        // Desvio e bit E no delay slot, em todas as combinações (programas curtos: o sorteio de estados
        // escolhe se IBxx é tomado ou não).
        static const char* const kNames[] = {
            "b com ibne no delay slot", "ibne com b no delay slot", "bal com jr no delay slot", "b com bit E",
            "ibne com bit E", "jr com bit E", "bit E com b no delay slot", "ibne com bit E no delay slot",
            "b com bit E e bit E no delay slot", "bit E com bit E no delay slot", "ibne com ibne com ibne",
            "ibeq com bit E e ibne no delay slot"};
        for (unsigned v = 0; v < std::size(kNames); ++v) {
            Asm a{{}, {}, rng, lowers, uppers};
            a.fillTo(2);
            a.setJump(at, 10);
            bool jr = false;
            switch (v) {
                case 0: a.b(10); a.ibne(1, 2, 8); break;
                case 1: a.ibne(1, 2, 10); a.b(8); break;
                case 2: a.bal(5, 10); a.jr(kJumpReg); jr = true; break;
                case 3: a.b(10, true); a.normal(); break;
                case 4: a.ibne(1, 2, 10, true); a.normal(); break;
                case 5: a.jr(kJumpReg, true); a.normal(); jr = true; break;
                case 6: a.normalE(); a.b(10); break;
                case 7: a.ibne(1, 2, 10); a.normalE(); break;
                case 8: a.b(10, true); a.normalE(); break;
                case 9: a.normalE(); a.normalE(); break;
                case 10: a.ibne(1, 2, 10); a.ibne(3, 4, 13); a.ibne(5, 6, 14); break;
                default: a.ibeq(1, 2, 10, true); a.ibne(3, 4, 12); break;
            }
            if (v == 9 || v == 6) a.slots.push_back(4);  // o par depois do E também é delay slot
            a.fillTo(18);
            a.end();
            place(kNames[v], jr, false, a);
        }
        // Erro do guest no meio do trecho (lower inválido, bit D): depois da exceção, o ciclo e o estado visíveis
        // têm de ser os do interpretador (o código recompilado mantém o ciclo numa variável local e o grava em
        // cycle_ antes de qualquer chamada que lança).
        {
            std::uint32_t invalidLower = 0;
            for (std::uint32_t op = 0; op < 128 && !invalidLower; ++op) {
                for (const std::uint32_t low : {0x0u, 0x3Cu, 0x3Du, 0x3Eu, 0x3Fu}) {
                    const std::uint32_t w = (op << 25) | low;
                    if (anyps2::vu::decode(w, kUNop).lower.op == L::INVALID) {
                        invalidLower = w;
                        break;
                    }
                }
            }
            if (!invalidLower) {
                std::cerr << "nenhum lower inválido para o teste de erro\n";
                return 1;
            }
            for (unsigned v = 0; v < 2; ++v) {
                Asm a{{}, {}, rng, lowers, uppers};
                a.fillTo(12);
                if (v == 0) {
                    a.withUpper(invalidLower);
                } else {
                    a.normal();
                    a.w.back() |= 1u << 28;  // bit D
                }
                a.fillTo(18);
                a.end();
                place(v == 0 ? "erro: lower inválido" : "erro: bit D", false, false, a, true);
            }
        }
        // Laços infinitos: o limite de pares por execução tem de dar o erro (dentro de um trecho, entre
        // trechos e por JR).
        {
            Asm a{{}, {}, rng, lowers, uppers};
            a.fillTo(2); a.b(2); a.normal();            // o B salta para si mesmo
            a.fillTo(8);
            a.end();
            place("laço infinito num trecho", false, true, a);
        }
        {
            Asm a{{}, {}, rng, lowers, uppers};
            a.fillTo(70); a.b(62); a.normal();          // 62..71 atravessa a fronteira do trecho
            a.fillTo(75);
            a.end();
            place("laço infinito entre trechos", false, true, a);
        }
        {
            Asm a{{}, {}, rng, lowers, uppers};
            a.fillTo(1); a.setJump(at, 2); a.jr(kJumpReg); a.normal();   // JR para si mesmo
            a.fillTo(8);
            a.end();
            place("laço infinito por jr", true, true, a);
        }
    }

    const std::string out = argv[2];
    // 4 grupos de programas, cada um num arquivo com a sua tabela
    // (kFuzzVu0..3): compilam em paralelo (sob sanitizers o C++ gerado é
    // lento de compilar).
    constexpr std::size_t kGroups = 4;
    for (std::size_t g = 0; g < kGroups; ++g) {
        std::vector<anyps2::vu::MicroBlob> group;
        for (std::size_t i = g; i < blobs.size(); i += kGroups) group.push_back(blobs[i]);
        const std::string table = "kFuzzVu" + std::to_string(g);
        std::string text;
        for (const auto& f : anyps2::codegen::generateVuSources(group, table, 1u << 20)) text += f.text + "\n";
        const std::string name = out + "/vu_fuzz_" + std::to_string(g) + ".cpp";
        std::ofstream o(name, std::ios::binary);
        o << text;
        if (!o) {
            std::cerr << "falha ao escrever " << name << "\n";
            return 1;
        }
    }
    {
        std::string text;
        for (const auto& f : anyps2::codegen::generateVuSources(handBlobs, "kFuzzVuHand", 1u << 20)) text += f.text + "\n";
        std::ofstream o(out + "/vu_fuzz_4.cpp", std::ios::binary);
        o << text;
        if (!o) {
            std::cerr << "falha ao escrever vu_fuzz_4.cpp\n";
            return 1;
        }
    }
    std::ofstream h(out + "/vu_fuzz_data.h", std::ios::binary);
    h << "// Gerado por vu_fuzz_gen — não edite.\n#pragma once\n#include <cstdint>\n\n"
         "struct FuzzProgram {\n    std::uint32_t offset;\n    std::uint32_t pairs;\n    bool hasJr;\n    bool longLoop;\n};\n"
         "inline constexpr FuzzProgram kFuzzPrograms[] = {\n";
    for (const auto& p : progs) {
        h << "    {" << p.offset << "u, " << p.pairs << "u, " << (p.hasJr ? "true" : "false") << ", "
          << (p.longLoop ? "true" : "false") << "},\n";
    }
    h << "};\n";
    h << "struct HandProgram {\n    const char* name;\n    std::uint32_t offset;\n    std::uint32_t pairs;\n    bool hasJr;\n"
         "    bool infinite;\n    bool errors;\n    std::uint32_t slots[16];\n    std::uint32_t slotCount;\n};\n"
         "inline constexpr HandProgram kHandPrograms[] = {\n";
    for (const auto& p : hands) {
        h << "    {\"" << p.name << "\", " << p.offset << "u, " << p.pairs << "u, " << (p.hasJr ? "true" : "false") << ", "
          << (p.infinite ? "true" : "false") << ", " << (p.errors ? "true" : "false") << ", {";
        const std::size_t n = std::min<std::size_t>(p.slots.size(), 16);
        for (std::size_t k = 0; k < 16; ++k) h << (k ? ", " : "") << (k < n ? p.slots[k] : 0u) << "u";
        h << "}, " << n << "u},\n";
    }
    h << "};\ninline constexpr std::uint32_t kFuzzImage[] = {";
    for (std::size_t k = 0; k < image.size(); ++k) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08Xu,", image[k]);
        h << (k % 8 == 0 ? "\n    " : " ") << buf;
    }
    h << "\n};\n";
    std::cout << "vu_fuzz_gen: " << progs.size() << " programas, " << uppers.size() << " uppers, " << lowers.size()
              << " lowers (" << lowerOps.size() << " operações lower distintas)\n";
    return h ? 0 : 1;
}
