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

// Páginas de um retângulo de pixels: linhas rowLo..rowHi, colunas colLo..colHi, num
// buffer de base `bp` (em blocos de 256 bytes, como TBP/FBP·32) e largura bw (em
// unidades de 64 pixels), no formato psm. Segue o endereçamento de vram.cpp: a
// página do pixel (x, y) é bp/32 + (y/altura)·largura + x/largura, com páginas de
// 64x32 (32 bits), 64x64 (16 bits), 128x64 (PSMT8) e 128x128 (PSMT4), e a largura
// do buffer em páginas é bw (bw/2 nos formatos de 8 e 4 bits, como em vram.cpp).
// O conjunto é exato por linha de páginas: uma faixa vertical estreita não inclui
// as páginas das outras colunas. Passar do fim da VRAM dá a volta; cobrir 512
// páginas ou mais liga todas.
PageSet pageSet(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, unsigned rowLo, unsigned rowHi,
                unsigned colLo, unsigned colHi);

}  // namespace anyps2::rt::gs
