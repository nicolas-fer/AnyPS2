#pragma once

// Localiza microcódigo de VU dentro de dados arbitrários (segmentos do ELF ou
// dumps da micro memória gravados pelo runtime com ANYPS2_VU_DUMP).
//
// Duas fontes:
//  * pacotes VIF `MPG` montados em tempo de compilação: o código vem logo
//    depois do VIFcode e o endereço de carga é conhecido;
//  * blocos "crus" (o programa monta o MPG em tempo de execução, como o
//    `packet2_vif_add_micro_program` do ps2sdk): sequências de pares que
//    decodificam como instruções canônicas e contêm um bit E. O endereço de
//    carga é desconhecido; o runtime casa o bloco pelo conteúdo.
//
// Falsos positivos custam só tamanho de código: o runtime confere cada par
// com a micro memória antes de executar a versão recompilada.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace anyps2::vu {

inline constexpr std::uint32_t kUnknownLoad = 0xFFFFFFFFu;

struct MicroBlob {
    std::uint32_t address = 0;              // posição na entrada (endereço virtual ou deslocamento)
    std::uint32_t loadAddress = kUnknownLoad;  // destino na micro memória (bytes), se conhecido
    std::vector<std::uint32_t> words;       // pares (lower, upper)
    bool fromMpg = false;
};

// Um par que pode ser microcódigo de verdade: as duas metades canônicas (ou
// lower como imediato do LOI) e sem os bits de depuração D/T.
bool plausiblePair(std::uint32_t lowerWord, std::uint32_t upperWord);

// `address` é o endereço do primeiro byte de `data` (alinhamentos são
// verificados no endereço absoluto). Blocos idênticos aparecem uma vez.
std::vector<MicroBlob> findMicrocode(const std::uint8_t* data, std::size_t size, std::uint32_t address);

// Junta resultados de várias entradas eliminando blocos repetidos (o primeiro
// com endereço de carga conhecido vence).
void appendUnique(std::vector<MicroBlob>& out, std::vector<MicroBlob> more);

}  // namespace anyps2::vu
