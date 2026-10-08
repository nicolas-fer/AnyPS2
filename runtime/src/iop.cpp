#include "anyps2/runtime/iop.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "anyps2/common/bytes.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

namespace {

// Comandos SIF do sistema (ps2sdk/common/include/sifcmd-common.h).
constexpr std::uint32_t kCmdChangeSaddr = 0x80000000u;
constexpr std::uint32_t kCmdSetSreg = 0x80000001u;
constexpr std::uint32_t kCmdInitCmd = 0x80000002u;
constexpr std::uint32_t kCmdResetCmd = 0x80000003u;
constexpr std::uint32_t kCmdRpcEnd = 0x80000008u;
constexpr std::uint32_t kCmdRpcBind = 0x80000009u;
constexpr std::uint32_t kCmdRpcCall = 0x8000000Au;
constexpr std::uint32_t kCmdRpcRdata = 0x8000000Cu;

// Registradores SIF (ps2sdk/ee/kernel/include/sifdma.h).
constexpr std::uint32_t kRegMainAddr = 1, kRegSubAddr = 2, kRegMsFlag = 3, kRegSmFlag = 4;
constexpr std::uint32_t kStatSifInit = 0x10000, kStatCmdInit = 0x20000, kStatBootEnd = 0x40000;

// Layout da RAM do IOP no HLE.
constexpr std::uint32_t kIopCmdBuffer = 0x00001000u;  // onde o EE envia comandos (SMCOM)
constexpr std::uint32_t kServerBufferSize = 0x10000u;
constexpr unsigned kDmacSif0 = 5;

// fileio (ps2sdk/common/include/fileio-common.h, io_common.h)
enum FioFunction : std::uint32_t {
    FIO_OPEN = 0, FIO_CLOSE, FIO_READ, FIO_WRITE, FIO_LSEEK, FIO_IOCTL, FIO_REMOVE, FIO_MKDIR,
    FIO_RMDIR, FIO_DOPEN, FIO_DCLOSE, FIO_DREAD, FIO_GETSTAT, FIO_CHSTAT, FIO_FORMAT,
    FIO_ADDDRV, FIO_DELDRV,
};
const char* fioName(std::uint32_t f) {
    static const char* kNames[] = {"open", "close", "read", "write", "lseek", "ioctl", "remove",
                                   "mkdir", "rmdir", "dopen", "dclose", "dread", "getstat",
                                   "chstat", "format", "adddrv", "deldrv"};
    return f < 17 ? kNames[f] : "?";
}
constexpr std::uint32_t kIoRdOnly = 1, kIoWrOnly = 2, kIoAppend = 0x100, kIoCreat = 0x200, kIoTrunc = 0x400;
constexpr std::int32_t kENOENT = 2, kEBADF = 9, kEINVAL = 22;

std::uint32_t rd32(const std::vector<std::uint8_t>& v, std::size_t off) {
    return off + 4 <= v.size() ? readLE32(v, off) : 0;
}
void wr32(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) {
    if (v.size() < off + 4) v.resize(off + 4);
    writeLE32(v, off, x);
}
std::string cstr(const std::vector<std::uint8_t>& in, std::size_t off, std::size_t max) {
    if (off >= in.size()) return {};
    const char* p = reinterpret_cast<const char*>(in.data() + off);
    return std::string(p, strnlen(p, std::min(max, in.size() - off)));
}

// io_stat_t (ps2sdk/common/include/io_common.h) com os bits FIO_SO_* do ioman.
std::vector<std::uint8_t> ioStat(const std::filesystem::path& p, bool& ok) {
    std::vector<std::uint8_t> st(40, 0);
    std::error_code ec;
    const auto status = std::filesystem::status(p, ec);
    ok = !ec && std::filesystem::exists(status);
    if (!ok) return st;
    const bool dir = std::filesystem::is_directory(status);
    std::uint32_t mode = (dir ? 0x0020u : 0x0010u) | 0x0007u;  // FIO_SO_IFDIR/IFREG | rwx
    std::uint64_t size = dir ? 0 : std::filesystem::file_size(p, ec);
    writeLE32(st, 0, mode);
    writeLE32(st, 8, static_cast<std::uint32_t>(size));
    writeLE32(st, 36, static_cast<std::uint32_t>(size >> 32));
    return st;
}

std::vector<std::uint8_t> result32(std::int32_t r) {
    std::vector<std::uint8_t> v(4);
    writeLE32(v, 0, static_cast<std::uint32_t>(r));
    return v;
}

}  // namespace

