#pragma once

// Acessos à VRAM dos desenhos do GS e a regra de ordem entre eles (gs_bands.h
// usa estes tipos). Fica separado de gs_bands.h porque o próprio Gs (gs.h) guarda
// os acessos pendentes e não pode depender de Vertex antes de ele ser definido.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace anyps2::rt::gs {

// Conjunto de páginas (8 KB) da VRAM: um bit por página, 512 páginas em 8 palavras
// de 64 bits. Representa o retângulo de um acesso de forma exata (linha de páginas
// por linha de páginas, inclusive a volta no fim da VRAM), e a interseção de dois
// acessos é um AND de 8 palavras, sem laços sobre listas de intervalos.
struct PageSet {
    static constexpr std::uint32_t kPages = 512;
    static constexpr unsigned kWords = kPages / 64;
    std::uint64_t w[kWords] = {};

    bool empty() const {
        std::uint64_t any = 0;
        for (unsigned i = 0; i < kWords; ++i) any |= w[i];
        return any == 0;
    }
    bool full() const {
        std::uint64_t all = ~std::uint64_t{0};
        for (unsigned i = 0; i < kWords; ++i) all &= w[i];
        return all == ~std::uint64_t{0};
    }
    bool test(std::uint32_t page) const { return page < kPages && ((w[page >> 6] >> (page & 63)) & 1) != 0; }
    void setAll() {
        for (unsigned i = 0; i < kWords; ++i) w[i] = ~std::uint64_t{0};
    }
    // Liga as páginas lo..hi (0 <= lo <= hi < kPages).
    void setRange(std::uint32_t lo, std::uint32_t hi) {
        const std::uint32_t w0 = lo >> 6, w1 = hi >> 6;
        const std::uint64_t m0 = ~std::uint64_t{0} << (lo & 63);
        const std::uint64_t m1 = ~std::uint64_t{0} >> (63 - (hi & 63));
        if (w0 == w1) {
            w[w0] |= m0 & m1;
            return;
        }
        w[w0] |= m0;
        for (std::uint32_t i = w0 + 1; i < w1; ++i) w[i] = ~std::uint64_t{0};
        w[w1] |= m1;
    }
    void merge(const PageSet& o) {
        for (unsigned i = 0; i < kWords; ++i) w[i] |= o.w[i];
    }
    bool intersects(const PageSet& o) const {
        std::uint64_t any = 0;
        for (unsigned i = 0; i < kWords; ++i) any |= w[i] & o.w[i];
        return any != 0;
    }
};

// Acesso à VRAM de um desenho. surface: 0 = FRAME, 1 = ZBUF, 2 = textura.
// Escritas de FRAME/ZBUF carregam o mapeamento (surface, base, bw, psm): dois
// desenhos com o mesmo mapeamento escrevem cada pixel na mesma faixa.
struct VramAccess {
    PageSet pages;  // páginas tocadas (exatas por linha de páginas do retângulo)
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
