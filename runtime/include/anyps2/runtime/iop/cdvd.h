#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace anyps2::rt {

class Iop;

// Imagem de disco ISO 9660 (dump próprio do usuário). Aceita imagens de
// setores de 2048 bytes (.iso) e de setores brutos de 2352 bytes (.bin,
// modo 1 ou modo 2 form 1). Só a parte ISO 9660 é lida: sem Joliet/UDF, que
// os discos de PS2 não exigem.
class IsoImage {
public:
    static constexpr std::uint32_t kSectorSize = 2048;

    struct Entry {
        std::string name;  // como gravado no disco ("SYSTEM.CNF;1")
        std::uint32_t lsn = 0, size = 0;
        bool isDir = false;
        std::uint8_t flags = 0;
        std::array<std::uint8_t, 7> date{};  // ano-1900, mês, dia, hora, min, seg, fuso
    };

    IsoImage() = default;
    ~IsoImage();
    IsoImage(const IsoImage&) = delete;
    IsoImage& operator=(const IsoImage&) = delete;

    // Abre e valida (volume primário no setor 16). Lança anyps2::Error.
    void open(const std::string& path);
    bool isOpen() const { return fp_ != nullptr; }
    const std::string& path() const { return path_; }
    std::uint32_t sectorCount() const { return sectors_; }

    // Lê `count` setores de 2048 bytes. false se passar do fim da imagem.
    bool readSectors(std::uint32_t lsn, std::uint32_t count, std::uint8_t* dst);
    // Lê bytes de um arquivo (lsn inicial + deslocamento).
    bool readBytes(std::uint32_t lsn, std::uint64_t offset, std::uint32_t size, std::uint8_t* dst);

    // Procura "\\DIR\\ARQ.EXT;1" (aceita '/' e omissão do ";1"; sem
    // distinção de maiúsculas). Caminho vazio ou "\\" = raiz.
    std::optional<Entry> lookup(const std::string& path);
    // Entradas de um diretório (sem "." e "..").
    std::vector<Entry> list(const Entry& dir);
    const Entry& root() const { return root_; }

private:
    std::FILE* fp_ = nullptr;
    std::string path_;
    std::uint32_t rawSize_ = 2048, dataOffset_ = 0, sectors_ = 0;
    Entry root_;
};

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
