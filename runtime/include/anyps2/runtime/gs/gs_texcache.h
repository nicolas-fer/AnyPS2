#pragma once

// Cache de texels crus do GS (ANYPS2_GS_TEXCACHE=1; desligado por padrão).
//
// O que se guarda: a palavra crua do texel como readPixel a devolve (índice de 4/8
// bits, 16 bits, 24/32 bits), sem CLUT, TEXA nem wrap. A expansão para RGBA continua
// na faixa (paleta memorizada por faixa, Gs::palette), então uma troca de CLUT/CSA/TEXA
// não invalida nada aqui.
//
// Estrutura: um TexLevel é uma versão imutável de um nível de textura (formato, TBP,
// TBW e tamanho w×h). A grade dele é a de blocos de VRAM do formato (8×8 em 32 bits,
// 16×8 em 16 bits, 16×16 em T8, 32×16 em T4), e cada bloco é preenchido sob demanda
// por quem o pedir primeiro, lendo a VRAM com readPixel (idêntico ao caminho sem
// cache por construção). Estado do bloco: vazio -> ocupado (CAS, o vencedor preenche)
// -> pronto (store release). Os outros esperam (yield) e releem com acquire.
//
// Quando uma versão vale (a regra de que tudo depende): o produtor (a thread do EE)
// guarda, por chave, a versão corrente e as gerações das páginas da VRAM que ela cobre
// (Gs::pageState_, incrementadas em toda escrita, na ordem do programa, antes de
// qualquer desenho seguinte). Um desenho só recebe a versão corrente enquanto as
// gerações não mudaram; senão nasce uma versão nova e a velha fica viva só para os
// desenhos em voo que a fixaram (shared_ptr no DrawEnv). Logo, enquanto uma versão
// existe nas mãos de desenhos enfileirados, nenhuma escrita na VRAM que ela cobre pode
// ter acontecido nem estar em andamento: um bloco preenchido em qualquer momento tem o
// mesmo conteúdo. Ver clashes() em gs_bands.cpp, que documenta o que isto exige do
// agendamento das faixas.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "anyps2/runtime/gs/gs_access.h"

namespace anyps2::rt::gs {

class Vram;

// Geometria do bloco de um formato de textura: dimensões em texels (potências de 2) e
// bytes por palavra crua na arena (1 para índices de 4/8 bits, 2 para 16 bits, 4 para
// 24/32 bits).
struct TexGeom {
    unsigned bwShift, bhShift;
    unsigned elemBytes;
};
TexGeom texGeom(std::uint32_t psm);

// Contadores compartilhados entre o cache (produtor) e as versões (que podem sobreviver
// ao índice enquanto há desenhos em voo): bytes de arenas vivas e o pico.
struct TexLiveCounters {
    std::atomic<std::uint64_t> liveBytes{0};
    std::atomic<std::uint64_t> peakLiveBytes{0};
};

class TexLevel {
public:
    enum State : std::uint8_t { Empty = 0, Busy = 1, Ready = 2 };
    // Como get() atendeu: o bloco já estava pronto (ou outra faixa o preencheu), esta
    // chamada o preencheu, ou a coordenada está fora da grade do nível (sem cache).
    enum class Got : std::uint8_t { Hit, Filled, Outside };

    TexLevel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, unsigned w, unsigned h,
             std::shared_ptr<TexLiveCounters> counters);
    ~TexLevel();
    TexLevel(const TexLevel&) = delete;
    TexLevel& operator=(const TexLevel&) = delete;

    // Palavra crua do texel (x, y) do nível, já com o wrap aplicado e dentro de 0..2047.
    // Fora de w×h devolve Outside e não toca `raw`: quem chama lê a VRAM direto.
    Got get(const Vram& vram, unsigned x, unsigned y, std::uint32_t& raw) const {
        if (x >= w_ || y >= h_) return Got::Outside;
        const unsigned bi = (y >> bhShift_) * gridW_ + (x >> bwShift_);
        Got got = Got::Hit;
        if (state_[bi].load(std::memory_order_acquire) != Ready) {
            got = ensure(vram, bi) ? Got::Filled : Got::Hit;
        }
        const std::size_t off = (static_cast<std::size_t>(bi) << (bwShift_ + bhShift_)) +
                                (static_cast<std::size_t>(y & ((1u << bhShift_) - 1)) << bwShift_) +
                                (x & ((1u << bwShift_) - 1));
        switch (elemBytes_) {
            case 1:
                raw = arena_[off];
                break;
            case 2: {
                std::uint16_t v;
                std::memcpy(&v, arena_.get() + off * 2, 2);
                raw = v;
                break;
            }
            default: {
                std::uint32_t v;
                std::memcpy(&v, arena_.get() + off * 4, 4);
                raw = v;
                break;
            }
        }
        return got;
    }

