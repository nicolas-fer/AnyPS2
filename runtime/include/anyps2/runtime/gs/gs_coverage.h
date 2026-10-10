#pragma once

// Cobertura analítica dos primitivos do GS: quantos pixels um desenho escreveria
// (passam pela janela do SCISSOR e pelo SCANMSK), sem rasterizar nem ler a VRAM.
// O tempo do GS (FINISH) usa essa conta, feita na thread do EE enquanto o worker
// desenha; por isso tem de dar exatamente o que shadePixel contaria. O teste
// gs_coverage confere isso contra a contagem real do rasterizador.

#include <cstdint>

#include "anyps2/runtime/gs/gs.h"

namespace anyps2::rt::gs {

// Janela de escrita: SCISSOR (limites inclusivos) e SCANMSK (0 = sem máscara).
struct DrawWindow {
    int x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    unsigned scanmsk = 0;
};

// Triângulo preparado como o rasterizador o usa: vértices com área positiva
// (v[0], v[1], v[2]), caixa já cortada pela janela e funções de aresta
// incrementais. Os pesos do sombreamento saem na mesma ordem.
struct TriangleSetup {
    bool valid = false;  // área não nula e caixa não vazia dentro da janela
    Vertex v[3];
    int minX = 0, maxX = 0, minY = 0, maxY = 0;
    std::int64_t area = 0;     // |área| (divisor dos pesos)
    std::int64_t row[3] = {};  // função de aresta de cada lado no canto (minX, minY)
    std::int64_t dx[3] = {};    // variação por pixel à direita
    std::int64_t dy[3] = {};    // variação por linha
    std::int64_t bias[3] = {};  // regra top-left: 0 ou −1
};

struct SpriteRect {
    int minX = 0, maxX = 0, minY = 0, maxY = 0;  // inclusivos
    bool empty() const { return minX > maxX || minY > maxY; }
};

// Pixel de uma linha no passo i: t é a fração do segmento e (px, py) o pixel.
struct LineSample {
    double t = 0;
    int px = 0, py = 0;
};

TriangleSetup triangleSetup(const DrawWindow& w, const Vertex& v0, const Vertex& v1, const Vertex& v2);
SpriteRect spriteRect(const DrawWindow& w, const Vertex& v0, const Vertex& v1);
int lineSteps(const Vertex& v0, const Vertex& v1);
LineSample lineSample(const Vertex& v0, const Vertex& v1, int steps, int i);

// Número de pixels que o desenho do tipo PRIM.PRIM (0..6) escreveria na janela.
// v0, v1 e v2 são os vértices que o desenho usa (ponto: v0; linha e sprite:
// v0 e v1; triângulo: os três).
std::uint64_t coveredPixels(const DrawWindow& w, unsigned type, const Vertex& v0, const Vertex& v1, const Vertex& v2);

}  // namespace anyps2::rt::gs
