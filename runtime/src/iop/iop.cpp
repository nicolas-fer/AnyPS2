#include "anyps2/runtime/iop/iop.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <fstream>
#include <iterator>

#include "anyps2/common/bytes.h"
#include "anyps2/runtime/audio.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/iop/audsrv.h"
#include "anyps2/runtime/iop/cdvd.h"
#include "anyps2/runtime/iop/mcserv.h"
#include "anyps2/runtime/iop/pad.h"
#include "anyps2/runtime/iop/spu2.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/timing.h"

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
constexpr std::uint32_t heapStart() { return 0x00100000u; }
constexpr std::uint32_t kMaxMissingBinds = 2000;

// Servidores conhecidos (para a mensagem de erro de bind).
std::string knownServerName(std::uint32_t sid) {
    switch (sid) {
        case 0x80000100: case 0x80000101: return "padman/xpadman";
        case 0x8000010F: case 0x8000011F: return "padman da ROM";
        case 0x80000400: return "mcserv";
        case 0x80000480: return "mcserv (DEV9)";
        case 0x80000592: case 0x80000593: case 0x80000595: case 0x80000596: case 0x80000597:
        case 0x8000059A: return "cdvdfsv";
        case 0x80000701: return "sdrdrv (libsdr)";
        case 0x80000901: case 0x80000902: case 0x80000903: return "multitap (mtapman)";
        case 0x0870884E: return "audsrv";
        case 0x80001000: return "libsd remoto";
        case 0x80000A00: return "usbd";
        case 0x0B0B0B00: return "fileXio";
        default: return "módulo desconhecido";
    }
}

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
constexpr std::int32_t kENOENT = 2, kEBADF = 9, kEINVAL = 22, kEROFS = 30;

// "cdrom0:\\ARQ;1" → true e o caminho no disco.
bool cdPath(const std::string& name, std::string& rel) {
    const auto colon = name.find(':');
    if (colon == std::string::npos) return false;
    const std::string device = name.substr(0, colon);
    if (device != "cdrom0" && device != "cdrom") return false;
    rel = name.substr(colon + 1);
    return true;
}

// io_stat_t de uma entrada do disco (FIO_SO_IFDIR/IFREG, só leitura).
std::vector<std::uint8_t> cdStat(const IsoImage::Entry& e) {
    std::vector<std::uint8_t> st(40, 0);
    writeLE32(st, 0, (e.isDir ? 0x0020u : 0x0010u) | 0x0005u);
    writeLE32(st, 8, e.size);
    // ctime/atime/mtime: {res, seg, min, hora, dia, mês, ano16}
    const int year = e.date[0] + 1900;
    const std::uint8_t t[8] = {0, e.date[5], e.date[4], e.date[3], e.date[2], e.date[1],
                               static_cast<std::uint8_t>(year & 0xFF), static_cast<std::uint8_t>(year >> 8)};
    for (int i = 0; i < 3; ++i) std::memcpy(st.data() + 12 + 8 * i, t, 8);
    return st;
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

}  // namespace

namespace iopio {
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
std::vector<std::uint8_t> result32(std::int32_t r) {
    std::vector<std::uint8_t> v(4);
    writeLE32(v, 0, static_cast<std::uint32_t>(r));
    return v;
}
}  // namespace iopio

