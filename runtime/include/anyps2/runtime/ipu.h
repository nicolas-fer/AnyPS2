#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace anyps2::rt {

class Runtime;

// IPU (Image Processing Unit): decodificador de MPEG-2 do EE.
//
// Registradores em 0x1000_2000 (CMD, CTRL, BP, TOP), FIFO de entrada em
// 0x1000_7010 (escrita de 128 bits) e de saída em 0x1000_7000.
//
// O fluxo de bits vem do FIFO de entrada (8 quadwords) através de um buffer
// interno de 2 quadwords: BP é a posição em bits dentro do primeiro, FP
// quantos estão carregados e IFC quantos esperam no FIFO — os três aparecem
// em IPU_BP e os programas os usam para saber quanto do fluxo foi consumido.
//
// Um comando que precisa de mais dados do que há fica ocupado (CTRL.BUSY) e
// continua quando chegam mais; ao terminar, pede a interrupção do IPU no
// INTC (8). Os de decodificação (VDEC, BDEC) são reiniciáveis: se os dados
// acabam no meio, o estado volta ao do começo do comando, os quadwords já
// lidos voltam para a frente do FIFO e o comando recomeça quando o DMA
// trouxer mais — o resultado é o mesmo de decodificar aos poucos.
//
// DMA toIPU (canal 4): sob demanda, como no hardware. O canal só enche o
// FIFO (8 quadwords) e fica pausado com STR ligado; cada quadword que o IPU
// tira do FIFO puxa mais do canal. Assim MADR/QWC/TADR + IFC/FP/BP dizem
// exatamente quanto do fluxo foi consumido (a libmpeg usa isso).
//
// Saída: o comando grava numa fila; OFC mostra até 8 quadwords e o DMA
// fromIPU (canal 3) ou leituras de 0x1000_7000 a esvaziam. O comando só deixa
// de estar ocupado quando o que sobra cabe no FIFO de saída (8 quadwords).
//
// Implementado: BCLR, VDEC, BDEC (saída RAW16), FDEC, SETIQ, SETVQ e SETTH.
// IDEC, CSC e PACK lançam erro dizendo o que falta; MPEG-1 (CTRL.MP1) no
// BDEC também.
class Ipu {
public:
    explicit Ipu(Runtime* rt);

    // Registradores 0x1000_2000–0x1000_2FFF (repetem a cada 0x100).
    std::uint64_t readRegister(std::uint32_t addr, std::uint32_t pc);
    void writeRegister(std::uint32_t addr, std::uint32_t value, std::uint32_t pc);
    // Escrita de um quadword no FIFO de entrada (0x1000_7010). Retorna false se
    // o FIFO estava cheio (o dado é descartado, como no hardware).
    bool fifoWrite(const std::uint8_t (&qw)[16], std::uint32_t pc);
    // Leitura de um quadword do FIFO de saída (0x1000_7000 ou DMA fromIPU).
    // FIFO vazio: zeros e false.
    bool fifoRead(std::uint8_t (&qw)[16], std::uint32_t pc);
    // DMA toIPU: aceita até o FIFO encher; retorna quantos quadwords entraram.
    // Não executa comandos (o DMAC chama kick() ao fim da transferência).
    std::uint32_t dmaWrite(const std::uint8_t* data, std::uint32_t qwc);
    // Quadwords prontos para o DMA fromIPU.
    std::size_t outputCount() const { return out_.size(); }
    // Continua o comando que esperava dados, se houver.
    void kick(std::uint32_t pc);
    void reset();

    bool busy() const { return busy_; }
    const std::array<std::uint8_t, 64>& intraQuant() const { return iq_; }
    const std::array<std::uint8_t, 64>& nonIntraQuant() const { return niq_; }
    const std::array<std::uint16_t, 16>& vqClut() const { return vqclut_; }
    std::uint16_t threshold(unsigned i) const { return th_[i & 1]; }

private:
    static constexpr unsigned kFifoQw = 8;
    using Qword = std::array<std::uint8_t, 16>;
    struct NeedData {};  // os dados acabaram no meio de um comando reiniciável

    // ---- Fluxo de bits ------------------------------------------------------
    // Garante `bits` (até 64) bits a partir de BP no buffer interno, puxando
    // do FIFO.
    bool fill(unsigned bits);
    // Próximos `bits` (1–32) bits sem consumir; exige fill(bits).
    std::uint32_t peek(unsigned bits) const;
    // Consome `bits` (exige fill(bits)).
    void advance(unsigned bits);
    bool popFifo(Qword& qw);
    // O FIFO tem espaço: o DMA toIPU pausado continua.
    void requestData();
    // Para os comandos reiniciáveis: sem dados, lançam NeedData.
    std::uint32_t show(unsigned bits);
    std::uint32_t get(unsigned bits);
    void skip(unsigned bits);
    unsigned inputCount() const;

    // ---- Comandos -----------------------------------------------------------
    void startCommand(std::uint32_t cmd, std::uint32_t pc);
    // Avança o comando corrente o quanto os dados permitirem.
    void run(std::uint32_t pc);
    void finishCommand();
    void softReset();
    // Comandos reiniciáveis (NeedData se faltar dado).
    void runVdec(std::uint32_t pc);
    void runBdec(std::uint32_t pc);
    void intraBlock(unsigned cc, std::int32_t (&coef)[64], int quantizerScale);
    void nonIntraBlock(std::int32_t (&coef)[64], int quantizerScale);
    // Depois de um macrobloco: start code à frente liga SCD (outro dado, ECD).
    void checkStartCode();
    void pushOutput(const std::uint8_t* data, std::size_t qwords);

    Runtime* rt_;
    std::uint32_t pc_ = 0;  // PC do acesso corrente (erros do DMA puxado pelo IPU)

    std::uint8_t internal_[32] = {};  // 2 quadwords seguidos
    unsigned bp_ = 0;  // 0..127 no primeiro quadword
    unsigned fp_ = 0;  // quadwords carregados no buffer interno (0..2)
    std::deque<Qword> fifo_;
    std::deque<Qword> out_;

    // Instantâneo do começo do comando reiniciável e o que ele já tirou do FIFO.
    struct Snapshot {
        std::uint8_t internal[32];
        unsigned bp, fp;
        int dcPred[3];
        std::uint32_t ctrl;  // ECD/SCD/CBP só valem quando o comando termina
    };
    Snapshot snap_{};
    std::deque<Qword> popped_;
    bool restartable_ = false;

    std::uint32_t ctrl_ = 0;  // bits graváveis e flags (IFC/OFC/BUSY montados na leitura)
    std::uint32_t cmd_ = 0;   // comando corrente (ou o último)
    std::uint32_t data_ = 0;  // IPU_CMD.DATA (resultado do FDEC/VDEC)
    std::uint32_t top_ = 0;
    bool busy_ = false;
    bool running_ = false;
    bool waitingOutput_ = false;  // decodificado; esperando a saída caber no FIFO
    unsigned pos_ = 0;            // progresso do comando corrente (FDEC/SETIQ/SETVQ)
    int dcPred_[3] = {};

    std::array<std::uint8_t, 64> iq_{};
    std::array<std::uint8_t, 64> niq_{};
    std::array<std::uint16_t, 16> vqclut_{};
    std::uint16_t th_[2] = {};
};

}  // namespace anyps2::rt
