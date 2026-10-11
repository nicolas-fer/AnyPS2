#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <vector>

#include "anyps2/runtime/gs/gs_events.h"
#include "anyps2/runtime/vif_exec.h"

namespace anyps2::rt {

class Runtime;
class Gif;
class HostThread;
struct VuMemory;
namespace gs {
class Gs;
}

// Fila ordenada única de tudo o que o EE entrega ao VU1, ao GIF e ao GS (fatia 4 do plano do VU1
// em outra thread). Cada item tem `seq` (a ordem de emissão do EE, que é também a ordem
// PATH1/PATH2/PATH3 do GIF) e `stamp` (Timing::now() do EE ao enfileirar). Entram na fila:
//   - os itens do VifExec do VIF1 (Unpack, Mpg, Mscal/Mscnt, Direct, Path3Mask, Reset);
//   - Path3: o DMA do canal GIF e as escritas no GIF_FIFO (dados COPIADOS);
//   - GifWrite/GifReset: escritas do EE nos registradores do GIF e o reset do GIF (ResetEE);
//   - GsPriv: escritas privilegiadas do EE no GS que são estado do consumidor (PMODE e demais
//     registradores de vídeo, BUSDIR, reset do GS). CSR (sem reset), IMR e SIGLBLID são do
//     espelho do EE e não passam pela fila;
//   - Vblank: o que o timing faz no GS a cada VSYNC;
//   - Vu1Start (CTC2 CMSAR1) e Vu1Reset (FBRST.RST);
//   - Fence.
// O VIF0 e o VU0 ficam no EE e não entram.
//
// O CONSUMIDOR é o dono do VU1, do GIF e do Gs como produtor. Ele aplica cada item com
// Gs::setItemClock(item.stamp) antes, e nunca toca o estado do lado do EE: os eventos que o GS
// gera (SIGNAL, FINISH, LABEL, VSINT, reset) são PUBLICADOS numa fila de respostas (GsEvent com
// seq e due), e o EE os aplica no espelho dele (GsEeState: CSR, IMR, SIGLBLID, FINISH pendentes),
// que também pede a interrupção do INTC. Os erros do consumidor (GuestError/Unimplemented) são
// guardados com a mensagem e o pc do EE do item e relançados no EE.
//
// Três modos do consumidor:
//   - INLINE (padrão, ANYPS2_VU1_THREAD=0 ou ausente): cada push aplica o item na hora, com os
//     dados emprestados do chamador (sem cópia), esvazia a fila de respostas e relança o erro, se
//     houve. O tempo é o de antes da separação.
//   - ADIADO (setDeferred, só para testes): enfileira com cópia e só consome no fence (ou em
//     drain), na própria thread do EE. Mostra o que a thread precisa: o que o EE reescreve depois
//     do push não muda o que o GS recebe.
//   - THREAD (ANYPS2_VU1_THREAD=1, fatia 5): uma thread consumidora de verdade. A fila é um anel
//     SPSC de blocos (`ring_`, potência de 2 de slots; ANYPS2_VU1_RING muda o tamanho, para
//     teste): o EE preenche o slot com os dados COPIADOS (o mesmo preenchimento do modo adiado) e
//     publica `pubSeq_` com um store release (nenhum mutex ou RMW por item; publica-se por lote:
//     de imediato nos itens que disparam trabalho e a cada kPublishBatch UNPACK/MPG); o
//     consumidor publica `processedSeq_` (liberando o slot) depois de aplicar cada item. O EE
//     bloqueia quando o anel enche (nunca descarta) e o consumidor dorme quando o anel esvazia
//     (condvar só no caminho lento: o produtor só toma o mutex se o consumidor avisou que dorme).
//
// DETERMINISMO (W = 0). A thread só muda QUANDO o trabalho roda, não o que o EE observa: toda
// leitura do EE de estado que o consumidor altera passa por fence(), que espera o consumidor
// chegar ao último item enfileirado, aplica as respostas na ordem em que foram publicadas e
// relança o erro. Como as respostas (SIGNAL/FINISH/VSINT, e com elas o INTC_GS) só chegam ao EE
// num fence, e o fence é uma barreira completa (processou tudo, aplicou tudo), o estado visível
// ao EE em cada ponto de observação é o mesmo do modo inline. Os pontos de observação são, além
// dos acessos listados abaixo: o safepoint (Timing::process e nextEventTime, que o EE executa a
// cada <= 20000 ciclos: o relógio virtual não passa do carimbo de um item não processado sem
// esperá-lo), a entrega de interrupções (Kernel::serviceInterrupts) e a leitura/escrita do
// INTC_STAT, porque a interrupção do GS vem de uma resposta. Com W = 0 o ganho é só a
// sobreposição do consumidor com o EE entre dois safepoints; o horizonte W (fatia 6) o amplia.
//
// ERROS: GuestError/Unimplemented (e qualquer outra exceção) do consumidor são guardados com a
// exceção original e relançados no EE no próximo fence ou enfileiramento; os itens depois do que
// falhou são pulados até o relançamento (que antes espera o consumidor esvaziar o anel, para o
// recomeço ser determinista).
//
// ENCERRAMENTO: shutdown() (também no destrutor) pede a parada e dá join; o que ainda está no
// anel é descartado (o caminho normal faz um fence antes: Runtime::run). O consumidor nunca espera
// o EE, então não há deadlock; ele só espera os workers do GS, como sempre.
//
// Leituras do EE de estado do consumidor (CSR, SIGLBLID, IMR, GIF_STAT e demais registradores do
// GIF, PATH3 mascarado, registradores privilegiados do GS, BUSDIR, downloads, VPU_STAT, o VU1 lido
// pelo VU0 e a memória do VU1) passam por fence(), que hoje só relança erro pendente (e consome o
// que estiver adiado).
//
// O que o consumidor ainda lê do lado do EE, e de onde vem:
//   - o relógio: do item (Gs::setItemClock), nunca de Timing::now();
//   - TOP/ITOP e o VBlank do rastreador: do item do VIF (Mscal) ou do Vu1Start;
//   - o pc do EE (mensagens de erro): do item;
//   - Runtime::options() (traceGs no Gif/Vu, vuMode): só leitura, constante depois da criação;
//   - HostProfile: o consumidor tem a trilha de tempo e os contadores dele (bindConsumerThread);
//     GsTrace e VuTrace só são tocados por ele (o Gs e o VU1 são dele) e ANYPS2_TRACE=gs (que
//     imprime do EE e do consumidor) força o modo inline, com aviso.
// O Vu::xgkick chama Gif::kick direto: o VU1 e o GIF pertencem ao mesmo consumidor.
class Vu1Stream : public gs::GsEventSink {
public:
    // Tudo que se enfileira. Os ponteiros só valem durante o push (o item enfileirado tem cópia).
    struct Item {
        enum class Kind : std::uint8_t {
            Vif,        // vif: item do VifExec do VIF1
            Path3,      // data[0..qwords): DMA do GIF ou GIF_FIFO
            GifWrite,   // addr/value: escrita do EE em registrador do GIF
            GifReset,   // GIF.reset()
            GsPriv,     // addr/value: escrita privilegiada no GS
            Vblank,     // início do VBlank
            Vu1Start,   // start (bytes) e vblanks do rastreador
            Vu1Reset,   // VU1.reset()
            Fence,      // marca de ordem
        };
        explicit Item(Kind k) : kind(k) {}
        Kind kind;
        std::uint64_t seq = 0;    // numeração pelo stream (preenchida no push)
        std::uint64_t stamp = 0;  // Timing::now() do EE ao enfileirar
        std::uint32_t eePc = 0;   // pc do EE do item (erros)
        VifItem vif{VifItem::Kind::Reset};
        const std::uint8_t* data = nullptr;
        std::size_t qwords = 0;
        std::uint32_t addr = 0;
        std::uint64_t value = 0;
        std::uint32_t start = 0;
        std::uint64_t vblanks = 0;
    };