Iop::OpenFile Iop::makeFile(std::FILE* fp, bool console, std::string path) {
    OpenFile f;
    f.fp = fp;
    f.console = console;
    f.path = std::move(path);
    return f;
}

Iop::Iop(Runtime& rt) : rt_(rt), ram_(kRamSize, 0) {
    // O IOP do HLE já "bootou": SIF e SIFCMD prontos.
    smcom_ = kIopCmdBuffer;
    smflg_ = kStatSifInit | kStatCmdInit | kStatBootEnd;
    files_[0] = makeFile(stdin, true, "tty:");
    files_[1] = makeFile(stdout, true, "tty:");
    files_[2] = makeFile(stderr, true, "tty:");
    registerFileio();
    registerIopHeap();
}

Iop::~Iop() {
    for (auto& [fd, f] : files_) {
        if (f.fp && !f.console) std::fclose(f.fp);
    }
}

std::uint8_t* Iop::iopPointer(std::uint32_t addr, std::uint32_t size, std::uint32_t pc) {
    const std::uint32_t a = addr & 0x1FFFFFu;
    if (std::uint64_t{a} + size > kRamSize) {
        throw GuestError("acesso à RAM do IOP fora dos 2 MB: " + anyps2::hex(addr) + " +" + anyps2::hex(size), pc);
    }
    return ram_.data() + a;
}

// ---------------------------------------------------------------------------
// Registradores
// ---------------------------------------------------------------------------

std::uint32_t Iop::sifSetReg(std::uint32_t reg, std::uint32_t value) {
    switch (reg) {
        case kRegMainAddr: mscom_ = value; return 0;
        case kRegSubAddr: smcom_ = value; return 0;
        case kRegMsFlag: msflg_ |= value; return 0;
        case kRegSmFlag: smflg_ &= ~value; return 0;
        default:
            if (reg & 0x80000000u) sysregs_[reg] = value;
            return 0;
    }
}

std::uint32_t Iop::sifGetReg(std::uint32_t reg) const {
    switch (reg) {
        case kRegMainAddr: return mscom_;
        case kRegSubAddr: return smcom_;
        case kRegMsFlag: return msflg_;
        case kRegSmFlag: return smflg_;
        default: {
            auto it = sysregs_.find(reg);
            return it == sysregs_.end() ? 0 : it->second;
        }
    }
}

std::uint32_t Iop::sifReadRegister(std::uint32_t addr) const {
    switch (addr) {
        case 0x1000F200: return mscom_;
        case 0x1000F210: return smcom_;
        case 0x1000F220: return msflg_;
        case 0x1000F230: return smflg_;
        case 0x1000F240: return ctrl_;
        case 0x1000F260: return bd6_;
        default: return 0;
    }
}

