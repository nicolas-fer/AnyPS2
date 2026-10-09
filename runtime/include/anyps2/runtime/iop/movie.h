#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace anyps2::rt::movie {

// MPEG-2 Program Stream lido em sequência (vídeos dos servidores MPG1/MPG2
// do PDISTR.IRX): pula pack headers, system header, áudio e padding e
// devolve os pacotes de vídeo (stream 0xE0).
class ProgramStream {
public:
    // Lê `n` bytes a partir de `pos` (relativo ao início do stream).
    using Reader = std::function<bool(std::uint64_t pos, std::uint32_t n, std::uint8_t* dst)>;
    // loop: no fim, volta ao começo (vídeos de fundo, flag 0x10 do MPG1) — a
    // não ser que a passada inteira não tenha tido nenhum pacote de vídeo.
    ProgramStream(Reader read, std::uint64_t size, bool loop = false)
        : read_(std::move(read)), size_(size), loop_(loop) {}

    // Próximo pacote de vídeo: o que vem depois do campo de tamanho do
    // cabeçalho PES (extensão do cabeçalho + dados). false no fim do stream
    // (código 0x1B9, fim dos dados ou algo que não é start code).
    bool nextVideo(std::vector<std::uint8_t>& payload, std::uint32_t pc);
    std::uint64_t position() const { return pos_; }
    bool ended() const { return ended_; }

private:
    bool read(std::uint32_t n, std::uint8_t* dst);

    Reader read_;
    std::uint64_t size_;
    std::uint64_t pos_ = 0;
    bool loop_;
    bool ended_ = false;
    bool videoThisPass_ = false;
};

// Tamanho da fatia que o fn 2 do MPG1 enche com um pacote de vídeo.
inline constexpr std::uint32_t kSlotSize = 5120;

// Monta a fatia: registro {bytes, desalinhamento 0, bytes que faltam, 0}, os
// dados (múltiplo de 16) e um registro zerado. Sem pacote, só o registro
// zerado (fim do stream). Retorna quantos bytes foram escritos; erro se o
// pacote não couber.
std::uint32_t buildSlot(const std::vector<std::uint8_t>* payload, std::uint8_t (&slot)[kSlotSize], std::uint32_t pc);

}  // namespace anyps2::rt::movie
