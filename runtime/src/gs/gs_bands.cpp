#include "anyps2/runtime/gs/gs_bands.h"

#include <algorithm>

#include "anyps2/runtime/gs/vram.h"

namespace anyps2::rt::gs {

namespace {

bool overlap(const VramSpan& a, const VramSpan& b) {
    return a.first <= b.last && b.first <= a.last;
}

// Mesma escrita para o mesmo buffer: o mapeamento pixel→endereço é o mesmo, então
// um pixel só aparece numa linha e portanto numa faixa só.
bool sameMapping(const VramAccess& a, const VramAccess& b) {
    return a.write && b.write && a.surface == b.surface && a.base == b.base && a.bw == b.bw && a.psm == b.psm;
}

}  // namespace

std::uint32_t laneMask(int lo, int hi, unsigned lanes) {
    if (lo > hi || lanes == 0) return 0;
    const std::uint32_t all = lanes >= 32 ? ~0u : (1u << lanes) - 1;
    std::uint32_t mask = 0;
    const int b1 = hi >> 4;
    for (int b = lo >> 4; b <= b1 && mask != all; ++b) {
        mask |= 1u << (static_cast<unsigned>(b) % lanes);
    }
    return mask;
}

RowRange drawRows(const DrawWindow& w, unsigned type, const Vertex& v0, const Vertex& v1, const Vertex& v2) {
    RowRange r;
    constexpr int kNone = 1 << 30;
    r.lo = kNone;
    r.hi = -kNone;
    switch (type) {
        case 0: {  // ponto
            const int y = (v0.y + 8) >> 4;
            r.lo = r.hi = y;
            break;
        }
        case 1:
        case 2: {  // linha: as linhas dos pixels amostrados
            const int steps = lineSteps(v0, v1);
            for (int i = 0; i < std::max(steps, 1); ++i) {
                const int py = lineSample(v0, v1, steps, i).py;
                r.lo = std::min(r.lo, py);
                r.hi = std::max(r.hi, py);
            }
            break;
        }
        case 3:
        case 4:
        case 5: {  // triângulo: a caixa já vem cortada pela janela
            const TriangleSetup t = triangleSetup(w, v0, v1, v2);
            if (t.valid) {
                r.lo = t.minY;
                r.hi = t.maxY;
            }
            break;
        }
        case 6: {  // sprite
            const SpriteRect s = spriteRect(w, v0, v1);
            if (!s.empty()) {
                r.lo = s.minY;
                r.hi = s.maxY;
            }
            break;
        }
        default:
            return RowRange{};
    }
    // O rasterizador só escreve dentro do SCISSOR (shadePixel).
    r.lo = std::max(r.lo, w.y0);
    r.hi = std::min(r.hi, w.y1);
    return r;
}

RowRange drawCols(const DrawWindow& w, unsigned type, const Vertex& v0, const Vertex& v1, const Vertex& v2) {
    RowRange r;
    constexpr int kNone = 1 << 30;
    r.lo = kNone;
    r.hi = -kNone;
    switch (type) {
        case 0: {  // ponto
            const int x = (v0.x + 8) >> 4;
            r.lo = r.hi = x;
            break;
        }
        case 1:
        case 2: {  // linha: as colunas dos pixels amostrados
            const int steps = lineSteps(v0, v1);
            for (int i = 0; i < std::max(steps, 1); ++i) {
                const int px = lineSample(v0, v1, steps, i).px;
                r.lo = std::min(r.lo, px);
                r.hi = std::max(r.hi, px);
            }
            break;
        }
        case 3:
        case 4:
        case 5: {
            const TriangleSetup t = triangleSetup(w, v0, v1, v2);
            if (t.valid) {
                r.lo = t.minX;
                r.hi = t.maxX;
            }
            break;
        }
        case 6: {
            const SpriteRect s = spriteRect(w, v0, v1);
            if (!s.empty()) {
                r.lo = s.minX;
                r.hi = s.maxX;
            }
            break;
        }
        default:
            return RowRange{};
    }
    r.lo = std::max(r.lo, w.x0);
    r.hi = std::min(r.hi, w.x1);
    return r;
}

VramSpan pageSpan(std::uint32_t psm, std::uint32_t base, std::uint32_t bw, unsigned rowLo, unsigned rowHi,
                  unsigned cols) {
    // Endereçamento de vram.cpp, pixel (x, y) no bloco bp + k·32 + ordem, com
    // k = (y/32)·bw + x/64 nos formatos de 32 bits, (y/64)·bw + x/64 nos de 16
    // bits, e metade da largura (bw/2) e colunas de 128 pixels nos de 8 e 4 bits.
    // Como a página é 32 blocos, a página do pixel está em [base + k, base + k + 1].
    // Para o limite inferior usa-se o menor deslocamento (divisão por 128 e
    // largura bw/2 nos formatos estreitos); para o superior, o maior (divisão
    // por 32 e largura bw). Qualquer um desses valores é só um limite, então o
    // intervalo cobre tudo o que o endereçamento toca.
    const bool narrow = psm == PSMT8 || psm == PSMT4;
    const std::uint64_t lowStride = narrow ? (bw >> 1) : bw;
    const unsigned colsEff = std::max(cols, 1u);
    const std::uint64_t first = std::uint64_t{base} + std::uint64_t{rowLo >> 7} * lowStride;
    const std::uint64_t last =
        std::uint64_t{base} + std::uint64_t{rowHi >> 5} * bw + ((colsEff - 1) >> 6) + 1;
    VramSpan s;
    // A VRAM dá a volta a cada 512 páginas: um intervalo que passa disso cobre tudo.
    if (last >= Vram::kSize / 8192 || first > last) {
        s.first = 0;
        s.last = Vram::kSize / 8192 - 1;
    } else {
        s.first = static_cast<std::uint32_t>(first);
        s.last = static_cast<std::uint32_t>(last);
    }
    return s;
}

bool clashes(const VramAccess& a, const VramAccess& b) {
    if (!a.write && !b.write) return false;
    if (!overlap(a.span, b.span)) return false;
    return !sameMapping(a, b);
}

bool PendingAccess::clashesWith(const VramAccess& a) const {
    return std::any_of(list_.begin(), list_.end(), [&a](const VramAccess& p) { return clashes(p, a); });
}

void PendingAccess::add(const VramAccess& a) {
    for (VramAccess& p : list_) {
        if (sameMapping(p, a)) {
            p.span.first = std::min(p.span.first, a.span.first);
            p.span.last = std::max(p.span.last, a.span.last);
            return;
        }
        if (!p.write && !a.write && p.span.first == a.span.first && p.span.last == a.span.last) return;
    }
    list_.push_back(a);
}

}  // namespace anyps2::rt::gs
