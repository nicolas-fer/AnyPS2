#pragma once

// Recompilação estática do microcódigo dos VUs.
//
// Cada bloco de microcódigo (vu::MicroBlob) vira uma função C++ que executa
// os pares com a instrução já decodificada como constante (o núcleo inline do
// runtime, vu_exec.h, é especializado pelo compilador) e resolve os desvios
// por um switch sobre o deslocamento dentro do bloco. Ver VuProgramEntry no
// runtime para o contrato de execução.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "anyps2/elf/elf_file.h"
#include "anyps2/vu/scan.h"

namespace anyps2::codegen {

// Microcódigo nos segmentos carregáveis do ELF.
std::vector<vu::MicroBlob> findElfMicrocode(const elf::ElfFile& elf);

// Microcódigo de um dump da micro memória (ANYPS2_VU_DUMP): o deslocamento no
// arquivo é o endereço de carga.
std::vector<vu::MicroBlob> findDumpMicrocode(const std::filesystem::path& file);

struct VuSourceFile {
    std::string name;  // ex.: "vu_000.cpp"
    std::string text;
};

// Gera os arquivos C++ dos blocos e a tabela
//   extern const ::anyps2::rt::VuProgramEntry <table>[];
//   extern const std::size_t <table>Count;
// (com zero blocos a tabela tem uma entrada nula e Count = 0).
std::vector<VuSourceFile> generateVuSources(const std::vector<vu::MicroBlob>& blobs, const std::string& table,
                                            std::size_t pairsPerFile = 1500);

// Descrição de um bloco para mensagens ("MPG em 0x... → micro 0x...").
std::string describeBlob(const vu::MicroBlob& blob);

}  // namespace anyps2::codegen
