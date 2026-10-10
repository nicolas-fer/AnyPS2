#pragma once

// Faixas do rasterizador do GS. A tela é cortada em blocos de 16 linhas e o
// bloco b pertence à faixa b % faixas: a carga de um triângulo grande vai para
// várias threads, e blocos vizinhos (de faixas diferentes) equilibram o trabalho.
//
// Como as faixas não são ordenadas entre si, o produtor só enfileira um desenho
// numa faixa se ela tem linhas que o desenho toca, e mantém o registro dos
// acessos à VRAM feitos desde a última barreira (gs_access.h). Um desenho que
// colide com um acesso pendente espera com uma barreira antes.

#include <cstdint>

#include "anyps2/runtime/gs/gs_access.h"
#include "anyps2/runtime/gs/gs_coverage.h"

namespace anyps2::rt::gs {

// Faixa dona da linha y (y ≥ 0).
inline bool rowInLane(unsigned lane, unsigned lanes, int y) {
    return (static_cast<unsigned>(y) >> 4) % lanes == lane;
}

// Bit k setado se a faixa k tem alguma linha em [lo, hi] (lo, hi ≥ 0).
std::uint32_t laneMask(int lo, int hi, unsigned lanes);

// Linhas que um desenho escreve, já cortadas pela janela (lo > hi: nenhuma).
struct RowRange {
    int lo = 1;
    int hi = 0;
    bool empty() const { return lo > hi; }
};
RowRange drawRows(const DrawWindow& w, unsigned type, const Vertex& v0, const Vertex& v1, const Vertex& v2);
// O mesmo para as colunas (lo/hi em x), cortadas pela janela.
RowRange drawCols(const DrawWindow& w, unsigned type, const Vertex& v0, const Vertex& v1, const Vertex& v2);

// Páginas de um retângulo de pixels: linhas rowLo..rowHi, colunas 0..cols-1, num
// buffer de base `base` (páginas) e largura bw (em unidades de 64 pixels), no
// formato psm. Conservador: o intervalo contém todas as páginas tocadas, e pode
// conter mais.
VramSpan pageSpan(std::uint32_t psm, std::uint32_t base, std::uint32_t bw, unsigned rowLo, unsigned rowHi,
                  unsigned cols);

}  // namespace anyps2::rt::gs