using namespace iopio;

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
    pad_ = std::make_unique<PadMan>(*this);
    mc_ = std::make_unique<McServ>(*this);
    cdvd_ = std::make_unique<Cdvd>(*this);
    spu2_ = std::make_unique<Spu2>();
    audsrv_ = std::make_unique<AudSrv>(*this);
    // Módulos residentes desde o boot (IOPBTCONF do console).
    registerFileio();
    registerIopHeap();
    registerLoadfile();
    cdvd_->registerServers();
    for (const auto& [sid, srv] : servers_) bootServers_.insert(sid);
    loaded_.insert("cdvdman");
    loaded_.insert("cdvdfsv");
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
            // Reboot do IOP (SifIopReset): os módulos carregados pelo programa
            // somem; o IOP do HLE já volta pronto.
            resetModules();
            smflg_ |= kStatSifInit | kStatCmdInit | kStatBootEnd;
            return;
        case kCmdRpcBind: {
            const std::uint32_t sid = rd32(p, 32);
            auto it = servers_.find(sid);
            std::vector<std::uint8_t> reply(64, 0);
            wr32(reply, 16, rd32(p, 16));  // rec_id
            wr32(reply, 20, rd32(p, 20));  // pkt_addr
            wr32(reply, 24, rd32(p, 24));  // rpc_id
            wr32(reply, 28, rd32(p, 28));  // cd
            wr32(reply, 32, kCmdRpcBind);  // cid
            if (it == servers_.end()) {
                // Como no console: server = NULL e o EE tenta de novo. Sem
                // módulo que registre o servidor, isso nunca termina.
                if (++missingBinds_[sid] > kMaxMissingBinds) {
                    throw Unimplemented("o programa espera pelo servidor SIF RPC " + anyps2::hex(sid) + " (" +
                                            knownServerName(sid) +
                                            "), mas nenhum módulo carregado o registra — carregue o módulo "
                                            "(SifLoadModule/SifExecModuleBuffer) ou ele ainda não tem HLE",
                                        pc);
                }
                if (rt_.options().traceIop) {
                    std::fprintf(stderr, "[iop] bind %08x: servidor inexistente\n", sid);
                }
                sendToEe(kCmdRpcEnd, reply, nullptr, 0, 0, pc);
                return;
            }
            missingBinds_.erase(sid);
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
            const std::uint32_t sd = rd32(p, 52);
            const Server* server = nullptr;
            for (const auto& [sid, s] : servers_) {
                if (s.serverData == sd) server = &s;
            }
            if (!server) throw GuestError("chamada SIF RPC para servidor desconhecido " + anyps2::hex(sd), pc);
            const std::uint8_t* data = iopPointer(server->buffer, sendSize, pc);
            std::vector<std::uint8_t> in(data, data + sendSize);
            Deferred call;
            call.recvBuf = rd32(p, 40);
            call.recvSize = rd32(p, 44);
            call.rmode = rd32(p, 48);
            call.reply.assign(64, 0);
            wr32(call.reply, 16, rd32(p, 16));
            wr32(call.reply, 20, rd32(p, 20));
            wr32(call.reply, 24, rd32(p, 24));
            wr32(call.reply, 28, rd32(p, 28));
            wr32(call.reply, 32, kCmdRpcCall);
            if (rt_.options().traceIop) {
                std::fprintf(stderr, "[iop] %s.%u (%u bytes)\n", server->name.c_str(), rpcNumber, sendSize);
            }
            currentCall_ = call;
            currentDeferred_ = false;
            std::optional<std::vector<std::uint8_t>> out;
            try {
                out = server->handler(rpcNumber, in, pc);
            } catch (...) {
                currentCall_.reset();
                throw;
            }
            currentCall_.reset();
            if (out) {
                finishCall(call, *out, pc);
            } else if (!currentDeferred_) {
                throw GuestError("servidor HLE " + server->name + " não respondeu à função " +
                                     std::to_string(rpcNumber),
                                 pc);
            }
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

void Iop::finishCall(const Deferred& d, const std::vector<std::uint8_t>& out, std::uint32_t pc) {
    const std::uint32_t n = std::min<std::uint32_t>(d.recvSize, static_cast<std::uint32_t>(out.size()));
    if (d.rmode == 0) {
        // Sem RPC_END (NOWAIT sem callback): como o sceSifExecRequest do IOP,
        // copia os dados e sobrescreve o pacote do cliente com rec_id/rpc_id
        // zerados — é isso que faz sceSifCheckStatRpc devolver "terminado".
        if (n) rt_.memory().copyToGuest(d.recvBuf, out.data(), n, pc);
        std::vector<std::uint8_t> pkt = d.reply;
        const std::uint32_t pktAddr = rd32(pkt, 20);
        wr32(pkt, 16, 0);  // rec_id
        wr32(pkt, 24, 0);  // rpc_id
        if (pktAddr) rt_.memory().copyToGuest(pktAddr, pkt.data(), static_cast<std::uint32_t>(pkt.size()), pc);
        return;
    }
    sendToEe(kCmdRpcEnd, d.reply, out.data(), n, d.recvBuf, pc);
}

std::uint32_t Iop::deferCurrentCall() {
    if (!currentCall_) throw std::logic_error("deferCurrentCall fora de uma chamada RPC");
    const std::uint32_t token = nextDeferred_++;
    deferred_[token] = *currentCall_;
    currentDeferred_ = true;
    return token;
}

void Iop::completeDeferred(std::uint32_t token, const std::vector<std::uint8_t>& out, std::uint32_t pc) {
    auto it = deferred_.find(token);
    if (it == deferred_.end()) return;
    const Deferred d = std::move(it->second);
    deferred_.erase(it);
    finishCall(d, out, pc);
    deliverPending(pc);
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
    auto it = servers_.find(sid);
    if (it != servers_.end()) {  // módulo recarregado: mantém os endereços
        it->second.name = std::move(name);
        it->second.handler = std::move(handler);
        return;
    }
    Server s;
    s.sid = sid;
    s.name = std::move(name);
    s.handler = std::move(handler);
    s.buffer = nextServerBuffer_;
    s.serverData = nextServerData_;
    nextServerData_ += 0x80u;
    nextServerBuffer_ += kServerBufferSize;
    if (nextServerBuffer_ > heapStart()) {
        throw GuestError("IOP do HLE sem espaço para buffers de servidores RPC", 0);
    }
    servers_[sid] = std::move(s);
}

void Iop::vblank(std::uint32_t pc) {
    pad_->vblank(pc);
    flushAudio(pc);
}

void Iop::flushAudio(std::uint32_t pc) {
    // Amostras devidas até agora no tempo emulado.
    const std::uint64_t now = rt_.timing().now();
    const std::uint64_t target = now / Timing::kEeHz * Spu2::kRate + now % Timing::kEeHz * Spu2::kRate / Timing::kEeHz;
    if (target <= mixedFrames_) return;
    // Depois de uma pausa longa do host (relógio real) não gera minutos de som.
    if (target - mixedFrames_ > 2 * Spu2::kRate) mixedFrames_ = target - 2 * Spu2::kRate;
    constexpr std::size_t kChunk = 1024;
    std::int32_t mix[kChunk * 2];
    std::int16_t out[kChunk * 2];
    Audio* audio = rt_.audio();
    while (mixedFrames_ < target) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, target - mixedFrames_));
        std::fill(mix, mix + 2 * n, 0);
        spu2_->render(mix, n);
        audsrv_->render(mix, n);
        for (std::size_t i = 0; i < 2 * n; ++i) out[i] = static_cast<std::int16_t>(std::clamp(mix[i], -32768, 32767));
        if (audio) audio->write(out, n);
        mixedFrames_ += n;
    }
    audsrv_->update(pc);
}

