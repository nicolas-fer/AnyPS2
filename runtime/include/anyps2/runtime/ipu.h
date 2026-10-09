#pragma once

#include <array>
#include <cstdint>

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
// INTC (8).
//
// Implementado: BCLR, FDEC, SETIQ, SETVQ e SETTH. IDEC, BDEC, VDEC, CSC e
// PACK lançam erro dizendo o que falta.
class Ipu {
public:
    explicit Ipu(Runtime* rt);

    // Registradores 0x1000_2000–0x1000_2FFF (repetem a cada 0x100).
    std::uint64_t readRegister(std::uint32_t addr, std::uint32_t pc);
    void writeRegister(std::uint32_t addr, std::uint32_t value, std::uint32_t pc);
    // Escrita de um quadword no FIFO de entrada (0x1000_7010). Retorna false se
    // o FIFO estava cheio (o dado é descartado, como no hardware).
    bool fifoWrite(const std::uint8_t (&qw)[16], std::uint32_t pc);
    // Leitura de um quadword do FIFO de saída (0x1000_7000).
    void fifoRead(std::uint8_t (&qw)[16], std::uint32_t pc);
    void reset();

    bool busy() const { return busy_; }
    unsigned inputCount() const { return ifc_; }
    const std::array<std::uint8_t, 64>& intraQuant() const { return iq_; }
    const std::array<std::uint8_t, 64>& nonIntraQuant() const { return niq_; }
    const std::array<std::uint16_t, 16>& vqClut() const { return vqclut_; }
    std::uint16_t threshold(unsigned i) const { return th_[i & 1]; }

private:
    static constexpr unsigned kFifoQw = 8;

    // ---- Fluxo de bits ------------------------------------------------------
    // Garante `bits` bits a partir de BP no buffer interno, puxando do FIFO.
    bool fill(unsigned bits);
    // Próximos `bits` (1–32) bits sem consumir; exige fill(bits).
    std::uint32_t peek(unsigned bits) const;
    // Consome `bits` (exige fill(bits)).
    void advance(unsigned bits);
    bool popFifo(std::uint8_t (&qw)[16]);

    void startCommand(std::uint32_t cmd, std::uint32_t pc);
    // Avança o comando corrente o quanto os dados permitirem.
    void run(std::uint32_t pc);
    void finishCommand();
    void softReset();

    Runtime* rt_;

    std::uint8_t internal_[32] = {};  // 2 quadwords seguidos
    unsigned bp_ = 0;  // 0..127 no primeiro quadword
    unsigned fp_ = 0;  // quadwords carregados no buffer interno (0..2)
    std::uint8_t fifo_[kFifoQw][16] = {};
    unsigned fifoRead_ = 0;
    unsigned ifc_ = 0;

    std::uint8_t outFifo_[kFifoQw][16] = {};
    unsigned outRead_ = 0;
    unsigned ofc_ = 0;

    std::uint32_t ctrl_ = 0;  // bits graváveis e flags (IFC/OFC/BUSY montados na leitura)
    std::uint32_t cmd_ = 0;   // comando corrente (ou o último)
    std::uint32_t data_ = 0;  // IPU_CMD.DATA (resultado do FDEC/VDEC)
    std::uint32_t top_ = 0;
    bool busy_ = false;
    bool running_ = false;
    bool skipped_ = false;    // FB do comando corrente já pulado
    unsigned pos_ = 0;        // progresso do comando corrente

    std::array<std::uint8_t, 64> iq_{};
    std::array<std::uint8_t, 64> niq_{};
    std::array<std::uint16_t, 16> vqclut_{};
    std::uint16_t th_[2] = {};
};

}  // namespace anyps2::rt
