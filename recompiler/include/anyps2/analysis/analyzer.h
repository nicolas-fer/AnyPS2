#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "anyps2/elf/elf_file.h"

namespace anyps2::analysis {

// Uma função do guest que vira uma função C++.
struct Function {
    std::uint32_t start = 0;
    std::uint32_t end = 0;  // exclusivo
    std::string name;       // símbolo, ou "sub_XXXXXXXX"
    bool fromSymbol = false;

    // Alvos de desvio dentro da função (viram rótulos de goto).
    std::set<std::uint32_t> labels;
    // Pontos onde a execução pode (re)entrar vindo de fora: retornos de
    // chamadas, alvos de saltos externos, ponteiros encontrados em dados.
    // Viram casos do switch de entrada da função.
    std::set<std::uint32_t> entries;

    bool contains(std::uint32_t a) const { return a >= start && a < end; }
};

struct CodeRegion {
    std::uint32_t start;
    std::uint32_t end;
    std::string name;
};

struct AnalysisOptions {
    // Endereços extras de início de função (configuração por jogo).
    std::vector<std::uint32_t> extraFunctions;
    // Varre .data/.rodata atrás de ponteiros para código (jump tables,
    // tabelas de callbacks).
    bool scanDataPointers = true;
    // Varre pares lui/addiu|ori que formam endereços de código.
    bool scanCodeConstants = true;
};

struct Diagnostic {
    std::uint32_t address;
    std::string message;
};

struct ProgramModel {
    std::uint32_t entry = 0;
    std::vector<CodeRegion> regions;
    std::vector<Function> functions;  // ordenadas e disjuntas
    std::vector<Diagnostic> warnings;

    const Function* functionContaining(std::uint32_t a) const;
    Function* functionContaining(std::uint32_t a);
    bool inCode(std::uint32_t a) const;
};

// Descobre funções, rótulos e pontos de entrada. Lança anyps2::Error se o ELF
// não tem código executável.
ProgramModel analyze(const elf::ElfFile& elf, const AnalysisOptions& options = {});

}  // namespace anyps2::analysis