std::uint32_t Iop::iopAlloc(std::uint32_t size) {
    size = (size + 255) & ~255u;  // o SYSMEM aloca em blocos de 256 bytes
    if (heapNext_ + size > kRamSize) return 0;
    const std::uint32_t addr = heapNext_;
    heapNext_ += size;
    return addr;
}

void Iop::resetModules() {
    for (auto it = servers_.begin(); it != servers_.end();) {
        if (bootServers_.count(it->first)) ++it;
        else it = servers_.erase(it);
    }
    loaded_ = {"cdvdman", "cdvdfsv"};
    missingBinds_.clear();
    pad_->reset();
    mc_->reset();
    audsrv_->reset();
    spu2_->reset();
}

// ---------------------------------------------------------------------------
// Servidor de heap do IOP (0x80000003): SifAllocIopHeap / SifFreeIopHeap /
// SifLoadIopHeap
// ---------------------------------------------------------------------------

void Iop::registerIopHeap() {
    registerServer(0x80000003u, "iopheap", [this](std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                  std::uint32_t pc) -> std::vector<std::uint8_t> {
        switch (fn) {
            case 1:  // alloc(size)
                return result32(static_cast<std::int32_t>(iopAlloc(rd32(in, 0))));
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
                                "\" não suportado pelo fileio do HLE (cartões de memória: use o libmc)",
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
                if (std::string rel; cdPath(name, rel)) {
                    if ((mode & 3) != kIoRdOnly) return result32(-kEROFS);
                    const auto e = cdvd_->image("open(\"" + name + "\")", pc).lookup(rel);
                    if (!e || e->isDir) return result32(-kENOENT);
                    OpenFile f;
                    f.path = name;
                    f.cd = true;
                    f.cdLsn = e->lsn;
                    f.cdSize = e->size;
                    const std::int32_t fd = nextFd_++;
                    files_[fd] = std::move(f);
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
                if (it == files_.end() || it->second.isDir) return result32(-kEBADF);
                if (it->second.cd) return result32(-kEROFS);
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
                if (it == files_.end() || it->second.isDir) return result32(-kEBADF);
                const std::uint32_t ptr = rd32(in, 4), size = rd32(in, 8), readData = rd32(in, 12);
                std::vector<std::uint8_t> buf(size);
                std::size_t got = 0;
                if (OpenFile& f = it->second; f.cd) {
                    got = std::min(size, f.cdSize - std::min(f.cdPos, f.cdSize));
                    if (got && !cdvd_->image("read", pc).readBytes(f.cdLsn, f.cdPos,
                                                                   static_cast<std::uint32_t>(got), buf.data())) {
                        throw GuestError("leitura de " + f.path + " passa do fim da imagem de disco", pc);
                    }
                    f.cdPos += static_cast<std::uint32_t>(got);
                } else {
                    got = std::fread(buf.data(), 1, size, f.fp);
                }
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
                if (it == files_.end() || it->second.console || it->second.isDir) return result32(-kEBADF);
                const auto off = static_cast<std::int32_t>(rd32(in, 4));
                const std::uint32_t whence = rd32(in, 8);
                if (whence > 2) return result32(-kEINVAL);
                if (OpenFile& f = it->second; f.cd) {
                    const std::int64_t base = whence == 0 ? 0 : whence == 1 ? f.cdPos : f.cdSize;
                    const std::int64_t pos = base + off;
                    if (pos < 0) return result32(-kEINVAL);
                    f.cdPos = static_cast<std::uint32_t>(std::min<std::int64_t>(pos, 0xFFFFFFFF));
                    return result32(static_cast<std::int32_t>(f.cdPos));
                }
                if (std::fseek(it->second.fp, off, static_cast<int>(whence)) != 0) return result32(-kEINVAL);
                return result32(static_cast<std::int32_t>(std::ftell(it->second.fp)));
            }
            case FIO_GETSTAT: {  // {io_stat_t* buf; char name[256]}
                const std::uint32_t buf = rd32(in, 0);
                const std::string name = cstr(in, 4, 256);
                if (std::string rel; cdPath(name, rel)) {
                    const auto e = cdvd_->image("getstat(\"" + name + "\")", pc).lookup(rel);
                    if (!e) return result32(-kENOENT);
                    const auto st = cdStat(*e);
                    m.copyToGuest(buf, st.data(), static_cast<std::uint32_t>(st.size()), pc);
                    return result32(0);
                }
                bool ok = false;
                const auto st = ioStat(hostPath(name, pc), ok);
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
            case FIO_MKDIR: {  // union {char name[256]; int result}
                std::error_code ec;
                std::filesystem::create_directory(hostPath(cstr(in, 0, 256), pc), ec);
                return result32(ec ? -kENOENT : 0);
            }
            case FIO_DOPEN: {  // union {char name[256]; int result}
                const std::string name = cstr(in, 0, 256);
                if (std::string rel; cdPath(name, rel)) {
                    IsoImage& iso = cdvd_->image("dopen(\"" + name + "\")", pc);
                    const auto e = iso.lookup(rel);
                    if (!e || !e->isDir) return result32(-kENOENT);
                    OpenFile f;
                    f.path = rel;
                    f.isDir = true;
                    f.cd = true;
                    for (const auto& c : iso.list(*e)) f.entries.push_back(c.name);
                    const std::int32_t fd = nextFd_++;
                    files_[fd] = std::move(f);
                    return result32(fd);
                }
                const auto path = hostPath(name, pc);
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
                std::vector<std::uint8_t> dirent;  // io_stat_t + nome
                if (d.cd) {
                    const auto e = cdvd_->image("dread", pc).lookup(d.path + "\\" + name);
                    dirent = e ? cdStat(*e) : std::vector<std::uint8_t>(40, 0);
                } else {
                    dirent = ioStat(std::filesystem::path(d.path) / name, ok);
                }
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
