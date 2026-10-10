#pragma once

// Acessos à VRAM dos desenhos do GS e a regra de ordem entre eles (gs_bands.h
// usa estes tipos). Fica separado de gs_bands.h porque o próprio Gs (gs.h) guarda
// os acessos pendentes e não pode depender de Vertex antes de ele ser definido.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace anyps2::rt::gs {

// Intervalo de páginas (8 KB, índice absoluto na VRAM).
struct VramSpan {
    std::uint32_t first = 0;
    std::uint32_t last = 0;
};

// Acesso à VRAM de um desenho. surface: 0 = FRAME, 1 = ZBUF, 2 = textura.
// Escritas de FRAME/ZBUF carregam o mapeamento (surface, base, bw, psm): dois
// desenhos com o mesmo mapeamento escrevem cada pixel na mesma faixa.
struct VramAccess {
    VramSpan span;
    bool write = false;
    unsigned surface = 0;
    std::uint32_t base = 0;
    std::uint32_t bw = 0;
    std::uint32_t psm = 0;
};

// Dois acessos colidem se as páginas se cruzam e pelo menos um escreve, salvo
// quando são escritas com o mesmo mapeamento (cada pixel fica numa faixa só, e
// as linhas de uma faixa são executadas na ordem do produtor).
bool clashes(const VramAccess& a, const VramAccess& b);

// Acessos desde a última barreira. Escritas com o mesmo mapeamento se fundem
// num intervalo só; leituras iguais não se repetem. Cheio: o produtor põe uma
// barreira (ver full()).
class PendingAccess {
public:
    static constexpr std::size_t kMax = 64;

    bool clashesWith(const VramAccess& a) const;
    void add(const VramAccess& a);
    bool full() const { return list_.size() >= kMax; }
    void clear() { list_.clear(); }
    std::size_t size() const { return list_.size(); }

private:
    std::vector<VramAccess> list_;
};

}  // namespace anyps2::rt::gs
