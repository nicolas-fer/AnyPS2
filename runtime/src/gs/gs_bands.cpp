#include "anyps2/runtime/gs/gs_bands.h"

#include <algorithm>

#include "anyps2/runtime/gs/vram.h"

namespace anyps2::rt::gs {

namespace {

bool overlap(const VramSpan& a, const VramSpan& b) {
    return !a.empty() && !b.empty() && a.first <= b.last && b.first <= a.last;
}

bool overlap(const VramAccess& a, const VramAccess& b) {
    return overlap(a.span, b.span) || overlap(a.span, b.wrap) || overlap(a.wrap, b.span) ||
           overlap(a.wrap, b.wrap);
}

bool sameSpans(const VramAccess& a, const VramAccess& b) {
    const auto same = [](const VramSpan& x, const VramSpan& y) {
        return (x.empty() && y.empty()) || (x.first == y.first && x.last == y.last);
    };
    return same(a.span, b.span) && same(a.wrap, b.wrap);
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

PageSpans pageSpan(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, unsigned rowLo, unsigned rowHi,
                   unsigned colLo, unsigned colHi) {
    // Tamanho da página em pixels e largura do buffer em páginas, como em vram.cpp
    // (byteAddress32/16/8 e nibbleAddress4): bloco = bp + k·32 + ordem, com
    // k = (y/altura)·largura + x/largura da página.
    unsigned pageW = 64, pageH = 32;
    std::uint64_t stride = bw;
    switch (psm) {
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
            pageH = 64;
            break;
        case PSMT8:
            pageW = 128;
            pageH = 64;
            stride = bw >> 1;
            break;
        case PSMT4:
            pageW = 128;
            pageH = 128;
            stride = bw >> 1;
            break;
        default:  // 32 bits, 24 bits e os de 8/4 bits altos (PSMT8H, PSMT4HL/HH)
            break;
    }
    // O endereço corta x e y em 2047 (fora disso daria a volta, sem ordem).
    rowHi = std::min(rowHi, 2047u);
    colHi = std::min(colHi, 2047u);
    rowLo = std::min(rowLo, rowHi);
    colLo = std::min(colLo, colHi);
    const std::uint64_t kMin = std::uint64_t{rowLo / pageH} * stride + colLo / pageW;
    const std::uint64_t kMax = std::uint64_t{rowHi / pageH} * stride + colHi / pageW;
    // Intervalo de blocos tocados; a página é bloco/32. A VRAM tem 512 páginas.
    constexpr std::uint64_t kPages = Vram::kSize / 8192;
    const std::uint64_t firstPage = (std::uint64_t{bp} + kMin * 32) / 32;
    const std::uint64_t lastPage = (std::uint64_t{bp} + kMax * 32 + 31) / 32;
    PageSpans s;
    if (lastPage - firstPage + 1 >= kPages) {
        s.span = VramSpan{0, static_cast<std::uint32_t>(kPages - 1)};
        return s;
    }
    const auto a = static_cast<std::uint32_t>(firstPage % kPages);
    const auto b = static_cast<std::uint32_t>(lastPage % kPages);
    if (a <= b) {
        s.span = VramSpan{a, b};
    } else {
        s.span = VramSpan{a, static_cast<std::uint32_t>(kPages - 1)};
        s.wrap = VramSpan{0, b};
    }
    return s;
}

bool clashes(const VramAccess& a, const VramAccess& b) {
    if (!a.write && !b.write) return false;
    if (!overlap(a, b)) return false;
    return !sameMapping(a, b);
}

bool PendingAccess::clashesWith(const VramAccess& a) const {
    return std::any_of(list_.begin(), list_.end(), [&a](const VramAccess& p) { return clashes(p, a); });
}

void PendingAccess::add(const VramAccess& a) {
    for (VramAccess& p : list_) {
        // Fundir escritas do mesmo mapeamento só vale com um intervalo cada (o casco
        // de dois intervalos que dão a volta seria a VRAM toda): as com volta ficam
        // como entradas à parte.
        if (sameMapping(p, a) && p.wrap.empty() && a.wrap.empty()) {
            p.span.first = std::min(p.span.first, a.span.first);
            p.span.last = std::max(p.span.last, a.span.last);
            return;
        }
        if (!p.write && !a.write && sameSpans(p, a)) return;
        if (p.write && a.write && sameMapping(p, a) && sameSpans(p, a)) return;
    }
    list_.push_back(a);
}

}  // namespace anyps2::rt::gs
