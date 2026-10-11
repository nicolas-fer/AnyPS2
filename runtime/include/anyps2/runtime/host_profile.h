#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace anyps2::rt {

namespace gs {
struct BandStats;
struct TexReport;
struct TexSimReport;
struct TexCacheReport;
}

// Contadores da medição "vu1 sync" (ANYPS2_PROFILE=1): quantas vezes por quadro o EE
// teria de esperar uma thread dona do VU1/GIF/GS (cada contador é um ponto de sincronia
// potencial), como os lotes do VIF1 se espaçam e como os eventos do GS são entregues.
// Só observam: nada aqui altera o estado emulado. Tudo roda na thread do EE (o produtor
// do GS também), então os campos são simples, sem atômicos.
struct SyncStats {
    static constexpr unsigned kBuckets = 48;  // histogramas em potências de 2
    // Eventos do GS: SIGNAL, FINISH, LABEL; origem 0 = escrita direta/privilegiada,
    // 1 a 3 = PATH1/2/3, 4 = outra.
    enum Ev : unsigned { EvSignal, EvFinish, EvLabel, EvCount };
    static constexpr unsigned kSources = 5;
    // Entrega ao CSR: SIGNAL, FINISH e VSINT, cada um interrupção, só bit ou bit já ligado.
    enum Raise : unsigned { RaiseSignal, RaiseFinish, RaiseVsint, RaiseCount };
    enum Outcome : unsigned { OutInterrupt, OutCsrOnly, OutAlreadySet, OutCount };

    // Pontos de sincronia potenciais.
    std::uint64_t gifReads = 0;                                  // leituras do EE de registradores do GIF
    std::uint64_t gsCsrReads = 0, gsSiglblidReads = 0, gsOtherPrivReads = 0;  // leituras privilegiadas do GS
    std::uint64_t vu1MemReads = 0, vu1MemWrites = 0;             // EE em 0x1100_8000–0x1100_FFFF (ANYPS2_VU1_MEMCOUNT)
    std::uint64_t vu0Cross = 0;                                  // VU0 tocando os registradores do VU1
    std::uint64_t cfc2Vpustat = 0, cfc2Fbrst = 0, cfc2Cmsar1 = 0;
    std::uint64_t ctc2Cmsar1 = 0, ctc2Fbrst = 0;                 // CTC2 que inicia/zera o VU1
    std::uint64_t gsDownloadDma = 0, gsDownloadFifo = 0, gsLocalToHost = 0;
    std::uint64_t rowReads = 0, rowReadsMode = 0;                // leituras de ROW do VIF1 (e as com MODE != 0)
    std::uint64_t mscnt = 0;
    // Lotes do VIF1 (do início do DMA até o canal terminar).
    std::uint64_t vif1Starts = 0, gifDmaStarts = 0, gifFifoWrites = 0;
    std::uint64_t mscalTotal = 0, xgkickTotal = 0;               // MSCAL/MSCALF/MSCNT do VIF1 e XGKICK, em geral
    std::uint64_t batches = 0, batchMscal = 0, batchXgkick = 0;  // lotes fechados e o que houve dentro deles
    std::uint64_t batchMscalMax = 0, batchXgkickMax = 0;         // maiores valores por lote (desde o começo)
    std::uint64_t gapHist[kBuckets] = {};                        // ciclos do EE entre inícios do VIF1: < 2^k
    std::uint64_t hostHist[kBuckets] = {};                       // ns de host por lote: < 2^k
    // Eventos do GS.
    std::uint64_t evWrites[EvCount][kSources] = {};
    std::uint64_t evRaises[RaiseCount][OutCount] = {};
    bool vu1MemCounted = false;                                  // a memória do VU1 foi mapeada com contador
};

// Tempo do host por parte do runtime (ANYPS2_PROFILE=1): diz onde o PC
// gasta o tempo de cada quadro. A contagem é exclusiva — um desenho do GS
// disparado pelo VU1 (XGKICK) conta para o GS, não para o VU1. O que não
// está em nenhuma parte (código do EE, HLE do kernel e do IOP) fica em "EE".
// Só uma thread do EE roda por vez (bastão do kernel), então a pilha é única.
class HostProfile {
    using Clock = std::chrono::steady_clock;

public:
    // GsWait: o EE parado esperando o worker do GS (leitura da VRAM, FINISH,
    // VBlank). O desenho em si roda na thread do worker e é medido à parte
    // (addWorker), como fração do tempo de parede.
    enum Part : unsigned { Ee, Gif, Gs, GsWait, Vu0, Vu1, Ipu, Vif, Count };