void Iop::sifWriteRegister(std::uint32_t addr, std::uint32_t value) {
    switch (addr) {
        case 0x1000F200: mscom_ = value; break;
        case 0x1000F220: msflg_ |= value; break;
        case 0x1000F230: smflg_ &= ~value; break;
        case 0x1000F240: ctrl_ = value; break;
        case 0x1000F260: bd6_ = value; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------
// DMA EE -> IOP e comandos
// ---------------------------------------------------------------------------

std::uint32_t Iop::sifSetDma(std::uint32_t transfers, std::uint32_t count, std::uint32_t pc) {
    Memory& m = rt_.memory();
    std::vector<std::vector<std::uint8_t>> commands;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t t = transfers + 16 * i;
        const std::uint32_t src = m.read<std::uint32_t>(t, pc);
        const std::uint32_t dest = m.read<std::uint32_t>(t + 4, pc);
        const std::uint32_t size = m.read<std::uint32_t>(t + 8, pc);
        const std::uint32_t sizeAligned = (size + 15) & ~15u;  // DMA em quadwords
        std::uint8_t* dst = iopPointer(dest, sizeAligned, pc);
        m.copyFromGuest(dst, src, size, pc);
        if ((dest & 0x1FFFFFu) == (smcom_ & 0x1FFFFFu)) {
            commands.emplace_back(dst, dst + size);
        }
    }
    for (const auto& cmd : commands) handleCommand(cmd, pc);
    deliverPending(pc);
    return nextDmaId_++;
}

void Iop::handleCommand(const std::vector<std::uint8_t>& p, std::uint32_t pc) {
    const std::uint32_t cid = rd32(p, 8);
    const std::uint32_t opt = rd32(p, 12);
    if (rt_.options().traceIop) {
        std::fprintf(stderr, "[iop] comando 0x%08x (opt %u, %zu bytes)\n", cid, opt, p.size());
    }
    switch (cid) {
        case kCmdInitCmd:
            if (opt == 0) {
                eeCmdBuffer_ = rd32(p, 16);
            } else {
                // Inicialização do SIF RPC: responde SET_SREG(RPCINIT = 1).
                std::vector<std::uint8_t> reply(24, 0);
                wr32(reply, 16, 0);  // SIF_SREG_RPCINIT
                wr32(reply, 20, 1);
                sendToEe(kCmdSetSreg, reply, nullptr, 0, 0, pc);
            }
            return;
        case kCmdChangeSaddr:
            eeCmdBuffer_ = rd32(p, 16);
            return;
        case kCmdSetSreg:
            sysregs_[0x40000000u | rd32(p, 16)] = rd32(p, 20);
            return;
        case kCmdResetCmd:
            // Reboot do IOP (SifIopReset): no HLE o IOP já está pronto.
            smflg_ |= kStatSifInit | kStatCmdInit | kStatBootEnd;
            return;
        case kCmdRpcBind: {
            const std::uint32_t sid = rd32(p, 32);
            auto it = servers_.find(sid);
            if (it == servers_.end()) {
                throw Unimplemented("bind de servidor SIF RPC " + anyps2::hex(sid) +
                                        " — módulo do IOP não implementado no HLE (servidores: fileio "
                                        "0x80000001)",
                                    pc);
            }
            std::vector<std::uint8_t> reply(64, 0);
            wr32(reply, 16, rd32(p, 16));  // rec_id
            wr32(reply, 20, rd32(p, 20));  // pkt_addr
            wr32(reply, 24, rd32(p, 24));  // rpc_id
            wr32(reply, 28, rd32(p, 28));  // cd
            wr32(reply, 32, kCmdRpcBind);  // cid
            wr32(reply, 36, it->second.serverData);
            wr32(reply, 40, it->second.buffer);
            wr32(reply, 44, 0);  // cbuf
            if (rt_.options().traceIop) std::fprintf(stderr, "[iop] bind %s\n", it->second.name.c_str());
            sendToEe(kCmdRpcEnd, reply, nullptr, 0, 0, pc);
            return;
        }
        case kCmdRpcCall: {
            const std::uint32_t rpcNumber = rd32(p, 32);
            const std::uint32_t sendSize = rd32(p, 36);
            const std::uint32_t recvBuf = rd32(p, 40);
            const std::uint32_t recvSize = rd32(p, 44);
            const std::uint32_t rmode = rd32(p, 48);
            const std::uint32_t sd = rd32(p, 52);
            const Server* server = nullptr;
            for (const auto& [sid, s] : servers_) {
                if (s.serverData == sd) server = &s;
            }
            if (!server) throw GuestError("chamada SIF RPC para servidor desconhecido " + anyps2::hex(sd), pc);
            const std::uint8_t* data = iopPointer(server->buffer, sendSize, pc);
            std::vector<std::uint8_t> in(data, data + sendSize);
            std::vector<std::uint8_t> out = server->handler(rpcNumber, in, pc);
            const std::uint32_t n = std::min<std::uint32_t>(recvSize, static_cast<std::uint32_t>(out.size()));
            if (rmode == 0) {
                if (n) rt_.memory().copyToGuest(recvBuf, out.data(), n, pc);
                return;
            }
            std::vector<std::uint8_t> reply(64, 0);
            wr32(reply, 16, rd32(p, 16));
            wr32(reply, 20, rd32(p, 20));
            wr32(reply, 24, rd32(p, 24));
            wr32(reply, 28, rd32(p, 28));
            wr32(reply, 32, kCmdRpcCall);
            sendToEe(kCmdRpcEnd, reply, out.data(), n, recvBuf, pc);
            return;
        }
        case kCmdRpcRdata: {
            // Pedido de dados da RAM do IOP para o EE.
            const std::uint32_t recvbuf = rd32(p, 28), src = rd32(p, 32), dest = rd32(p, 36), size = rd32(p, 40);
            std::vector<std::uint8_t> reply(64, 0);
            wr32(reply, 20, rd32(p, 20));
            wr32(reply, 28, recvbuf);
            wr32(reply, 32, kCmdRpcRdata);
            sendToEe(kCmdRpcEnd, reply, iopPointer(src, size, pc), size, dest, pc);
            return;
        }
        default:
            throw Unimplemented("comando SIF " + anyps2::hex(cid) + " enviado ao IOP não é suportado pelo HLE", pc);
    }
}

void Iop::sendToEe(std::uint32_t cid, std::vector<std::uint8_t> packet, const std::uint8_t* extra,
                   std::uint32_t extraSize, std::uint32_t extraDest, std::uint32_t pc) {
    if (eeCmdBuffer_ == 0) {
        throw GuestError("IOP tentou enviar comando ao EE antes de SIF_CMD_INIT_CMD", pc);
    }
    if (extraSize > 0) rt_.memory().copyToGuest(extraDest, extra, extraSize, pc);
    const auto psize = static_cast<std::uint32_t>(packet.size());
    wr32(packet, 0, (psize & 0xFF) | (extraSize << 8));
    wr32(packet, 4, extraSize ? extraDest : 0);
    wr32(packet, 8, cid);
    pending_.push_back({std::move(packet)});
}

void Iop::deliverPending(std::uint32_t pc) {
    if (delivering_) return;  // um handler está rodando; entrega ao terminar
    delivering_ = true;
    try {
        while (!pending_.empty() && rt_.kernel().canDeliverDmac(kDmacSif0)) {
            Pending next = std::move(pending_.front());
            pending_.erase(pending_.begin());
            rt_.memory().copyToGuest(eeCmdBuffer_, next.packet.data(),
                                     static_cast<std::uint32_t>(next.packet.size()), pc);
            rt_.kernel().raiseDmacInterrupt(kDmacSif0, pc);
        }
    } catch (...) {
        delivering_ = false;
        throw;
    }
    delivering_ = false;
}

void Iop::registerServer(std::uint32_t sid, std::string name, RpcHandler handler) {
    Server s;
    s.sid = sid;
    s.name = std::move(name);
    s.handler = std::move(handler);
    s.buffer = nextServerBuffer_;
    s.serverData = 0x00008000u + static_cast<std::uint32_t>(servers_.size()) * 0x80u;
    nextServerBuffer_ += kServerBufferSize;
    servers_[sid] = std::move(s);
}

// ---------------------------------------------------------------------------
// Servidor de heap do IOP (0x80000003): SifAllocIopHeap / SifFreeIopHeap /
// SifLoadIopHeap
// ---------------------------------------------------------------------------

void Iop::registerIopHeap() {
    registerServer(0x80000003u, "iopheap", [this](std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                  std::uint32_t pc) -> std::vector<std::uint8_t> {
        switch (fn) {
            case 1: {  // alloc(size): o SYSMEM aloca em blocos de 256 bytes
                const std::uint32_t size = (rd32(in, 0) + 255) & ~255u;
                if (heapNext_ + size > kRamSize) return result32(0);
                const std::uint32_t addr = heapNext_;
                heapNext_ += size;
                return result32(static_cast<std::int32_t>(addr));
            }
            case 2:  // free: o bump allocator do HLE não reaproveita
                return result32(0);
            case 3: {  // load(addr, path): carrega um arquivo na RAM do IOP
                const std::uint32_t addr = rd32(in, 0);
                std::string path(reinterpret_cast<const char*>(in.data() + 4),
                                 strnlen(reinterpret_cast<const char*>(in.data() + 4), in.size() - 4));
                if (path == "rom:ROMVER" || path == "rom0:ROMVER") {
                    // Identificador de versão sintético (não é conteúdo de ROM):
                    // versão 2.30, região E, console CEX, data 2008-02-20.
                    static const char kRomver[16] = "0230AC20080220";  // 2.30, EUA (NTSC), console
                    std::memcpy(iopPointer(addr, 16, pc), kRomver, 16);
                    return result32(0);
                }
                throw Unimplemented("SifLoadIopHeap(\"" + path + "\") não suportado no HLE", pc);
            }
            default:
                throw Unimplemented("iopheap: função " + std::to_string(fn) + " desconhecida", pc);
        }
    });
}

// ---------------------------------------------------------------------------
// Servidor fileio (0x80000001)
// ---------------------------------------------------------------------------

std::filesystem::path Iop::hostPath(const std::string& name, std::uint32_t pc) const {
    const auto colon = name.find(':');
    const std::string device = colon == std::string::npos ? "" : name.substr(0, colon);
    if (device != "host" && device != "host0") {
        throw Unimplemented("fileio: dispositivo '" + device + "' em \"" + name +
                                "\" não suportado no HLE (cdrom0:/mc0: chegam na Fase 6)",
                            pc);
    }
    std::string rel = name.substr(colon + 1);
    while (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) rel.erase(0, 1);
    const char* base = std::getenv("ANYPS2_HOST_DIR");
    return std::filesystem::path(base ? base : ".") / rel;
}

void Iop::registerFileio() {
    registerServer(0x80000001u, "fileio", [this](std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                 std::uint32_t pc) -> std::vector<std::uint8_t> {
        Memory& m = rt_.memory();
        if (rt_.options().traceIop) std::fprintf(stderr, "[iop] fileio.%s\n", fioName(fn));
        switch (fn) {
            case FIO_OPEN: {
                const std::uint32_t mode = rd32(in, 0);
                std::string name(reinterpret_cast<const char*>(in.data() + 4),
                                 strnlen(reinterpret_cast<const char*>(in.data() + 4), 256));
                if (name.rfind("tty", 0) == 0) {
                    const std::int32_t fd = nextFd_++;
                    files_[fd] = makeFile(stdout, true, name);
                    return result32(fd);
                }
                const std::filesystem::path path = hostPath(name, pc);
                std::string fmode;
                const std::uint32_t acc = mode & 3;
                if (acc == kIoRdOnly) fmode = "rb";
                else if (mode & kIoAppend) fmode = acc == kIoWrOnly ? "ab" : "a+b";
                else if (mode & (kIoTrunc | kIoCreat)) fmode = acc == kIoWrOnly ? "wb" : "w+b";
                else fmode = "r+b";
                std::FILE* fp = std::fopen(path.string().c_str(), fmode.c_str());
                if (!fp) return result32(-kENOENT);
                const std::int32_t fd = nextFd_++;
                files_[fd] = makeFile(fp, false, path.string());
                return result32(fd);
            }
            case FIO_CLOSE: {
                auto it = files_.find(static_cast<std::int32_t>(rd32(in, 0)));
                if (it == files_.end()) return result32(-kEBADF);
                if (it->second.fp && !it->second.console) std::fclose(it->second.fp);
                files_.erase(it);
                return result32(0);
            }
            case FIO_WRITE: {
                auto it = files_.find(static_cast<std::int32_t>(rd32(in, 0)));
                if (it == files_.end()) return result32(-kEBADF);
                const std::uint32_t ptr = rd32(in, 4), size = rd32(in, 8), mis = rd32(in, 12);
                std::vector<std::uint8_t> buf(size);
                const std::uint32_t head = std::min(mis, size);
                if (head) std::memcpy(buf.data(), in.data() + 16, head);
                if (size > head) m.copyFromGuest(buf.data() + head, ptr + head, size - head, pc);
                const std::size_t written = std::fwrite(buf.data(), 1, size, it->second.fp);
                if (it->second.console) std::fflush(it->second.fp);
                return result32(static_cast<std::int32_t>(written));
            }
            case FIO_READ: {
                auto it = files_.find(static_cast<std::int32_t>(rd32(in, 0)));
                if (it == files_.end()) return result32(-kEBADF);
                const std::uint32_t ptr = rd32(in, 4), size = rd32(in, 8), readData = rd32(in, 12);
                std::vector<std::uint8_t> buf(size);
                const std::size_t got = std::fread(buf.data(), 1, size, it->second.fp);
                if (got) m.copyToGuest(ptr, buf.data(), static_cast<std::uint32_t>(got), pc);
                // _fio_read_intr copia buf1/buf2 para as pontas desalinhadas;
                // já escrevemos tudo, então os tamanhos são zero.
                if (readData) {
                    const std::uint32_t zero[4] = {0, 0, 0, 0};
                    m.copyToGuest(readData, zero, sizeof(zero), pc);
                }
                return result32(static_cast<std::int32_t>(got));
            }
            case FIO_LSEEK: {
                auto it = files_.find(static_cast<std::int32_t>(rd32(in, 0)));
                if (it == files_.end() || it->second.console) return result32(-kEBADF);
                const auto off = static_cast<std::int32_t>(rd32(in, 4));
                const std::uint32_t whence = rd32(in, 8);
                if (whence > 2) return result32(-kEINVAL);
                if (std::fseek(it->second.fp, off, static_cast<int>(whence)) != 0) return result32(-kEINVAL);
                return result32(static_cast<std::int32_t>(std::ftell(it->second.fp)));
            }
            case FIO_GETSTAT: {  // {io_stat_t* buf; char name[256]}
                const std::uint32_t buf = rd32(in, 0);
                bool ok = false;
                const auto st = ioStat(hostPath(cstr(in, 4, 256), pc), ok);
                if (!ok) return result32(-kENOENT);
                m.copyToGuest(buf, st.data(), static_cast<std::uint32_t>(st.size()), pc);
                return result32(0);
            }
            case FIO_REMOVE:
            case FIO_RMDIR: {
                std::error_code ec;
                const bool removed = std::filesystem::remove(hostPath(cstr(in, 0, 256), pc), ec);
                return result32(removed ? 0 : -kENOENT);
            }
            case FIO_MKDIR: {  // {int mode; char name[256]}
                std::error_code ec;
                std::filesystem::create_directory(hostPath(cstr(in, 4, 256), pc), ec);
                return result32(ec ? -kENOENT : 0);
            }
            case FIO_DOPEN: {  // {int unused; char name[256]}
                const auto path = hostPath(cstr(in, 4, 256), pc);
                std::error_code ec;
                if (!std::filesystem::is_directory(path, ec)) return result32(-kENOENT);
                OpenFile f;
                f.path = path.string();
                f.isDir = true;
                for (const auto& e : std::filesystem::directory_iterator(path, ec)) {
                    f.entries.push_back(e.path().filename().string());
                }
                const std::int32_t fd = nextFd_++;
                files_[fd] = std::move(f);
                return result32(fd);
            }
            case FIO_DCLOSE: {
                auto it = files_.find(static_cast<std::int32_t>(rd32(in, 0)));
                if (it == files_.end() || !it->second.isDir) return result32(-kEBADF);
                files_.erase(it);
                return result32(0);
            }
            case FIO_DREAD: {  // {int fd; io_dirent_t* buf} -> 1 se leu, 0 no fim
                auto it = files_.find(static_cast<std::int32_t>(rd32(in, 0)));
                if (it == files_.end() || !it->second.isDir) return result32(-kEBADF);
                OpenFile& d = it->second;
                if (d.nextEntry >= d.entries.size()) return result32(0);
                const std::string& name = d.entries[d.nextEntry++];
                bool ok = false;
                auto dirent = ioStat(std::filesystem::path(d.path) / name, ok);  // io_stat_t
                dirent.resize(40 + 256 + 4, 0);
                std::memcpy(dirent.data() + 40, name.c_str(), std::min<std::size_t>(name.size(), 255));
                m.copyToGuest(rd32(in, 4), dirent.data(), static_cast<std::uint32_t>(dirent.size()), pc);
                return result32(1);
            }
            default:
                throw Unimplemented(std::string("fileio.") + fioName(fn) + " ainda não implementado no HLE do IOP", pc);
        }
    });
}

}  // namespace anyps2::rt
