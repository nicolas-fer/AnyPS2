#pragma once

// Triagem de um disco de PS2 ("anyps2 disc"): o que o jogo traz e o que
// disso o AnyPS2 já cobre. Só lê a imagem; nada é extraído nem gravado.

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace anyps2::disc {

struct SystemCnf {
    bool present = false;
    std::vector<std::pair<std::string, std::string>> fields;  // na ordem do arquivo
    std::string value(const std::string& key) const;          // "" se ausente
};

struct ElfSegment {
    std::uint32_t type = 0, offset = 0, vaddr = 0, filesz = 0, memsz = 0, flags = 0;
};

struct ElfSummary {
    std::uint64_t fileSize = 0;
    std::uint16_t type = 0;
    std::uint32_t entry = 0, eflags = 0;
    bool r5900 = false;
    std::uint32_t loadBase = 0, loadEnd = 0;
    std::vector<ElfSegment> segments;
    std::size_t sections = 0, symbols = 0, functions = 0;
};

// Arquivo do disco. `path` como "\\IRX\\SIO2MAN.IRX;1"; `layer` 0 ou 1.
struct DiscFile {
    std::string path;
    unsigned layer = 0;
    std::uint32_t lsn = 0, size = 0;
};

struct DiscIrx {
    DiscFile file;
    std::string name;  // gravado no IRX
    std::uint16_t version = 0;
    std::string hle;   // implementação HLE; vazio = não tem
};

// Imagem de módulos do IOP no formato ROMDIR (IOPRP*.IMG: módulos que
// substituem os da ROM depois de um reboot do IOP com rom0:UDNL).
struct ImageModule {
    std::string romName;  // nome na ROMDIR ("CDVDMAN")
    std::uint32_t size = 0;
    bool irx = false;
    std::string irxName;
    std::uint16_t version = 0;
    std::string hle;
    bool kernel = false;  // núcleo do IOP (SYSMEM, LOADCORE...): coberto pelo IOP em HLE
};
struct IopImage {
    DiscFile file;
    std::vector<ImageModule> modules;
    std::string bootConfig;  // conteúdo de IOPBTCONF, se houver
};

// ELF do disco que não é o principal nem IRX (overlays, outros programas).
struct OtherElf {
    DiscFile file;
    std::uint16_t type = 0;
    bool r5900 = false;
};

// String do executável principal com um caminho de dispositivo.
struct DeviceRef {
    std::uint32_t offset = 0;  // no arquivo ELF
    std::string text;          // string inteira, como gravada
    std::string device;        // "rom0", "cdrom0", "host"...; vazio = só o nome de um IRX
    std::string target;        // caminho depois do dispositivo, até o espaço
    bool module = false;       // módulo do IOP (rom0:, .IRX, .IMG)
    std::string status;        // o que é / o que o AnyPS2 faz com isso
    std::string hle;           // implementação HLE, se for módulo com HLE
};

struct EmbeddedIrx {
    std::uint32_t offset = 0;  // no arquivo ELF
    std::string name;
    std::uint16_t version = 0;
    std::string hle;
};

struct Report {
    std::string isoPath, volumeId;
    std::uint32_t sectors = 0;
    unsigned layers = 1;
    std::optional<std::uint32_t> layer1Start;
    std::size_t fileCount = 0, dirCount = 0;
    std::uint64_t totalBytes = 0;
    std::vector<DiscFile> largest;  // até 10, do maior para o menor

    SystemCnf cnf;
    std::string mainElfPath;     // no disco, a partir do BOOT2
    std::string mainElfProblem;  // por que não foi possível analisar; vazio se ok
    std::optional<ElfSummary> mainElf;
    std::vector<DeviceRef> refs;
    std::vector<EmbeddedIrx> embedded;

    std::vector<DiscIrx> irx;
    std::vector<IopImage> images;
    std::vector<OtherElf> elfs;
};

// Lê a imagem e monta o relatório. Lança anyps2::Error se a imagem não abrir.
Report triage(const std::string& isoPath);
void print(const Report& report, std::ostream& out);

}  // namespace anyps2::disc