    Vu1Stream(Runtime& rt, VuMemory& vu, Gif& gif, gs::Gs& gs);
    ~Vu1Stream();
    Vu1Stream(const Vu1Stream&) = delete;
    Vu1Stream& operator=(const Vu1Stream&) = delete;

    // ---- Lado do EE: enfileirar ---------------------------------------------------------
    void pushVif(const VifItem& item);
    void pushPath3(const std::uint8_t* data, std::size_t qwords, std::uint32_t pc);
    void pushGifWrite(std::uint32_t addr, std::uint32_t value, std::uint32_t pc);
    void pushGifReset();
    // Escrita privilegiada do EE no GS (0x1200_0000...): CSR sem RESET, IMR e SIGLBLID vão direto
    // ao espelho; o resto vira item GsPriv.
    void writeGsPrivileged(std::uint32_t addr, std::uint64_t value, std::uint32_t pc);
    // SetGsCrt do kernel: SMODE1 e SMODE2.
    void setCrt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field);
    // Início do VBlank, no instante `stamp` (ciclos do EE).
    void pushVblank(std::uint64_t stamp);
    void pushVu1Start(std::uint32_t startBytes, std::uint32_t eePc);
    void pushVu1Reset();
    void pushFence();

    // ---- Lado do EE: ler estado do consumidor (todas passam pelo fence) -----------------
    // Espera o consumidor chegar ao último item enfileirado, aplica as respostas no espelho e
    // relança o erro guardado. Inline: só relança. Adiado: consome o que estiver na fila.
    void fence() {
        if (needsFence()) fenceSlow();
    }
    // Há o que um fence faria: itens não processados, respostas não aplicadas ou erro guardado.
    bool needsFence() const {
        return processedSeq_.load(std::memory_order_acquire) + 1 != nextSeq_ ||
               respCount_.load(std::memory_order_acquire) != 0 || failed_.load(std::memory_order_acquire);
    }
    std::uint64_t readGsPrivileged(std::uint32_t addr, std::uint32_t pc);
    // CSR como o EE o lê em `now` (sem contar como leitura privilegiada).
    std::uint64_t csr(std::uint64_t now);
    std::uint64_t imr();
    void setImr(std::uint64_t v) { fence(); eeState().setImr(v); }
    std::uint32_t readGif(std::uint32_t addr, std::uint32_t pc);
    // PATH3 mascarado (MSKPATH3): espelho do EE, sem fence (ver push).
    bool path3Masked();
    // Espelho do EE (sem fence: quem lê estado que o consumidor altera chama fence antes).
    gs::GsEeState& eeState();
    std::uint64_t nextEventTime();
    void processEvents(std::uint64_t now);

