#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "anyps2/runtime/iop/iso9660.h"

namespace anyps2::rt {

class Iop;

// cdvdman/cdvdfsv em HLE (libcdvd: servidores 0x80000592..0x8000059A).
// O disco é a imagem em ANYPS2_ISO. Leituras são instantâneas (sem tempo de
// busca simulado) e o RTC devolve, no relógio virtual, uma data fixa
// (2000-01-01 00:00:00) mais o tempo emulado — determinístico.
class Cdvd {
public:
    explicit Cdvd(Iop& iop);
    ~Cdvd();
    void registerServers();

    // Disco presente? (ANYPS2_ISO definido.)
    bool hasDisc() const;
    // Abre a imagem na primeira vez; erro claro se não houver disco.
    IsoImage& image(const std::string& why, std::uint32_t pc);

    // Arquivo inteiro de cdrom0: (std::nullopt se não existir).
    std::optional<std::vector<std::uint8_t>> readFile(const std::string& path, std::uint32_t pc);

private:
    std::optional<std::vector<std::uint8_t>> scmd(std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                  std::uint32_t pc);
    std::optional<std::vector<std::uint8_t>> ncmd(std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                  std::uint32_t pc);
    std::vector<std::uint8_t> searchFile(const std::vector<std::uint8_t>& in, std::uint32_t pc);
    std::vector<std::uint8_t> diskReady(std::uint32_t pc);
    std::vector<std::uint8_t> readClock();

    Iop& iop_;
    IsoImage iso_;
    std::string isoPath_;
    std::uint32_t notReadyPolls_ = 0;
};

}  // namespace anyps2::rt
