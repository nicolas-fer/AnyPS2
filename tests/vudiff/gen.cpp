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
        progs.push_back({at, pairs, hasJr});
        at += pairs * 8;
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
    std::ofstream h(out + "/vu_fuzz_data.h", std::ios::binary);
    h << "// Gerado por vu_fuzz_gen — não edite.\n#pragma once\n#include <cstdint>\n\n"
         "struct FuzzProgram {\n    std::uint32_t offset;\n    std::uint32_t pairs;\n    bool hasJr;\n};\n"
         "inline constexpr FuzzProgram kFuzzPrograms[] = {\n";
    for (const auto& p : progs) {
        h << "    {" << p.offset << "u, " << p.pairs << "u, " << (p.hasJr ? "true" : "false") << "},\n";
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
