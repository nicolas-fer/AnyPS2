#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace anyps2::rt::gs {

// Eventos do GS visíveis ao EE (fatia 4 do plano do VU1 em outra thread). O consumidor (dono
// do GIF e do Gs como produtor) nunca altera o CSR/IMR/SIGLBLID do lado do EE nem pede a
// interrupção do INTC: ele PUBLICA eventos (GsEvent) numa fila de respostas, e o EE os aplica
// num espelho dele (GsEeState) no ponto em que hoje a interrupção acontece. Com o horizonte
// W = 0 a fila é esvaziada logo depois de cada item, então o tempo é o de antes da separação;
// com W > 0 (fatia 6) cada resposta vale em `at` = carimbo do item + W (ver Vu1Stream).

// Bits de evento do CSR.
inline constexpr unsigned kCsrSignalBit = 0, kCsrFinishBit = 1, kCsrHsintBit = 2, kCsrVsintBit = 3;
inline constexpr std::uint64_t kCsrFieldMask = 1ull << 13;
inline constexpr std::uint64_t kCsrResetMask = 1ull << 9;
// ID 0x55, revisão 0x1B, FIFO vazio (bits 14–15 = 01).
inline constexpr std::uint64_t kCsrConstBits = 0x551B0000ull | (1ull << 14);

struct GsEvent {
    enum class Kind : std::uint8_t {
        Raise,      // liga o bit `bit` do CSR (e pede a interrupção se o IMR deixar)
        SigLblId,   // SIGLBLID = (SIGLBLID & ~mask) | value (SIGNAL e LABEL)
        FinishDue,  // um FINISH que passa a valer em `due` (ciclos do EE)
        Vblank,     // início do VBlank: inverte o campo e liga VSINT
        Reset,      // reset do GS (CSR.RESET): zera os bits de evento e os FINISH pendentes
    };
    Kind kind = Kind::Raise;
    unsigned bit = 0;
    std::uint64_t mask = 0, value = 0;
    // Ciclos do EE em que o efeito vale: o carimbo do item (+ W no runtime). Para o FinishDue é o
    // instante em que o FINISH dispara (max(fim do trabalho do GS, `at`)).
    std::uint64_t due = 0;
    // Ciclos do EE a partir dos quais o EE aplica a resposta no espelho: carimbo do item + W
    // (preenchido pelo Vu1Stream; o Gs isolado aplica na hora e ignora). A ordem de aplicação é a
    // da fila (`seq`); `at` não decresce nela.
    std::uint64_t at = 0;
    std::uint64_t seq = 0;  // numeração da fila de respostas (a ordem de aplicação)
};

// Para onde o Gs manda os eventos que gera. Nulo = o Gs aplica no próprio espelho na hora
// (testes do GS isolado, e o comportamento de antes da separação).
class GsEventSink {
public:
    virtual ~GsEventSink() = default;
    virtual void publish(const GsEvent& e) = 0;
};

// Espelho do EE: CSR (bits de evento e campo), IMR, SIGLBLID e os FINISH pendentes. Só a thread
// do EE o toca. A interrupção do INTC sai daqui, no apply.
class GsEeState {
public:
    using IrqFn = void (*)(void* ctx);
    void setIrq(IrqFn fn, void* ctx) {
        irq_ = fn;
        irqCtx_ = ctx;
    }
    // Modelo de tempo do HSINT (ciclos do EE por linha); sem isto não há HSINT.
    void setHsyncSource(std::uint64_t cyclesPerLine) {
        hasClock_ = true;
        cyclesPerLine_ = cyclesPerLine;
    }
    // Relógio real: os FINISH são entregues um por vez (ver processEvents).
    void setRealTimeClock(bool realTime) { realTime_ = realTime; }

    void apply(const GsEvent& e);

    // CSR como o EE o lê em `now` (o HSINT vem da linha de `now`).
    std::uint64_t csr(std::uint64_t now);
    // Escrita do EE no CSR sem o bit RESET: escrever 1 limpa os bits de evento.
    void writeCsr(std::uint64_t value, std::uint64_t now);
    std::uint64_t imr() const { return imr_; }
    void setImr(std::uint64_t v) { imr_ = v & 0x7F00; }
    std::uint64_t siglblid() const { return siglblid_; }
    void setSigLblId(std::uint64_t v) { siglblid_ = v; }

    // Ciclo do EE do próximo FINISH pendente, ~0 se nenhum.
    std::uint64_t nextEventTime() const;
    void processEvents(std::uint64_t now);
    std::size_t pendingFinishes() const { return finishDue_.size(); }

private:
    void raise(unsigned bit);

    std::uint64_t csr_ = 0;
    std::uint64_t imr_ = 0x7F00;
    std::uint64_t siglblid_ = 0;
    bool hasClock_ = false;
    bool realTime_ = false;
    std::uint64_t cyclesPerLine_ = 0;
    std::uint64_t hsyncClearedAt_ = 0;
    std::vector<std::uint64_t> finishDue_;  // FINISH pendentes (ordem crescente)
    IrqFn irq_ = nullptr;
    void* irqCtx_ = nullptr;
};

}  // namespace anyps2::rt::gs
