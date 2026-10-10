#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

namespace anyps2::rt {

// Tempo do host por parte do runtime (ANYPS2_PROFILE=1): diz onde o PC
// gasta o tempo de cada quadro. A contagem é exclusiva — um desenho do GS
// disparado pelo VU1 (XGKICK) conta para o GS, não para o VU1. O que não
// está em nenhuma parte (código do EE, HLE do kernel e do IOP) fica em "EE".
// Só uma thread do EE roda por vez (bastão do kernel), então a pilha é única.
class HostProfile {
public:
    // GsWait: o EE parado esperando o worker do GS (leitura da VRAM, FINISH,
    // VBlank). O desenho em si roda na thread do worker e é medido à parte
    // (addWorker), como fração do tempo de parede.
    enum Part : unsigned { Ee, Gif, Gs, GsWait, Vu0, Vu1, Ipu, Count };

    static void enable();
    static bool enabled() { return enabled_; }
    // Relatório (percentual e segundos de cada parte desde enable()).
    static std::string report();
    // Relatório do trecho desde a última chamada (ou desde enable()), com a
    // velocidade em VBlanks por segundo do host.
    static std::string interval(std::uint64_t vblank);
    // Tempo gasto pelo worker do GS (chamado da thread dele).
    static void addWorker(std::int64_t ns) { workerNs_.fetch_add(ns, std::memory_order_relaxed); }

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
    using Clock = std::chrono::steady_clock;
    static void charge(Clock::time_point now);
    static inline bool enabled_ = false;
    static inline Part current_ = Ee;
    static inline Clock::time_point since_{};
    static inline Clock::time_point start_{};
    static inline std::int64_t ns_[Count] = {};
    static inline std::int64_t mark_[Count] = {};
    static inline std::uint64_t markVblank_ = 0;
    static inline std::atomic<std::int64_t> workerNs_{0};
    static inline std::int64_t workerMark_ = 0;
};

}  // namespace anyps2::rt
