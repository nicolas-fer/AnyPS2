#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace anyps2::rt {

class Runtime;
class Gif;

// Memórias dos VUs, visíveis no espaço do EE:
//   0x1100_0000  micro VU0 (4 KB)    0x1100_4000  dados VU0 (4 KB)
//   0x1100_8000  micro VU1 (16 KB)   0x1100_C000  dados VU1 (16 KB)
struct VuMemory {
    static constexpr std::uint32_t kVu0Size = 4 * 1024;
    static constexpr std::uint32_t kVu1Size = 16 * 1024;
    std::unique_ptr<std::uint8_t[]> micro0{new std::uint8_t[kVu0Size]()};
    std::unique_ptr<std::uint8_t[]> data0{new std::uint8_t[kVu0Size]()};
    std::unique_ptr<std::uint8_t[]> micro1{new std::uint8_t[kVu1Size]()};
    std::unique_ptr<std::uint8_t[]> data1{new std::uint8_t[kVu1Size]()};
};

// VIF0/VIF1: interpretam o fluxo de VIFcodes vindo do DMA (ou do FIFO).
// Implementado: NOP, STCYCL, OFFSET, BASE, ITOP, STMOD, MSKPATH3, MARK,
// FLUSH/FLUSHE/FLUSHA, STMASK, STROW, STCOL, MPG, DIRECT/DIRECTHL (PATH2) e
// UNPACK (todos os formatos, com máscara, modos offset/difference e
// escrita com salto). Execução de microprogramas (MSCAL/MSCALF/MSCNT)
// chega na Fase 5 e hoje lança erro dizendo o endereço do microprograma.
class Vif {
public:
    Vif(Runtime* rt, unsigned unit, VuMemory& vu, Gif* gif);

    // Processa dados (múltiplo de 4 bytes) vindos do DMA/FIFO.
    void transfer(const std::uint8_t* data, std::size_t bytes, std::uint32_t pc);
    // DMA com TTE: os 64 bits altos do DMAtag passam pelo VIF.
    void transferTag(std::uint64_t upper, std::uint32_t pc);

    std::uint32_t readRegister(std::uint32_t addr, std::uint32_t pc);
    void writeRegister(std::uint32_t addr, std::uint32_t value, std::uint32_t pc);
    void reset();
    bool idle() const { return state_ == State::Idle; }

private:
    enum class State { Idle, Mask, Row, Col, Mpg, Direct, Unpack };

    void word(std::uint32_t w, std::uint32_t pc);
    void command(std::uint32_t w, std::uint32_t pc);
    void unpackWord(std::uint32_t w, std::uint32_t pc);
    void writeUnpackedVector(const std::uint32_t (&v)[4], std::uint32_t pc);
    std::uint8_t* dataMem() const;
    std::uint32_t dataSize() const;
    std::uint8_t* microMem() const;
    std::uint32_t microSize() const;

    Runtime* rt_;
    unsigned unit_;
    VuMemory& vu_;
    Gif* gif_;

    State state_ = State::Idle;
    std::uint32_t code_ = 0;
    std::uint32_t remaining_ = 0;  // palavras (ou quadwords no DIRECT) restantes
    unsigned index_ = 0;
    // MPG
    std::uint32_t mpgAddr_ = 0;
    // DIRECT
    std::uint8_t directBuf_[16] = {};
    unsigned directFill_ = 0;
    // UNPACK
    unsigned upVn_ = 0, upVl_ = 0;
    bool upMask_ = false, upUsn_ = false;
    std::uint32_t upAddr_ = 0;     // quadword de destino base
    std::uint32_t upNum_ = 0;      // vetores restantes
    std::uint32_t upIndex_ = 0;    // vetores já escritos
    std::uint32_t upComp_[4] = {};  // componentes já lidos do vetor corrente
    unsigned upNbits_ = 0;
    std::uint32_t upWordsLeft_ = 0;

    // Registradores
    std::uint32_t stat_ = 0, err_ = 0, mark_ = 0, cycle_ = 0, mode_ = 0, num_ = 0, mask_ = 0;
    std::uint32_t itops_ = 0, base_ = 0, ofst_ = 0, tops_ = 0, itop_ = 0, top_ = 0;
    std::uint32_t row_[4] = {}, col_[4] = {};
};

}  // namespace anyps2::rt
