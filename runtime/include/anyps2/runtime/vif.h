#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "anyps2/runtime/vif_exec.h"

namespace anyps2::rt {

class Runtime;
class Gif;
class Vu1Stream;

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
//
// Duas camadas (fatia 3 do plano do VU1 em outra thread). A DECODIFICAÇÃO é esta classe: a
// máquina de estados dos VIFcodes e todo o estado que o EE enxerga (STAT, FQC, MARK, DBF, TOPS,
// TOP, ITOP, CYCLE, MASK, ROW, COL, MODE, bit I, FIFO, bytes consumidos do DMA). Ela roda no EE e
// não toca a memória do VU, não chama o VU nem o GIF: produz itens (vif_exec.h). A EXECUÇÃO é o
// VifExec, que aplica os itens (memória do VU, MSCAL, GIF) e hoje roda inline, na hora e na mesma
// ordem. A separação vale para as duas unidades; só o VIF1 gera Direct e MSKPATH3.
// Implementado: NOP, STCYCL, OFFSET, BASE, ITOP, STMOD, MSKPATH3, MARK,
// FLUSH/FLUSHE/FLUSHA, STMASK, STROW, STCOL, MPG, DIRECT/DIRECTHL (PATH2) e
// UNPACK (todos os formatos, com máscara, modos offset/difference e
// escrita com salto) e MSCAL/MSCALF/MSCNT, que executam o microprograma do
// VU correspondente na hora (com o double buffering TOPS/BASE/OFST do VIF1).
//
// VIFcode com bit I (sem ERR.MII): ao fim do comando o VIF liga STAT.VIS e
// STAT.INT, pede a interrupção do INTC (VIF0 = 4, VIF1 = 5) e para; o resto
// do quadword fica no FIFO e o DMA do canal pausa. FBRST.STC libera: o FIFO
// é processado e o DMA continua.
class Vif {
public:
    Vif(Runtime* rt, unsigned unit, VuMemory& vu, Gif* gif);

    // Com um stream, os itens vão para a fila ordenada do VU1 (vu1_stream.h) em vez de ao
    // executor próprio: é como o VIF1 funciona no runtime. Sem ele (o VIF0, que fica no EE, e os
    // testes) o executor próprio os aplica na hora.
    void setStream(Vu1Stream* stream) { stream_ = stream; }

    // Processa dados (múltiplo de 4 bytes) vindos do DMA. Retorna quantos
    // bytes foram aceitos: tudo, ou — se o VIF parou — até o fim do quadword
    // em que parou (o resto dele fica no FIFO); 0 se já estava parado.
    std::size_t transfer(const std::uint8_t* data, std::size_t bytes, std::uint32_t pc);
    // Escrita no VIFn_FIFO pelo EE: com o VIF parado, os dados esperam no FIFO.
    void fifoWrite(const std::uint8_t* data, std::size_t bytes, std::uint32_t pc);
    // DMA com TTE: os 64 bits altos do DMAtag passam pelo VIF.
    void transferTag(std::uint64_t upper, std::uint32_t pc);
    // Parado (bit I, STOP ou ForceBreak) até FBRST.STC.
    bool stalled() const;

    std::uint32_t readRegister(std::uint32_t addr, std::uint32_t pc);
    void writeRegister(std::uint32_t addr, std::uint32_t value, std::uint32_t pc);
    void reset();
    // Entrega ao executor o lote de UNPACK/MPG acumulado (uso interno de transfer e resume).
    void flushBatches() { flush(); }
    bool idle() const { return state_ == State::Idle; }
    std::uint32_t top() const { return top_; }
    std::uint32_t itop() const { return itop_; }

private:
    enum class State { Idle, Mask, Row, Col, Mpg, Direct, Unpack };

    void word(std::uint32_t w, std::uint32_t pc);
    void command(std::uint32_t w, std::uint32_t pc);
    void unpackWord(std::uint32_t w);
    void writeUnpackedVector(const std::uint32_t (&v)[4]);
    void writeFillVectors();
    // Entrega ao executor o que está acumulado (UNPACK e MPG saem em lotes) e depois o item.
    void emit(const VifItem& item);
    void flush();

    Runtime* rt_;
    unsigned unit_;
    VifExec exec_;
    Vu1Stream* stream_ = nullptr;
    void deliver(const VifItem& item);
    std::uint32_t dataQwords_;  // quadwords da memória de dados da unidade

    State state_ = State::Idle;
    std::uint32_t code_ = 0;
    // Últimos VIFcodes aceitos, para a mensagem de VIFcode inválido.
    std::uint32_t history_[8] = {};
    unsigned historyPos_ = 0;
    bool irqPending_ = false;           // comando corrente tem o bit I
    std::vector<std::uint8_t> fifo_;    // dados recebidos e ainda não processados (parado)
    void stallOnIrq();
    void resume(std::uint32_t pc);
    std::uint32_t remaining_ = 0;  // palavras (ou quadwords no DIRECT) restantes
    unsigned index_ = 0;
    // MPG
    std::uint32_t mpgAddr_ = 0;
    std::vector<std::uint32_t> mpgBatch_;  // palavras ainda não entregues, a partir de mpgBatchAddr_
    std::uint32_t mpgBatchAddr_ = 0;
    std::vector<VifQwWrite> upBatch_;      // quadwords do UNPACK ainda não entregues
    // DIRECT
    std::uint8_t directBuf_[16] = {};
    unsigned directFill_ = 0;
    // UNPACK
    unsigned upVn_ = 0, upVl_ = 0;
    bool upMask_ = false, upUsn_ = false;
    std::uint32_t upAddr_ = 0;     // quadword de destino base
    std::uint32_t upNum_ = 0;      // vetores restantes a gravar
    std::uint32_t upReadLeft_ = 0;  // vetores restantes a ler do stream
    bool upFill_ = false;          // escrita de preenchimento (CL < WL)
    unsigned upCl_ = 0, upWl_ = 0;
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
