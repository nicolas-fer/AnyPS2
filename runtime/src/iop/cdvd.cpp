// cdvdman/cdvdfsv em HLE: servidores RPC do libcdvd (ps2sdk ee/rpc/cdvd).
//
//   0x80000592 INIT       sceCdInit
//   0x80000593 SCMD       comandos "S" (relógio, tipo de disco, bandeja...)
//   0x80000595 NCMD       comandos "N" (leitura de setores, seek, pausa...)
//   0x80000596 POFF       callback de desligamento (só registro)
//   0x80000597 SEARCHFILE sceCdSearchFile
//   0x8000059A DISKREADY  sceCdDiskReady

#include "anyps2/runtime/iop/cdvd.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/iop/iop.h"
#include "anyps2/runtime/memory.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/timing.h"

namespace anyps2::rt {

using namespace iopio;

namespace {

enum NCmd : std::uint32_t {
    CD_NCMD_READ = 1, CD_NCMD_CDDAREAD, CD_NCMD_DVDREAD, CD_NCMD_GETTOC, CD_NCMD_SEEK, CD_NCMD_STANDBY,
    CD_NCMD_STOP, CD_NCMD_PAUSE, CD_NCMD_STREAM, CD_NCMD_CDDASTREAM, CD_NCMD_READ_KEY, CD_NCMD_NCMD,
    CD_NCMD_READIOPMEM, CD_NCMD_DISKREADY, CD_NCMD_READCHAIN,
};
const char* ncmdName(std::uint32_t f) {
    static const char* kNames[] = {"?", "READ", "CDDAREAD", "DVDREAD", "GETTOC", "SEEK", "STANDBY", "STOP",
                                   "PAUSE", "STREAM", "CDDASTREAM", "READ_KEY", "NCMD", "READIOPMEM",
                                   "DISKREADY", "READCHAIN"};
    return f < 16 ? kNames[f] : "?";
}

enum SCmd : std::uint32_t {
    CD_SCMD_READCLOCK = 1, CD_SCMD_WRITECLOCK, CD_SCMD_GETDISKTYPE, CD_SCMD_GETERROR, CD_SCMD_TRAYREQ,
    CD_SCMD_READ_ILINK_ID, CD_SCMD_WRITE_ILINK_ID, CD_SCMD_READ_NVM, CD_SCMD_WRITE_NVM, CD_SCMD_DEC_SET,
    CD_SCMD_SCMD, CD_SCMD_STATUS, CD_SCMD_SET_HD_MODE, CD_SCMD_OPEN_CONFIG, CD_SCMD_CLOSE_CONFIG,
    CD_SCMD_READ_CONFIG, CD_SCMD_WRITE_CONFIG, CD_SCMD_READ_CONSOLE_ID, CD_SCMD_WRITE_CONSOLE_ID,
    CD_SCMD_READ_MECHACON_VERSION, CD_SCMD_CTRL_AD_OUT, CD_SCMD_BREAK, CD_SCMD_READ_SUBQ, CD_SCMD_FORBID_DVDP,
    CD_SCMD_AUTO_ADJUST_CTRL, CD_SCMD_READ_MODEL_NAME, CD_SCMD_WRITE_MODEL_NAME, CD_SCMD_FORBID_READ,
    CD_SCMD_SPIN_CTRL, CD_SCMD_BOOT_CERTIFY, CD_SCMD_CANCELPOWEROFF, CD_SCMD_BLUELEDCTRL, CD_SCMD_POWEROFF,
    CD_SCMD_MMODE, CD_SCMD_SETTHREADPRI,
};
const char* scmdName(std::uint32_t f) {
    static const char* kNames[] = {
        "?", "READCLOCK", "WRITECLOCK", "GETDISKTYPE", "GETERROR", "TRAYREQ", "READ_ILINK_ID",
        "WRITE_ILINK_ID", "READ_NVM", "WRITE_NVM", "DEC_SET", "SCMD", "STATUS", "SET_HD_MODE", "OPEN_CONFIG",
        "CLOSE_CONFIG", "READ_CONFIG", "WRITE_CONFIG", "READ_CONSOLE_ID", "WRITE_CONSOLE_ID",
        "READ_MECHACON_VERSION", "CTRL_AD_OUT", "BREAK", "READ_SUBQ", "FORBID_DVDP", "AUTO_ADJUST_CTRL",
        "READ_MODEL_NAME", "WRITE_MODEL_NAME", "FORBID_READ", "SPIN_CTRL", "BOOT_CERTIFY", "CANCELPOWEROFF",
        "BLUELEDCTRL", "POWEROFF", "MMODE", "SETTHREADPRI"};
    return f < 36 ? kNames[f] : "?";
}

constexpr std::int32_t kDiskComplete = 2, kDiskNotReady = 6;
constexpr std::int32_t kTypeNoDisc = 0x00, kTypePs2Cd = 0x12, kTypePs2Dvd = 0x14;
constexpr std::int32_t kStatPause = 0x0A, kStatStop = 0x00;
// Versões que sceCdInit lê (> 0x104: formato novo de _CdAlignReadBuffer).
constexpr std::uint32_t kCdvdfsvVersion = 0x0225, kCdvdmanVersion = 0x0225;
// Um CD de 80 minutos tem ~360000 setores; acima disso a imagem é de DVD.
constexpr std::uint32_t kMaxCdSectors = 360000;
constexpr std::uint32_t kMaxNotReadyPolls = 2000;

std::uint8_t bcd(int v) {
    return static_cast<std::uint8_t>(((v / 10) << 4) | (v % 10));
}

}  // namespace

Cdvd::Cdvd(Iop& iop) : iop_(iop) {
    isoPath_ = iop_.runtime().options().iso;
}

Cdvd::~Cdvd() = default;

bool Cdvd::hasDisc() const {
    return !isoPath_.empty();
}

IsoImage& Cdvd::image(const std::string& why, std::uint32_t pc) {
    if (!hasDisc()) {
        throw Unimplemented(why + ": o programa lê o disco, mas nenhuma imagem foi informada (defina "
                                  "ANYPS2_ISO com o caminho do dump .iso/.bin)",
                            pc);
    }
    if (!iso_.isOpen()) iso_.open(isoPath_);
    return iso_;
}

std::optional<std::vector<std::uint8_t>> Cdvd::readFile(const std::string& path, std::uint32_t pc) {
    IsoImage& iso = image("leitura de cdrom0:" + path, pc);
    const auto e = iso.lookup(path);
    if (!e || e->isDir) return std::nullopt;
    std::vector<std::uint8_t> data(e->size);
    if (!iso.readBytes(e->lsn, 0, e->size, data.data())) {
        throw GuestError("cdrom0:" + path + " passa do fim da imagem '" + iso.path() + "' (dump truncado?)", pc);
    }
    return data;
}

void Cdvd::registerServers() {
    iop_.registerServer(0x80000592u, "cdvdfsv.init",
                        [this](std::uint32_t, const std::vector<std::uint8_t>&,
                               std::uint32_t) -> std::optional<std::vector<std::uint8_t>> {
                            // CdInitPkt {result, versão cdvdfsv, versão cdvdman, verbose}
                            std::vector<std::uint8_t> out(16, 0);
                            wr32(out, 0, 1);
                            wr32(out, 4, kCdvdfsvVersion);
                            wr32(out, 8, kCdvdmanVersion);
                            notReadyPolls_ = 0;
                            return out;
                        });
    iop_.registerServer(0x80000593u, "cdvdfsv.scmd",
                        [this](std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc) {
                            return scmd(fn, in, pc);
                        });
    iop_.registerServer(0x80000595u, "cdvdfsv.ncmd",
                        [this](std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc) {
                            return ncmd(fn, in, pc);
                        });
    iop_.registerServer(0x80000596u, "cdvdfsv.poff",
                        [](std::uint32_t fn, const std::vector<std::uint8_t>&,
                           std::uint32_t pc) -> std::optional<std::vector<std::uint8_t>> {
                            throw Unimplemented("cdvdfsv: função " + std::to_string(fn) +
                                                    " do servidor de desligamento (POFF) não implementada",
                                                pc);
                        });
    iop_.registerServer(0x80000597u, "cdvdfsv.searchfile",
                        [this](std::uint32_t, const std::vector<std::uint8_t>& in,
                               std::uint32_t pc) -> std::optional<std::vector<std::uint8_t>> {
                            return searchFile(in, pc);
                        });
    iop_.registerServer(0x8000059Au, "cdvdfsv.diskready",
                        [this](std::uint32_t, const std::vector<std::uint8_t>&,
                               std::uint32_t pc) -> std::optional<std::vector<std::uint8_t>> {
                            return diskReady(pc);
                        });
}

std::vector<std::uint8_t> Cdvd::diskReady(std::uint32_t pc) {
    if (hasDisc()) return result32(kDiskComplete);
    // Sem disco: "não pronto". Quem espera em laço nunca sai — erro claro.
    if (++notReadyPolls_ > kMaxNotReadyPolls) {
        throw Unimplemented("o programa espera o leitor de disco ficar pronto, mas nenhuma imagem foi "
                            "informada (defina ANYPS2_ISO)",
                            pc);
    }
    return result32(kDiskNotReady);
}

std::vector<std::uint8_t> Cdvd::searchFile(const std::vector<std::uint8_t>& in, std::uint32_t pc) {
    // SearchFilePkt {u8 padding[32]; char name[256]; void* dest} → o
    // sceCdlFILE {lsn, size, name[16], date[8]} é escrito em dest (EE).
    const std::string name = cstr(in, 32, 256);
    const std::uint32_t dest = rd32(in, 288);
    IsoImage& iso = image("sceCdSearchFile(\"" + name + "\")", pc);
    const auto e = iso.lookup(name);
    if (iop_.runtime().options().traceIop) {
        std::fprintf(stderr, "[iop] sceCdSearchFile(%s) → %s\n", name.c_str(), e ? "achou" : "não achou");
    }
    if (!e) return result32(0);
    std::uint8_t file[32] = {};
    std::memcpy(file + 0, &e->lsn, 4);
    std::memcpy(file + 4, &e->size, 4);
    std::memcpy(file + 8, e->name.data(), std::min<std::size_t>(e->name.size(), 15));
    // date: 0 = flags ISO, 1 = seg, 2 = min, 3 = hora, 4 = dia, 5 = mês, 6-7 = ano
    const int year = e->date[0] + 1900;
    file[24] = e->flags;
    file[25] = e->date[5];
    file[26] = e->date[4];
    file[27] = e->date[3];
    file[28] = e->date[2];
    file[29] = e->date[1];
    file[30] = static_cast<std::uint8_t>(year & 0xFF);
    file[31] = static_cast<std::uint8_t>(year >> 8);
    if (dest) iop_.runtime().memory().copyToGuest(dest, file, sizeof(file), pc);
    return result32(1);
}

std::vector<std::uint8_t> Cdvd::readClock() {
    // {result, sceCdCLOCK {stat, seg, min, hora, pad, dia, mês, ano} em BCD}
    std::tm t{};
    Runtime& rt = iop_.runtime();
    if (rt.options().virtualClock) {
        const auto secs = static_cast<long long>(rt.timing().now() / Timing::kEeHz);
        t.tm_year = 100;
        t.tm_mon = 0;
        t.tm_mday = 1 + static_cast<int>(secs / 86400);
        t.tm_hour = static_cast<int>(secs / 3600 % 24);
        t.tm_min = static_cast<int>(secs / 60 % 60);
        t.tm_sec = static_cast<int>(secs % 60);
        if (t.tm_mday > 31) t.tm_mday = 31;  // programas de teste não rodam um mês
    } else {
        const std::time_t now = std::time(nullptr);
#ifdef _WIN32
        localtime_s(&t, &now);
#else
        localtime_r(&now, &t);
#endif
    }
    std::vector<std::uint8_t> out(16, 0);
    wr32(out, 0, 1);
    out[4] = 0;
    out[5] = bcd(t.tm_sec);
    out[6] = bcd(t.tm_min);
    out[7] = bcd(t.tm_hour);
    out[8] = 0;
    out[9] = bcd(t.tm_mday);
    out[10] = bcd(t.tm_mon + 1);
    out[11] = bcd(t.tm_year % 100);
    return out;
}

std::optional<std::vector<std::uint8_t>> Cdvd::scmd(std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                    std::uint32_t pc) {
    (void)in;
    auto two = [](std::int32_t a, std::uint32_t b) {
        std::vector<std::uint8_t> v(8, 0);
        wr32(v, 0, static_cast<std::uint32_t>(a));
        wr32(v, 4, b);
        return v;
    };
    switch (fn) {
        case CD_SCMD_READCLOCK:
            return readClock();
        case CD_SCMD_GETDISKTYPE: {
            if (!hasDisc()) return result32(kTypeNoDisc);
            const IsoImage& iso = image("sceCdGetDiskType", pc);
            return result32(iso.sectorCount() > kMaxCdSectors ? kTypePs2Dvd : kTypePs2Cd);
        }
        case CD_SCMD_GETERROR:
            return result32(0);  // SCECdErNO
        case CD_SCMD_TRAYREQ:  // {result, traychk}: bandeja nunca muda
            return two(1, 0);
        case CD_SCMD_STATUS:
            return result32(hasDisc() ? kStatPause : kStatStop);
        case CD_SCMD_BREAK:
        case CD_SCMD_MMODE:
        case CD_SCMD_SETTHREADPRI:
        case CD_SCMD_SPIN_CTRL:
            return result32(1);
        case CD_SCMD_CANCELPOWEROFF:
        case CD_SCMD_BLUELEDCTRL:
        case CD_SCMD_FORBID_DVDP:
        case CD_SCMD_AUTO_ADJUST_CTRL:
            return two(1, 0);
        default:
            throw Unimplemented(std::string("libcdvd: comando S ") + scmdName(fn) + " (" + std::to_string(fn) +
                                    ") não implementado no HLE do cdvdfsv",
                                pc);
    }
}

std::optional<std::vector<std::uint8_t>> Cdvd::ncmd(std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                    std::uint32_t pc) {
    Runtime& rt = iop_.runtime();
    if (rt.options().traceIop) std::fprintf(stderr, "[iop] cdvd N %s\n", ncmdName(fn));
    switch (fn) {
        case CD_NCMD_READ:
        case CD_NCMD_DVDREAD:
        case CD_NCMD_READIOPMEM: {
            // readData {lbn, sectors, buf, mode, _rd_intr_data, &curReadPos}
            const std::uint32_t lbn = rd32(in, 0), sectors = rd32(in, 4), buf = rd32(in, 8);
            const std::uint32_t mode = rd32(in, 12), intr = rd32(in, 16), readPos = rd32(in, 20);
            const std::uint32_t pattern = (mode >> 16) & 0xFF;
            if (pattern != 0) {
                throw Unimplemented("sceCdRead com setores de " +
                                        std::string(pattern == 1 ? "2328" : pattern == 2 ? "2340" : "?") +
                                        " bytes (datapattern " + std::to_string(pattern) +
                                        "): só leitura de 2048 bytes no HLE",
                                    pc);
            }
            IsoImage& iso = image(std::string("sceCd") + (fn == CD_NCMD_READIOPMEM ? "ReadIOPMem" : "Read"), pc);
            std::vector<std::uint8_t> data(std::size_t{sectors} * IsoImage::kSectorSize);
            if (!iso.readSectors(lbn, sectors, data.data())) {
                throw GuestError("sceCdRead: setores " + std::to_string(lbn) + "+" + std::to_string(sectors) +
                                     " passam do fim da imagem (" + std::to_string(iso.sectorCount()) +
                                     " setores)",
                                 pc);
            }
            const auto bytes = static_cast<std::uint32_t>(data.size());
            if (fn == CD_NCMD_READIOPMEM) {
                std::memcpy(iop_.iopPointer(buf, bytes, pc), data.data(), bytes);
            } else {
                rt.memory().copyToGuest(buf, data.data(), bytes, pc);
                // _CdAlignReadBuffer: pontas desalinhadas já foram escritas.
                if (intr) {
                    const std::uint32_t zero[4] = {0, 0, 0, 0};
                    rt.memory().copyToGuest(intr, zero, sizeof(zero), pc);
                }
            }
            if (readPos) rt.memory().copyToGuest(readPos, &bytes, 4, pc);
            return std::vector<std::uint8_t>{};
        }
        case CD_NCMD_SEEK:
        case CD_NCMD_STANDBY:
        case CD_NCMD_STOP:
        case CD_NCMD_PAUSE:
            return std::vector<std::uint8_t>{};
        case CD_NCMD_DISKREADY:
            return diskReady(pc);
        default:
            throw Unimplemented(std::string("libcdvd: comando N ") + ncmdName(fn) + " (" + std::to_string(fn) +
                                    ") não implementado no HLE do cdvdfsv",
                                pc);
    }
}

}  // namespace anyps2::rt