    // ---- Modo e diagnóstico -------------------------------------------------------------
    // ANYPS2_VU1_THREAD pedido pelo ambiente (0 ou ausente = inline, senão 1).
    unsigned requestedThread() const { return requestedThread_; }
    bool inlineMode() const { return !deferred_ && !threaded_; }
    // Há uma thread consumidora de verdade (ANYPS2_VU1_THREAD=1 e sem ANYPS2_TRACE=gs).
    bool threaded() const { return threaded_; }
    // Só para testes: enfileira com cópia e consome no fence (ou em drain), em vez de na hora.
    void setDeferred(bool on);
    // Liga ou desliga a thread consumidora (o equivalente do ANYPS2_VU1_THREAD em tempo de
    // execução; os testes usam). Esvazia a fila antes de trocar de modo. Com ANYPS2_TRACE=gs não
    // liga.
    void setThreaded(bool on);
    // Para a thread e dá join (idempotente). O que estiver no anel é descartado. Depois disso o
    // stream volta ao modo inline.
    void shutdown();
    // Consome tudo o que está na fila (o consumidor, quando adiado). Não relança erro.
    void drain();
    std::uint64_t pushedSeq() const { return nextSeq_ - 1; }
    std::uint64_t processedSeq() const { return processedSeq_.load(std::memory_order_acquire); }
    std::size_t queued() const { return static_cast<std::size_t>(nextSeq_ - 1 - processedSeq()); }
    // Há erro guardado do consumidor, ainda não relançado no EE.
    bool hasPendingError() const { return failed_.load(std::memory_order_acquire); }
    // seq do item que falhou (só vale com hasPendingError).
    std::uint64_t pendingErrorSeq() const { return errorSeq_; }

