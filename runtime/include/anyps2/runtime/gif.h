#pragma once

#include <cstddef>
#include <cstdint>

namespace anyps2::rt {

class Runtime;
namespace gs {
class Gs;
}

// GIF: recebe pacotes (GIFtag + dados) pelos três caminhos e os transforma
// em escritas nos registradores do GS.
//   PATH1 — XGKICK do VU1 (Fase 5)
//   PATH2 — DIRECT/DIRECTHL do VIF1
//   PATH3 — DMA do canal GIF ou escrita direta no GIF_FIFO
// Modos de GIFtag: PACKED (descritores de registrador, incluindo A+D),
// REGLIST e IMAGE (dados de transferência HOST→LOCAL).
class Gif {
public:
    Gif(Runtime* rt, gs::Gs& gs);

    // Processa `qwords` quadwords (16 bytes cada) no caminho `path` (1–3).
    void transfer(unsigned path, const std::uint8_t* data, std::size_t qwords, std::uint32_t pc);
    // XGKICK: envia pelo PATH1 os pacotes a partir de addr (bytes) na memória
    // de dados do VU1 até terminar um pacote com EOP.
    void kick(const std::uint8_t* mem, std::uint32_t size, std::uint32_t addr, std::uint32_t pc);

    std::uint32_t readRegister(std::uint32_t addr, std::uint32_t pc);
    void writeRegister(std::uint32_t addr, std::uint32_t value, std::uint32_t pc);
    void reset();
    // PATH3 mascarado pelo VIF1 (MSKPATH3).
    void setPath3Masked(bool masked) { path3Masked_ = masked; }
    bool path3Masked() const { return path3Masked_; }

private:
    struct Path {
        bool active = false;  // dentro de um pacote (depois do GIFtag)
        std::uint32_t nloop = 0;
        unsigned nreg = 0;
        unsigned reg = 0;     // próximo descritor
        unsigned flg = 0;
        bool eop = false;
        std::uint64_t regs = 0;
        std::uint64_t tag[2] = {0, 0};
    };

    void writePacked(unsigned desc, std::uint64_t lo, std::uint64_t hi, std::uint32_t pc);
    void writeReglist(unsigned desc, std::uint64_t data, std::uint32_t pc);

    Runtime* rt_;
    gs::Gs& gs_;
    Path paths_[3];
    std::uint32_t q_ = 0x3F800000u;  // Q interno (bits de float), vindo do ST
    std::uint32_t ctrl_ = 0, mode_ = 0;
    bool path3Masked_ = false;
};

}  // namespace anyps2::rt
