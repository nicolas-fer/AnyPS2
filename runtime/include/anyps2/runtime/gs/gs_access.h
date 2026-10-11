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
    bool empty() const { return first > last; }
};

// Páginas de um acesso: um intervalo, ou dois quando o acesso passa do fim da VRAM
// e dá a volta ([a..511] e [0..b]); `wrap` é vazio no caso comum.
struct PageSpans {
    VramSpan span;
    VramSpan wrap{1, 0};
};

// Acesso à VRAM de um desenho. surface: 0 = FRAME, 1 = ZBUF, 2 = textura.
// Escritas de FRAME/ZBUF carregam o mapeamento (surface, base, bw, psm): dois
// desenhos com o mesmo mapeamento escrevem cada pixel na mesma faixa.
struct VramAccess {
    VramSpan span;
    VramSpan wrap{1, 0};  // segundo intervalo (a volta da VRAM); vazio se não houver
    bool write = false;
    unsigned surface = 0;
    std::uint32_t base = 0;
    std::uint32_t bw = 0;
    std::uint32_t psm = 0;
    // Só para PendingAccess::dropDone: o acesso termina de vez quando a barreira de
    // número `seq` (contada desde o início) conclui; 0 = já terminou.
    std::uint64_t seq = 0;
};

// Sobreposição de páginas, sem a exceção do mesmo mapeamento de clashes().
bool accessesOverlap(const VramAccess& a, const VramAccess& b);

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

    explicit PendingAccess(std::size_t max = kMax) : max_(max) {}

    bool clashesWith(const VramAccess& a) const;
    // Para quem acessa a VRAM direto do produtor (CLUT, HOST→LOCAL): sem a exceção
    // do mesmo mapeamento, que só vale entre desenhos divididos em faixas. Conflita
    // se há sobreposição de páginas e pelo menos um dos dois escreve.
    bool conflictsWith(const VramAccess& a) const;
    void add(const VramAccess& a);
    // Esquece os acessos que já terminaram (seq ≤ done).
    void dropDone(std::uint64_t done);
    bool full() const { return list_.size() >= max_; }
    void clear() { list_.clear(); }
    std::size_t size() const { return list_.size(); }

private:
    std::size_t max_;
    std::vector<VramAccess> list_;
};

}  // namespace anyps2::rt::gs