    std::uint32_t psm() const { return psm_; }
    unsigned width() const { return w_; }
    unsigned height() const { return h_; }
    // Memória da arena e dos estados (o que o orçamento do cache conta).
    std::size_t bytes() const { return bytes_; }

private:
    // Garante o bloco pronto. Verdadeiro se esta chamada o preencheu.
    bool ensure(const Vram& vram, unsigned bi) const;
    void fill(const Vram& vram, unsigned bi) const;

    std::uint32_t psm_, bp_, bw_;
    unsigned w_, h_;
    unsigned bwShift_, bhShift_, elemBytes_;
    unsigned gridW_;
    std::size_t bytes_ = 0;
    std::shared_ptr<TexLiveCounters> counters_;
    mutable std::unique_ptr<std::atomic<std::uint8_t>[]> state_;
    // Sem inicialização: só os blocos prontos são lidos (a arena grande vem do sistema
    // operacional já zerada e preguiçosa).
    mutable std::unique_ptr<std::uint8_t[]> arena_;
};

struct TexKey {
    std::uint32_t psm = 0, bp = 0, bw = 0, w = 0, h = 0;
    bool operator==(const TexKey& o) const {
        return psm == o.psm && bp == o.bp && bw == o.bw && w == o.w && h == o.h;
    }
};
struct TexKeyHash {
    std::size_t operator()(const TexKey& k) const {
        std::uint64_t h = 1469598103934665603ull;
        for (std::uint32_t x : {k.psm, k.bp, k.bw, k.w, k.h}) h = (h ^ x) * 1099511628211ull;
        return static_cast<std::size_t>(h);
    }
};

// Índice de versões (só o produtor mexe): uma versão corrente por chave, LRU e
// orçamento em bytes. O orçamento é um alvo, não um teto duro: versões fixadas por
// desenhos em voo continuam vivas depois de despejadas (liveBytes mostra o total).
class TexCache {
public:
    struct Entry {
        std::shared_ptr<TexLevel> level;
        PageSet pages;                    // páginas que a grade da versão cobre
        std::vector<std::uint32_t> gens;  // gerações dessas páginas quando a versão nasceu
        std::uint64_t validEpoch = 0;     // época de escrita (Gs::writeEpoch_) em que as gerações foram conferidas
        std::list<TexKey>::iterator lru;
    };
    struct Stats {
        std::uint64_t versions = 0;   // versões criadas
        std::uint64_t reused = 0;     // pedidos atendidos pela versão corrente
        std::uint64_t evictions = 0;  // versões despejadas do índice
        std::uint64_t bytes = 0;      // bytes das versões no índice
        std::uint64_t peakBytes = 0;
        std::uint64_t entries = 0;
    };

    explicit TexCache(std::size_t budgetBytes) : budget_(budgetBytes), counters_(std::make_shared<TexLiveCounters>()) {}

    // A entrada da chave (marcada como a mais recente), ou nula.
    Entry* find(const TexKey& k);
    // Troca (ou cria) a versão da chave por uma nova e despeja o excesso do orçamento.
    Entry& create(const TexKey& k, const PageSet& pages, std::vector<std::uint32_t> gens, std::uint64_t epoch);
    void noteReuse() { ++stats_.reused; }

    const Stats& stats() const { return stats_; }
    std::uint64_t liveBytes() const { return counters_->liveBytes.load(std::memory_order_relaxed); }
    std::uint64_t peakLiveBytes() const { return counters_->peakLiveBytes.load(std::memory_order_relaxed); }
    std::size_t budget() const { return budget_; }

private:
    void evictToBudget();

    std::size_t budget_;
    std::shared_ptr<TexLiveCounters> counters_;
    std::unordered_map<TexKey, Entry, TexKeyHash> map_;
    std::list<TexKey> lru_;  // frente = mais recente
    Stats stats_;
};

}  // namespace anyps2::rt::gs
