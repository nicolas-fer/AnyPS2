#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <vector>

#include "anyps2/runtime/gs/gs_events.h"
#include "anyps2/runtime/vif_exec.h"

namespace anyps2::rt {

class Runtime;
class Gif;
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
// Nesta fatia o consumidor roda INLINE: cada push aplica o item na hora, com os dados emprestados
// do chamador (sem cópia), esvazia a fila de respostas e relança o erro, se houve. O tempo é, por
// isso, o mesmo de antes da separação. O modo adiado (setDeferred, para testes) copia os dados no
// enfileiramento e só consome no fence, e mostra o que a thread de verdade precisará: o que o EE
// reescreve depois do push não muda o que o GS recebe.
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
//   - HostProfile e GsTrace/VuTrace: contadores e diagnóstico do consumidor, que quando houver
//     outra thread precisam ser por thread ou lidos depois de um fence (fatia 5).
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
    // relança o erro guardado. Inline: só relança (e consome o que estiver adiado).
    void fence() {
        if (!queue_.empty() || !responses_.empty() || failed_) fenceSlow();
    }
    std::uint64_t readGsPrivileged(std::uint32_t addr, std::uint32_t pc);
    // CSR como o EE o lê em `now` (sem contar como leitura privilegiada).
    std::uint64_t csr(std::uint64_t now);
    std::uint64_t imr();
    void setImr(std::uint64_t v) { fence(); eeState().setImr(v); }
    std::uint32_t readGif(std::uint32_t addr, std::uint32_t pc);
    bool path3Masked();
    // Espelho do EE (sem fence: quem lê estado que o consumidor altera chama fence antes).
    gs::GsEeState& eeState();
    std::uint64_t nextEventTime();
    void processEvents(std::uint64_t now);

    // ---- Modo e diagnóstico -------------------------------------------------------------
    // ANYPS2_VU1_THREAD pedido pelo ambiente: 0 = inline; 1 ainda não existe (fatia 5) e cai
    // no inline com um aviso.
    unsigned requestedThread() const { return requestedThread_; }
    bool inlineMode() const { return !deferred_; }
    // Só para testes: enfileira com cópia e consome no fence (ou em drain), em vez de na hora.
    void setDeferred(bool on);
    // Consome tudo o que está na fila (o consumidor, quando adiado). Não relança erro.
    void drain();
    std::uint64_t pushedSeq() const { return nextSeq_ - 1; }
    std::uint64_t processedSeq() const { return processedSeq_; }
    std::size_t queued() const { return queue_.size(); }
    // Há erro guardado do consumidor, ainda não relançado no EE.
    bool hasPendingError() const { return failed_; }
    // seq do item que falhou (só vale com hasPendingError).
    std::uint64_t pendingErrorSeq() const { return errorSeq_; }

    // O consumidor publica aqui (gs::GsEventSink).
    void publish(const gs::GsEvent& e) override;

private:
    // Item enfileirado (adiado): dono dos dados.
    struct Queued {
        explicit Queued(Item::Kind k) : item(k) {}
        Item item;
        std::vector<VifQwWrite> writes;
        std::vector<std::uint32_t> words;
        std::vector<std::uint8_t> bytes;
    };

    void push(Item& item);
    void consume(const Item& item);  // o consumidor: aplica e guarda o erro
    void apply(const Item& item);
    void applyResponses();
    void fenceSlow();
    void rethrowPending();
    void enqueueCopy(const Item& item);

    Runtime& rt_;
    Gif& gif_;
    gs::Gs& gs_;
    VifExec vif1_;  // executor do VIF1 (dono do VU1/GIF)
    unsigned requestedThread_ = 0;
    bool deferred_ = false;
    std::uint64_t nextSeq_ = 1, processedSeq_ = 0, respSeq_ = 0;
    std::deque<Queued> queue_;
    std::deque<gs::GsEvent> responses_;
    // Erro do consumidor: a exceção original (mesma classe, mensagem e pc) e o item que a causou.
    bool failed_ = false;
    std::exception_ptr error_;
    std::uint64_t errorSeq_ = 0;
};

}  // namespace anyps2::rt
