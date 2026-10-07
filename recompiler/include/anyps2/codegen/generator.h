#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "anyps2/analysis/analyzer.h"
#include "anyps2/elf/elf_file.h"

namespace anyps2::codegen {

struct GeneratorOptions {
    std::string projectName;              // nome do executável gerado
    std::filesystem::path outputDir;
    std::filesystem::path anyps2Root;     // raiz do repositório AnyPS2 (runtime)
    std::size_t instructionsPerFile = 12000;  // divide o código para compilar em paralelo
};

struct GenerationReport {
    std::size_t functions = 0;
    std::size_t instructions = 0;
    std::size_t sourceFiles = 0;
    // Instruções emitidas como "não suportada" (lançam erro se executadas),
    // agrupadas por mnemônico.
    std::map<std::string, std::size_t> unsupported;
    // Palavras inválidas dentro de funções (dados no meio do código ou
    // código que nunca executa) — também lançam erro se executadas.
    std::size_t invalidWords = 0;
    std::vector<analysis::Diagnostic> warnings;
};

// Gera um projeto CMake completo:
//   <out>/CMakeLists.txt, <out>/<nome>.image (segmentos do ELF),
//   <out>/src/functions_NNN.cpp, <out>/src/program.cpp, <out>/src/functions.h
GenerationReport generateProject(const elf::ElfFile& elf, const analysis::ProgramModel& model,
                                 const GeneratorOptions& options);

// Gera apenas o corpo C++ de uma função (usado nos testes).
std::string generateFunction(const elf::ElfFile& elf, const analysis::ProgramModel& model,
                             const analysis::Function& function, GenerationReport& report);

// Nome C++ da função que começa em `start`.
std::string functionSymbol(std::uint32_t start);

// Formato do arquivo .image: "AP2IMG\0\0", u32 versão=1, u32 entrada,
// u32 nº de segmentos; por segmento: u32 vaddr, u32 filesz, u32 memsz,
// seguido de filesz bytes.
void writeImage(const elf::ElfFile& elf, const std::filesystem::path& path);

}  // namespace anyps2::codegen
