#include "anyps2/runtime/gs/gs_coverage.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace anyps2::rt::gs {

namespace {

// Divisão inteira com divisor positivo, arredondando para baixo e para cima:
// os limites de x de cada linha saem daí, e C++ trunca em direção a zero.
std::int64_t floorDiv(std::int64_t a, std::int64_t b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}
std::int64_t ceilDiv(std::int64_t a, std::int64_t b) {
    return -floorDiv(-a, b);
}

// Função de aresta de a→b no ponto p (12.4), com a mesma fórmula de gs_draw.cpp.
std::int64_t edge(std::int64_t ax, std::int64_t ay, std::int64_t bx, std::int64_t by, std::int64_t px,
                  std::int64_t py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

// Regra top-left: arestas "de cima" ou "da esquerda" incluem os pixels exatamente
// sobre elas; as demais ficam com bias −1.
bool isTopLeft(const Vertex& p, const Vertex& q) {
    const int ex = q.x - p.x, ey = q.y - p.y;
    return (ey < 0) || (ey == 0 && ex > 0);
}

// O pixel (x, y) passa pelo SCISSOR e pelo SCANMSK (mesmo teste do shadePixel).
bool covers(const DrawWindow& w, int x, int y) {
    if (x < w.x0 || x > w.x1 || y < w.y0 || y > w.y1) return false;
    if ((w.scanmsk == 2 && (y & 1)) || (w.scanmsk == 3 && !(y & 1))) return false;
    return true;
}

// Linhas que o SCANMSK descarta (mesma regra de covers, sem olhar x).
bool rowSkipped(const DrawWindow& w, int y) {
    return (w.scanmsk == 2 && (y & 1)) || (w.scanmsk == 3 && !(y & 1));
}

// Triângulo: por linha, o intervalo de x que satisfaz as três arestas é
// contínuo (cada aresta é linear em x), então basta intersectar intervalos.
std::uint64_t triangleCoverage(const DrawWindow& w, const TriangleSetup& t) {
    if (!t.valid) return 0;
    const std::int64_t width = std::int64_t{t.maxX} - t.minX + 1;
    std::int64_t r[3] = {t.row[0], t.row[1], t.row[2]};
    std::uint64_t total = 0;
    for (int y = t.minY; y <= t.maxY; ++y) {
        if (!rowSkipped(w, y)) {
            std::int64_t lo = 0, hi = width - 1;
            for (int k = 0; k < 3; ++k) {
                // Com j = x − minX, a aresta vale c + j·d, onde c inclui o bias.
                const std::int64_t c = r[k] + t.bias[k];
                const std::int64_t d = t.dx[k];
                if (d == 0) {
                    if (c < 0) {
                        lo = 1;
                        hi = 0;
                    }
                } else if (d > 0) {
                    lo = std::max(lo, ceilDiv(-c, d));
                } else {
                    hi = std::min(hi, floorDiv(c, -d));
                }
            }
            if (lo <= hi) total += static_cast<std::uint64_t>(hi - lo + 1);
        }
        for (int k = 0; k < 3; ++k) r[k] += t.dy[k];
    }
    return total;
}

}  // namespace

TriangleSetup triangleSetup(const DrawWindow& w, const Vertex& v0, const Vertex& v1, const Vertex& v2) {
    TriangleSetup t;
    std::int64_t area = edge(v0.x, v0.y, v1.x, v1.y, v2.x, v2.y);
    if (area == 0) return t;
    const Vertex* a = &v0;
    const Vertex* b = &v1;
    const Vertex* c = &v2;
    if (area < 0) {
        std::swap(b, c);
        area = -area;
    }
    t.area = area;
    t.v[0] = *a;
    t.v[1] = *b;
    t.v[2] = *c;
    t.minX = std::max(w.x0, (std::min({a->x, b->x, c->x}) + 15) >> 4);
    t.maxX = std::min(w.x1, std::max({a->x, b->x, c->x}) >> 4);
    t.minY = std::max(w.y0, (std::min({a->y, b->y, c->y}) + 15) >> 4);
    t.maxY = std::min(w.y1, std::max({a->y, b->y, c->y}) >> 4);
    if (t.minX > t.maxX || t.minY > t.maxY) return t;
    t.valid = true;
    for (int k = 0; k < 3; ++k) {
        // Lado k é o oposto ao vértice k: (v1, v2), (v2, v0), (v0, v1).
        const Vertex& p = t.v[(k + 1) % 3];
        const Vertex& q = t.v[(k + 2) % 3];
        t.row[k] = edge(p.x, p.y, q.x, q.y, std::int64_t{t.minX} * 16, std::int64_t{t.minY} * 16);
        t.dx[k] = -std::int64_t{q.y - p.y} * 16;
        t.dy[k] = std::int64_t{q.x - p.x} * 16;
        t.bias[k] = isTopLeft(p, q) ? 0 : -1;
    }
    return t;
}

SpriteRect spriteRect(const DrawWindow& w, const Vertex& v0, const Vertex& v1) {
    const int xa = std::min(v0.x, v1.x), xb = std::max(v0.x, v1.x);
    const int ya = std::min(v0.y, v1.y), yb = std::max(v0.y, v1.y);
    SpriteRect r;
    r.minX = std::max(w.x0, (xa + 15) >> 4);
    r.maxX = std::min(w.x1, ((xb + 15) >> 4) - 1);
    r.minY = std::max(w.y0, (ya + 15) >> 4);
    r.maxY = std::min(w.y1, ((yb + 15) >> 4) - 1);
    return r;
}

int lineSteps(const Vertex& v0, const Vertex& v1) {
    const int dx = v1.x - v0.x, dy = v1.y - v0.y;
    return std::max(std::abs(dx), std::abs(dy)) >> 4;
}

LineSample lineSample(const Vertex& v0, const Vertex& v1, int steps, int i) {
    const int dx = v1.x - v0.x, dy = v1.y - v0.y;
    LineSample s;
    s.t = steps > 0 ? static_cast<double>(i) / steps : 0.0;
    s.px = (v0.x + static_cast<int>(std::lround(dx * s.t)) + 8) >> 4;
    s.py = (v0.y + static_cast<int>(std::lround(dy * s.t)) + 8) >> 4;
    return s;
}

std::uint64_t coveredPixels(const DrawWindow& w, unsigned type, const Vertex& v0, const Vertex& v1, const Vertex& v2) {
    switch (type) {
        case 0: {  // ponto
            return covers(w, (v0.x + 8) >> 4, (v0.y + 8) >> 4) ? std::uint64_t{1} : std::uint64_t{0};
        }
        case 1:
        case 2: {  // linha: o mesmo laço de drawLine, sem sombrear
            const int steps = lineSteps(v0, v1);
            std::uint64_t n = 0;
            for (int i = 0; i < std::max(steps, 1); ++i) {
                const LineSample s = lineSample(v0, v1, steps, i);
                if (covers(w, s.px, s.py)) ++n;
            }
            return n;
        }
        case 3:
        case 4:
        case 5:
            return triangleCoverage(w, triangleSetup(w, v0, v1, v2));
        case 6: {  // sprite: retângulo inteiro, menos as linhas do SCANMSK
            const SpriteRect r = spriteRect(w, v0, v1);
            if (r.empty()) return std::uint64_t{0};
            std::uint64_t rows = 0;
            for (int y = r.minY; y <= r.maxY; ++y) {
                if (!rowSkipped(w, y)) ++rows;
            }
            return rows * static_cast<std::uint64_t>(r.maxX - r.minX + 1);
        }
        default:
            return 0;
    }
}

}  // namespace anyps2::rt::gs
