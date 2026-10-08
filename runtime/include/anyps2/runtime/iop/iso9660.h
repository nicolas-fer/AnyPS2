#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace anyps2::rt {

// Imagem de disco ISO 9660 (dump próprio do usuário). Aceita imagens de
// setores de 2048 bytes (.iso) e de setores brutos de 2352 bytes (.bin,
// modo 1 ou modo 2 form 1). Só a parte ISO 9660 é lida: sem Joliet/UDF, que
// os discos de PS2 não exigem.
//
// DVDs de camada dupla (DVD-9) têm um segundo volume ISO 9660 na camada 1:
// o descritor primário dela fica 16 setores depois da base da camada, e os
// LSN gravados nos registros da camada 1 são relativos a essa base. Aqui
// todo `Entry::lsn` é absoluto (já somado à base), então readSectors/
// readBytes funcionam igual nas duas camadas.
class IsoImage {
public:
    static constexpr std::uint32_t kSectorSize = 2048;

    struct Entry {
        std::string name;  // como gravado no disco ("SYSTEM.CNF;1")
        std::uint32_t lsn = 0, size = 0;  // lsn absoluto na imagem
        bool isDir = false;
        std::uint8_t flags = 0;
        std::uint8_t layer = 0;
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
    // Identificador do volume primário (sem os espaços finais).
    const std::string& volumeId() const { return volumeId_; }

    // 2 se a imagem é um DVD-9 com o volume da camada 1 achado; senão 1.
    unsigned layerCount() const { return layer1Base_ ? 2u : 1u; }
    // Setor da imagem onde a camada 1 começa (base dos LSN dela).
    std::optional<std::uint32_t> layer1Start() const { return layer1Base_; }

    // Lê `count` setores de 2048 bytes. false se passar do fim da imagem.
    bool readSectors(std::uint32_t lsn, std::uint32_t count, std::uint8_t* dst);
    // Lê bytes de um arquivo (lsn inicial + deslocamento).
    bool readBytes(std::uint32_t lsn, std::uint64_t offset, std::uint32_t size, std::uint8_t* dst);

    // Procura "\\DIR\\ARQ.EXT;1" (aceita '/' e omissão do ";1"; sem
    // distinção de maiúsculas) no volume da camada `layer`. Caminho vazio ou
    // "\\" = raiz.
    std::optional<Entry> lookup(const std::string& path, unsigned layer = 0);
    // Entradas de um diretório (sem "." e "..").
    std::vector<Entry> list(const Entry& dir);
    const Entry& root(unsigned layer = 0) const { return layer == 1 && layer1Base_ ? root1_ : root_; }

private:
    void findLayer1(std::uint32_t volumeSpace);

    std::FILE* fp_ = nullptr;
    std::string path_, volumeId_;
    std::uint32_t rawSize_ = 2048, dataOffset_ = 0, sectors_ = 0;
    std::optional<std::uint32_t> layer1Base_;
    Entry root_, root1_;
};

}  // namespace anyps2::rt
