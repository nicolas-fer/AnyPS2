#pragma once

#include <cstdint>
#include <string>

namespace anyps2::rt {

class Runtime;

// Controlador de DMA do EE (10 canais).
//
// As transferências são executadas de forma síncrona no momento em que
// CHCR.STR é ligado (o destino — GIF/VIF/scratchpad — também processa tudo
// na hora), então do ponto de vista do programa o DMA termina "instantânea-
// mente": STR volta a 0, D_STAT.CIS é marcado e, se CIM permitir, a
// interrupção do DMAC é entregue no próximo safepoint.
//
// Suportado: modo normal e chain de origem (refe/cnt/next/ref/refs/call/
// ret/end, com TTE para os VIFs, IRQ+TIE, pilha ASR0/ASR1) nos canais VIF0,
// VIF1, GIF, toIPU e toSPR; modo normal no fromSPR, no fromIPU e no VIF1 →
// memória (download do GS). SIF por registradores, MFIFO e interleave lançam
// erro dizendo o que faltou.
//
// Exceção à transferência instantânea: se o VIF para (VIFcode com bit I), o
// canal pausa no ponto exato (MADR/QWC/TADR, STR continua ligado) e só
// continua quando o VIF for liberado (resumeChannel, chamado pelo VIF). O
// toIPU (normal e chain) é sob demanda: enche o FIFO do IPU e pausa; o IPU
// chama resumeChannel a cada quadword que consome. O fromIPU leva o que o IPU
// já produziu e pausa até ele produzir mais.
class Dmac {
public:
    static constexpr unsigned kChannels = 10;
    explicit Dmac(Runtime& rt);

    std::uint32_t read(std::uint32_t addr, std::uint32_t pc);
    void write(std::uint32_t addr, std::uint32_t value, std::uint32_t pc);
    // Faixas tratadas: canais (0x1000_8000–0x1000_D4FF), D_CTRL..D_STADR e
    // D_ENABLER/W.
    static bool handles(std::uint32_t addr);
    static const char* channelName(unsigned ch);

    // D_STAT
    std::uint32_t stat() const { return stat_; }
    std::uint32_t pendingChannels() const { return stat_ & (stat_ >> 16) & 0x3FF; }
    void clearChannel(unsigned ch) { stat_ &= ~(1u << ch); }
    bool channelMasked(unsigned ch) const { return (stat_ & (1u << (16 + ch))) == 0; }
    // Liga/desliga o bit CIM do canal (EnableDmac/DisableDmac do kernel).
    // Retorna o estado anterior.
    bool setChannelEnabled(unsigned ch, bool enabled);
    // Sinal CPCOND0 do COP0 (BC0T/BC0F).
    bool cpcond0() const { return ((~pcr_ | stat_) & 0x3FFu) == 0x3FFu; }
    // ResetEE(DMAC)
    void reset();
    // O destino do canal pausado aceita mais (VIF saiu da parada, FIFO do
    // IPU com espaço): o DMA continua.
    void resumeChannel(unsigned ch, std::uint32_t pc);

private:
    struct Channel {
        std::uint32_t chcr = 0, madr = 0, qwc = 0, tadr = 0, asr0 = 0, asr1 = 0, sadr = 0;
        bool pending = false;  // STR ligado com o DMAC suspenso/desabilitado
        bool paused = false;   // STR ligado, esperando o VIF sair da parada
        bool tagEnds = false;  // o tag corrente termina a cadeia (end/refe/ret, IRQ+TIE)
    };

    void start(unsigned ch, std::uint32_t pc);
    // Retornam true se a transferência terminou, false se pausou (VIF parado).
    bool runNormal(unsigned ch, std::uint32_t pc);
    bool runChain(unsigned ch, std::uint32_t pc);
    // Transfere QWC quadwords a partir de MADR (o quanto o destino aceitar) e
    // avança MADR/QWC. Retorna true se tudo foi e o destino não parou.
    bool transferData(unsigned ch, std::uint32_t pc);
    bool deviceStalled(unsigned ch);
    // Retorna quantos quadwords o destino aceitou.
    std::uint32_t sendToDevice(unsigned ch, std::uint32_t addr, std::uint32_t qwc, std::uint32_t pc);
    void finish(unsigned ch);
    std::uint8_t* hostAddress(std::uint32_t dmaAddr, std::uint32_t qwc, unsigned ch, std::uint32_t pc);
    void startPending(std::uint32_t pc);

    Runtime& rt_;
    Channel ch_[kChannels];
    std::uint32_t ctrl_ = 1;  // DMAE ligado (o kernel liga no boot)
    std::uint32_t stat_ = 0;
    std::uint32_t pcr_ = 0, sqwc_ = 0, rbsr_ = 0, rbor_ = 0, stadr_ = 0;
    std::uint32_t enable_ = 0x1201;
};

}  // namespace anyps2::rt
