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
    // Só diagnóstico (contadores de CLUT do perfil; nada aqui decide ordem nem resultado):
    //  * laneMask: faixas que escrevem (bit k = faixa k); todas as faixas para tudo o que não
    //    é desenho (HOST/LOCAL→LOCAL, barreiras). A união vale na fusão de PendingAccess::add.
    //  * rect*: caixa de pixels do acesso (colunas/linhas, inclusive) com o endereço base em
    //    blocos (`bp`), para comparar em nível de bloco de 256 B; rectValid = falso quando não
    //    se conhece (volta de endereço, fusão de mapeamentos diferentes).
    //  * host: veio de uma HOST→LOCAL (em fila).
    std::uint32_t laneMask = ~0u;
    std::uint32_t bp = 0;
    std::uint16_t rx0 = 0, ry0 = 0, rx1 = 0, ry1 = 0;
    bool rectValid = false;
    bool host = false;
};

// Sobreposição de páginas, sem a exceção do mesmo mapeamento de clashes().
bool accessesOverlap(const VramAccess& a, const VramAccess& b);

// Diagnóstico: a escrita `w` toca algum bloco de 256 B que a leitura `r` (a área de uma
// CLUT) lê? Exige as caixas (rectValid); sem elas, ou com uma fusão de várias escritas, o
// resultado é uma aproximação por cima (a caixa é a união das caixas fundidas) e, sem
// caixa, devolve verdadeiro.
bool accessesOverlapBlocks(const VramAccess& w, const VramAccess& r);

// Dois acessos colidem se as páginas se cruzam e pelo menos um escreve, salvo
// quando são escritas com o mesmo mapeamento (cada pixel fica numa faixa só, e
// as linhas de uma faixa são executadas na ordem do produtor).
//
// O CACHE DE TEXELS (gs_texcache.h) DEPENDE DESTA REGRA. Uma versão de textura é fixada
// por todos os desenhos enfileirados enquanto as gerações das páginas dela não mudam, e
// qualquer faixa de qualquer um desses desenhos pode preencher qualquer bloco, lendo a
// VRAM no meio da execução. Para o conteúdo ser o mesmo em qualquer momento, é preciso
// que:
//  * leitura de textura SEMPRE colida com escrita nas mesmas páginas (só duas escritas
//    com o mesmo mapeamento escapam daqui; uma leitura nunca): toda escrita anterior, em
//    ordem de programa, termina antes das tarefas do desenho que lê (barreira posta pelo
//    produtor em draw), e toda escrita posterior espera esse desenho (a mesma colisão, vista
//    do outro lado, vira barreira antes da escrita);
//  * a leitura declarada de um desenho (reads[] de Gs::draw, por páginas) cubra toda página
//    que tenha um texel que ele busca: o preenchimento é por bloco, e um bloco inteiro cabe
//    numa página lógica, então preencher um bloco só lê páginas que o desenho declarou;
//  * as escritas fora dos desenhos tenham ordem com as faixas: HOST→LOCAL em modo direto só
//    é escrita pelo produtor se o destino não tem acesso não concluído e, com a transferência
//    aberta, a palavra seguinte espera (hostDrain_) os desenhos que a tocam; com acesso não
//    concluído a HOST→LOCAL abre em modo fila e suas palavras só chegam à VRAM por uma
//    barreira (Gs::flushHostQueue), submetida antes de qualquer desenho, cópia ou carga de
//    CLUT que toque o destino, e antes de qualquer waitIdle; LOCAL→LOCAL é barreira
//    (submit), e a carga de CLUT é barreira ou diferida (ANYPS2_GS_CLUT_DEP) com a leitura
//    registrada no pending_ e no outstanding_ (a CLUT não passa pelo cache de texels);
//  * toda escrita passe por Gs::bumpPages antes do desenho seguinte (as gerações). No modo
//    fila a escrita real acontece depois, na barreira: a geração muda no início da
//    transferência e de novo em cada submissão se um desenho fixou, nesse meio tempo, uma
//    versão de textura que cruza o destino (hostGenDirty_). Assim uma versão fixada antes
//    de uma barreira de palavras nunca serve a um desenho enfileirado depois dela, e as
//    leituras do desenho que a fixou, enfileirado antes da barreira, não veem as palavras.
// Mudar qualquer uma destas regras (por exemplo, deixar uma leitura correr junto com uma
// escrita do mesmo mapeamento) quebra o cache. O modo ANYPS2_GS_TEXCACHE_VERIFY=1 e os
// testes de invalidação de test_gs_texcache.cpp existem para pegar isso.
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
    const std::vector<VramAccess>& items() const { return list_; }

private:
    std::size_t max_;
    std::vector<VramAccess> list_;
};

}  // namespace anyps2::rt::gs