    static void enable();
    static bool enabled() { return enabled_; }
    // Relatório (percentual e segundos de cada parte desde enable()).
    static std::string report();
    // Relatório do trecho desde a última chamada (ou desde enable()), com a
    // velocidade em VBlanks por segundo do host.
    static std::string interval(std::uint64_t vblank);
    // Serialização das faixas do GS (Gs::bandStats()): a linha "[perfil] faixas: ..."
    // do trecho desde a última chamada, com as contagens por VBlank (divididas
    // pelos VBlanks do intervalo) e os pares (FBP, TBP0) mais frequentes dele.
    static std::string bandsInterval(const gs::BandStats& total, std::uint64_t vblank);
    // O mesmo desde o começo (valores totais), para o relatório final.
    static std::string bandsReport(const gs::BandStats& total, std::uint64_t vblank);
    // Texturas (Gs::texReport()): a linha "[perfil] texels: ..." do trecho desde a última
    // chamada (por VBlank) e o total desde o começo.
    static std::string texelsInterval(const gs::TexReport& total, std::uint64_t vblank);
    static std::string texelsReport(const gs::TexReport& total, std::uint64_t vblank);
    // Simulação do cache de texturas (Gs::texSim()): a linha "[perfil] texcache sim: ..."
    // do trecho (por VBlank) e o total.
    static std::string texSimInterval(const gs::TexSimReport& total, std::uint64_t vblank);
    static std::string texSimReport(const gs::TexSimReport& total, std::uint64_t vblank);
    // Cache de texels crus (Gs::texCacheReport(), ANYPS2_GS_TEXCACHE=1): a linha
    // "[perfil] texcache: ..." do trecho (por VBlank) e o total. Vazio com o cache desligado.
    static std::string texCacheInterval(const gs::TexCacheReport& total, std::uint64_t vblank);
    static std::string texCacheReport(const gs::TexCacheReport& total, std::uint64_t vblank);
    // Medição "vu1 sync" (SyncStats): a linha "[perfil] vu1 sync: ..." do trecho desde a última
    // chamada (por VBlank) e o total desde o começo.
    static std::string syncInterval(std::uint64_t vblank);
    static std::string syncReport(std::uint64_t vblank);
    static SyncStats& sync() { return sync_; }
    // Soma 1 a um contador (um teste de bool com o perfil desligado).
    static void count(std::uint64_t SyncStats::* field) {
        if (enabled_) ++(sync_.*field);
    }
    static void gsEvent(SyncStats::Ev ev, unsigned source) {
        if (enabled_) ++sync_.evWrites[ev][source < 4 ? source : 4];
    }
    // Entrega de um evento do GS ao CSR (bit do CSR: 0 SIGNAL, 1 FINISH, 3 VSINT).
    static void gsRaise(unsigned csrBit, SyncStats::Outcome outcome) {
        if (!enabled_) return;
        const unsigned k = csrBit == 0 ? SyncStats::RaiseSignal : csrBit == 1 ? SyncStats::RaiseFinish
                           : csrBit == 3 ? SyncStats::RaiseVsint : SyncStats::RaiseCount;
        if (k != SyncStats::RaiseCount) ++sync_.evRaises[k][outcome];
    }
    // MSCAL/MSCALF/MSCNT do VIF1 e XGKICK: contam no total e no lote aberto, se houver.
    static void noteMscal(bool isMscnt);
    static void noteXgkick();
    // Para os testes: desliga o perfil de novo (enable() zera os contadores).
    static void disable() { enabled_ = false; }

    // Um lote do VIF1: cria-se ao ligar CHCR.STR do canal e vive até o canal terminar (ou
    // pausar). Registra a distância (ciclos do EE) até o início anterior e o tempo de host.
    class Vif1Batch {
    public:
        explicit Vif1Batch(std::uint64_t eeCycles) {
            if (enabled_) begin(eeCycles);
        }
        ~Vif1Batch() {
            if (active_) end();
        }
        Vif1Batch(const Vif1Batch&) = delete;
        Vif1Batch& operator=(const Vif1Batch&) = delete;

    private:
        void begin(std::uint64_t eeCycles);
        void end();
        bool active_ = false;
        Clock::time_point t0_{};
    };

    // Tempo gasto pelo worker do GS (chamado da thread dele).
    static void addWorker(std::int64_t ns) { workerNs_.fetch_add(ns, std::memory_order_relaxed); }
    // Faixas do GS (threads) que somam em addWorker: o relatório divide a soma
    // pelo número de faixas para dar a ocupação média por faixa.
    static void setWorkerLanes(unsigned lanes) { workerLanes_ = lanes ? lanes : 1; }

    class Scope {
    public:
        explicit Scope(Part p) {
            if (enabled_) enter(p);
        }
        ~Scope() {
            if (active_) leave();
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        void enter(Part p);
        void leave();
        bool active_ = false;
        Part previous_ = Ee;
    };

private:
    static void charge(Clock::time_point now);
    static inline bool enabled_ = false;
    static inline Part current_ = Ee;
    static inline Clock::time_point since_{};
    static inline Clock::time_point start_{};
    static inline std::int64_t ns_[Count] = {};
    static inline std::int64_t mark_[Count] = {};
    static inline std::uint64_t markVblank_ = 0;
    static inline std::atomic<std::int64_t> workerNs_{0};
    static inline std::atomic<unsigned> workerLanes_{1};
    static inline std::int64_t workerMark_ = 0;
    static inline std::uint64_t bandsMarkVblank_ = 0;
    static inline SyncStats sync_{};
    static inline std::uint64_t syncMarkVblank_ = 0;
    // Lote do VIF1 aberto: o que aconteceu dentro dele até agora.
    static inline bool batchOpen_ = false;
    static inline std::uint64_t batchMscal_ = 0, batchXgkick_ = 0;
    static inline bool lastStartValid_ = false;
    static inline std::uint64_t lastStart_ = 0;
};

}  // namespace anyps2::rt