    // Só para testes e diagnóstico. Semente do jitter do consumidor (ANYPS2_VU1_JITTER): a cada
    // item ele cede a CPU ou dorme por um tempo sorteado, para sacudir o escalonamento. 0 desliga.
    void setJitterSeed(std::uint64_t seed) { jitterSeed_.store(seed, std::memory_order_relaxed); }
    // Fence forçado depois de cada enfileiramento (ANYPS2_VU1_FENCE_ALL=1): a thread vira um
    // inline com troca de contexto.
    void setForceFence(bool on) { forceFence_ = on; }
    // Troca o número de slots do anel (potência de 2, >= 2; ANYPS2_VU1_RING). Só com a fila vazia.
    void setRingSlots(std::size_t slots);
    struct Stats {
        std::uint64_t fenceWaits = 0;  // fences que tiveram de esperar o consumidor
        std::uint64_t ringFull = 0;    // vezes em que o EE esperou espaço no anel
        std::uint64_t publishes = 0;   // stores release de publicação (lotes)
    };
    Stats stats() const { return stats_; }

    // O consumidor publica aqui (gs::GsEventSink).
    void publish(const gs::GsEvent& e) override;

private:
    // Slot do anel (e item do modo adiado): dono dos dados copiados.
    struct Queued {
        explicit Queued(Item::Kind k = Item::Kind::Fence) : item(k) {}
        Item item;
        std::vector<VifQwWrite> writes;
        std::vector<std::uint32_t> words;
        std::vector<std::uint8_t> bytes;
    };

    void push(Item& item);
    void pushQueued(Item& item);
    // O consumidor: aplica e guarda o erro; `q` (o slot do item, se houver) é aparado e só depois
    // o seq é publicado como processado, porque daí o EE pode reaproveitar o slot.
    void consume(const Item& item, Queued* q = nullptr);
    void apply(const Item& item);
    void applyResponses();
    void fenceSlow();
    void rethrowPending();
    void fillSlot(Queued& q, const Item& item);
    void trimSlot(Queued& q);
    Queued& slot(std::uint64_t seq) { return ring_[static_cast<std::size_t>(seq - 1) & ringMask_]; }
    void ensureRing();
    // ---- thread ----
    void startThread();
    void stopThread();
    void consumerMain();
    void publishPending();                                    // store release de pubSeq_ (+ acorda o consumidor)
    void waitProcessed(std::uint64_t target, unsigned part);  // bloqueia o EE até processedSeq_ >= target
    void notifyEe(std::uint64_t seq);                         // consumidor: acorda o EE se ele espera `seq`
    void idleWait(std::uint64_t next);                        // consumidor: espera um item publicado

    static constexpr std::uint64_t kPublishBatch = 32;
    static constexpr std::size_t kDefaultRingSlots = 8192;
    std::size_t ringSlots_ = kDefaultRingSlots;

    Runtime& rt_;
    Gif& gif_;
    gs::Gs& gs_;
    VifExec vif1_;  // executor do VIF1 (dono do VU1/GIF)
    unsigned requestedThread_ = 0;
    bool deferred_ = false;
    bool threaded_ = false;
    bool forceFence_ = false;
    bool path3MaskedEe_ = false;  // espelho do Gif::path3Masked, na ordem de emissão do EE
    // EE: numeração. O seq do item é o índice (1-based) do slot no anel.
    std::uint64_t nextSeq_ = 1;
    // Último seq consumido: escrito pelo consumidor (a thread, ou o EE em inline/adiado).
    std::atomic<std::uint64_t> processedSeq_{0};
    // Último seq publicado ao consumidor: escrito pelo EE.
    std::atomic<std::uint64_t> pubSeq_{0};
    std::vector<Queued> ring_;
    std::size_t ringMask_ = 0;
    // Fila de respostas (consumidor para EE). São poucas por quadro: mutex.
    std::mutex respMutex_;
    std::deque<gs::GsEvent> responses_;
    std::atomic<std::size_t> respCount_{0};
    std::uint64_t respSeq_ = 0;
    // Erro do consumidor: a exceção original (mesma classe, mensagem e pc) e o item que a causou.
    std::atomic<bool> failed_{false};
    std::exception_ptr error_;
    std::uint64_t errorSeq_ = 0;
    // Thread.
    std::unique_ptr<HostThread> thread_;
    std::atomic<bool> stop_{false};
    std::mutex mtx_;
    std::condition_variable consumerCv_, eeCv_;
    std::atomic<bool> consumerSleeping_{false};
    std::atomic<std::uint64_t> eeTarget_{0};  // seq que o EE espera (0 = não espera)
    std::atomic<std::uint64_t> jitterSeed_{0};
    Stats stats_;
};

}  // namespace anyps2::rt
