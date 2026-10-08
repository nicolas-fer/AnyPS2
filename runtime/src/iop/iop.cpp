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
#include "anyps2/runtime/iop/dbcman.h"
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
// Buffer de recepção de cada servidor RPC na RAM do IOP (o maior pedido
// conhecido tem 0x2090 bytes: troca grande do dbcman). 16 KB cabem 60
// servidores entre 0x10000 e o heap.
constexpr std::uint32_t kServerBufferSize = 0x4000u;
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

// Arquivos da ROM do console que programas leem como dados (não são
// conteúdo de ROM: só identificadores sintéticos). ROMVER = versão 2.30,
// região A (EUA/NTSC), console de varejo (C), 2008-02-20.
const std::vector<std::uint8_t>* syntheticRomFile(const std::string& path) {
    static const std::vector<std::uint8_t> kRomver = [] {
        std::vector<std::uint8_t> v(16, 0);
        const char text[] = "0230AC20080220";
        std::memcpy(v.data(), text, sizeof(text) - 1);
        return v;
    }();
    const auto colon = path.find(':');
    if (colon == std::string::npos) return nullptr;
    const std::string device = path.substr(0, colon);
    if (device != "rom" && device != "rom0") return nullptr;
    std::string name = path.substr(colon + 1);
    while (!name.empty() && (name[0] == '/' || name[0] == '\\')) name.erase(0, 1);
    if (name == "ROMVER") return &kRomver;
    return nullptr;
}

bool isRomPath(const std::string& path) {
    return path.rfind("rom:", 0) == 0 || path.rfind("rom0:", 0) == 0;
}

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
    dbc_ = std::make_unique<DbcMan>(*this);
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
        case kRegSmFlag: return smflag();
        default: {
            auto it = sysregs_.find(reg);
            return it == sysregs_.end() ? 0 : it->second;
        }
    }
}

// O reboot do IOP (SifIopReset) termina na primeira leitura do SMFLAG depois
// do reset: logo depois de mandar o reset, o EE limpa SIFINIT/CMDINIT (no
// console o IOP ainda está reiniciando) e só então espera as flags voltarem.
// Ligá-las já no reset faria o EE apagá-las e esperar para sempre.
std::uint32_t Iop::smflag() const {
    if (rebootPending_) {
        rebootPending_ = false;
        smflg_ |= kStatSifInit | kStatCmdInit | kStatBootEnd;
    }
    return smflg_;
}

std::uint32_t Iop::sifReadRegister(std::uint32_t addr) const {
    switch (addr) {
        case 0x1000F200: return mscom_;
        case 0x1000F210: return smcom_;
        case 0x1000F220: return msflg_;
        case 0x1000F230: return smflag();
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
            // somem; as flags de "pronto" voltam na próxima leitura (smflag()).
            resetModules();
            smflg_ &= ~(kStatSifInit | kStatCmdInit | kStatBootEnd);
            rebootPending_ = true;
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
            if (sendSize > kServerBufferSize) {
                throw GuestError("chamada SIF RPC ao servidor " + server->name + " com " + std::to_string(sendSize) +
                                     " bytes: maior que o buffer de recepção do HLE (" +
                                     std::to_string(kServerBufferSize) + ")",
                                 pc);
            }
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
    const auto psize = static_cast<std::uint32_t>(packet.size());
    wr32(packet, 0, (psize & 0xFF) | (extraSize << 8));
    wr32(packet, 4, extraSize ? extraDest : 0);
    wr32(packet, 8, cid);
    // Os dados vão para o EE junto com o comando (como o DMA do SIF), não
    // antes: um buffer do EE reaproveitado só é sobrescrito depois que o
    // comando anterior foi tratado.
    Pending p;
    p.packet = std::move(packet);
    if (extraSize > 0) p.extra.assign(extra, extra + extraSize);
    p.extraDest = extraDest;
    pending_.push_back(std::move(p));
}

void Iop::deliverPending(std::uint32_t pc) {
    if (delivering_) return;  // um handler está rodando; entrega ao terminar
    delivering_ = true;
    try {
        while (!pending_.empty() && rt_.kernel().canDeliverDmac(kDmacSif0)) {
            Pending next = std::move(pending_.front());
            pending_.erase(pending_.begin());
            if (!next.extra.empty()) {
                rt_.memory().copyToGuest(next.extraDest, next.extra.data(),
                                         static_cast<std::uint32_t>(next.extra.size()), pc);
            }
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
    dbc_->vblank(pc);
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
    // O fileio recarregado volta ao protocolo inicial até receber a função 255.
    fioSce_ = false;
    fioSceNext_ = 0;
    pad_->reset();
    dbc_->reset();
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
                if (const auto* rom = syntheticRomFile(path)) {
                    const auto n = static_cast<std::uint32_t>(rom->size());
                    std::memcpy(iopPointer(addr, n, pc), rom->data(), n);
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

// ---- Operações do fileio, comuns aos dois protocolos -----------------------

std::int32_t Iop::fioOpen(const std::string& name, std::uint32_t flags, std::uint32_t pc) {
    if (name.rfind("tty", 0) == 0) {
        const std::int32_t fd = nextFd_++;
        files_[fd] = makeFile(stdout, true, name);
        return fd;
    }
    if (isRomPath(name)) {
        const auto* rom = syntheticRomFile(name);
        if (!rom) {
            throw Unimplemented("open(\"" + name + "\"): arquivo da ROM do console que o HLE não fornece", pc);
        }
        if ((flags & 3) != kIoRdOnly) return -kEROFS;
        OpenFile f;
        f.path = name;
        f.rom = rom;
        f.cdSize = static_cast<std::uint32_t>(rom->size());
        const std::int32_t fd = nextFd_++;
        files_[fd] = std::move(f);
        return fd;
    }
    if (std::string rel; cdPath(name, rel)) {
        if ((flags & 3) != kIoRdOnly) return -kEROFS;
        const auto e = cdvd_->image("open(\"" + name + "\")", pc).lookup(rel);
        if (!e || e->isDir) return -kENOENT;
        OpenFile f;
        f.path = name;
        f.cd = true;
        f.cdLsn = e->lsn;
        f.cdSize = e->size;
        const std::int32_t fd = nextFd_++;
        files_[fd] = std::move(f);
        return fd;
    }
    const std::filesystem::path path = hostPath(name, pc);
    std::string fmode;
    const std::uint32_t acc = flags & 3;
    if (acc == kIoRdOnly) fmode = "rb";
    else if (flags & kIoAppend) fmode = acc == kIoWrOnly ? "ab" : "a+b";
    else if (flags & (kIoTrunc | kIoCreat)) fmode = acc == kIoWrOnly ? "wb" : "w+b";
    else fmode = "r+b";
    std::FILE* fp = std::fopen(path.string().c_str(), fmode.c_str());
    if (!fp) return -kENOENT;
    const std::int32_t fd = nextFd_++;
    files_[fd] = makeFile(fp, false, path.string());
    return fd;
}

std::int32_t Iop::fioClose(std::int32_t fd) {
    auto it = files_.find(fd);
    if (it == files_.end() || it->second.isDir) return -kEBADF;
    if (it->second.fp && !it->second.console) std::fclose(it->second.fp);
    files_.erase(it);
    return 0;
}

std::int32_t Iop::fioWrite(std::int32_t fd, const std::uint8_t* head, std::uint32_t headSize, std::uint32_t ptr,
                           std::uint32_t size, std::uint32_t pc) {
    auto it = files_.find(fd);
    if (it == files_.end() || it->second.isDir) return -kEBADF;
    if (it->second.cd || it->second.rom) return -kEROFS;
    std::vector<std::uint8_t> buf(size);
    const std::uint32_t h = std::min(headSize, size);
    if (h) std::memcpy(buf.data(), head, h);
    if (size > h) rt_.memory().copyFromGuest(buf.data() + h, ptr + h, size - h, pc);
    const std::size_t written = std::fwrite(buf.data(), 1, size, it->second.fp);
    if (it->second.console) std::fflush(it->second.fp);
    return static_cast<std::int32_t>(written);
}

std::int32_t Iop::fioRead(std::int32_t fd, std::uint32_t ptr, std::uint32_t size, std::uint32_t pc) {
    auto it = files_.find(fd);
    if (it == files_.end() || it->second.isDir) return -kEBADF;
    std::vector<std::uint8_t> buf(size);
    std::size_t got = 0;
    if (OpenFile& f = it->second; f.rom) {
        got = std::min(size, f.cdSize - std::min(f.cdPos, f.cdSize));
        if (got) std::memcpy(buf.data(), f.rom->data() + f.cdPos, got);
        f.cdPos += static_cast<std::uint32_t>(got);
    } else if (f.cd) {
        got = std::min(size, f.cdSize - std::min(f.cdPos, f.cdSize));
        if (got && !cdvd_->image("read", pc).readBytes(f.cdLsn, f.cdPos, static_cast<std::uint32_t>(got), buf.data())) {
            throw GuestError("leitura de " + f.path + " passa do fim da imagem de disco", pc);
        }
        f.cdPos += static_cast<std::uint32_t>(got);
    } else {
        got = std::fread(buf.data(), 1, size, f.fp);
    }
    if (got) rt_.memory().copyToGuest(ptr, buf.data(), static_cast<std::uint32_t>(got), pc);
    return static_cast<std::int32_t>(got);
}

std::int32_t Iop::fioLseek(std::int32_t fd, std::int32_t offset, std::uint32_t whence) {
    auto it = files_.find(fd);
    if (it == files_.end() || it->second.console || it->second.isDir) return -kEBADF;
    if (whence > 2) return -kEINVAL;
    if (OpenFile& f = it->second; f.cd || f.rom) {
        const std::int64_t base = whence == 0 ? 0 : whence == 1 ? f.cdPos : f.cdSize;
        const std::int64_t pos = base + offset;
        if (pos < 0) return -kEINVAL;
        f.cdPos = static_cast<std::uint32_t>(std::min<std::int64_t>(pos, 0xFFFFFFFF));
        return static_cast<std::int32_t>(f.cdPos);
    }
    if (std::fseek(it->second.fp, offset, static_cast<int>(whence)) != 0) return -kEINVAL;
    return static_cast<std::int32_t>(std::ftell(it->second.fp));
}

std::int32_t Iop::fioGetstat(const std::string& name, std::vector<std::uint8_t>& stat, std::uint32_t pc) {
    if (isRomPath(name)) {
        const auto* rom = syntheticRomFile(name);
        if (!rom) {
            throw Unimplemented("getstat(\"" + name + "\"): arquivo da ROM do console que o HLE não fornece", pc);
        }
        stat.assign(40, 0);
        writeLE32(stat, 0, 0x0010u | 0x0004u);  // FIO_SO_IFREG, só leitura
        writeLE32(stat, 8, static_cast<std::uint32_t>(rom->size()));
        return 0;
    }
    if (std::string rel; cdPath(name, rel)) {
        const auto e = cdvd_->image("getstat(\"" + name + "\")", pc).lookup(rel);
        if (!e) return -kENOENT;
        stat = cdStat(*e);
        return 0;
    }
    bool ok = false;
    stat = ioStat(hostPath(name, pc), ok);
    return ok ? 0 : -kENOENT;
}

std::int32_t Iop::fioRemove(const std::string& name, std::uint32_t pc) {
    std::error_code ec;
    return std::filesystem::remove(hostPath(name, pc), ec) ? 0 : -kENOENT;
}

std::int32_t Iop::fioMkdir(const std::string& name, std::uint32_t pc) {
    std::error_code ec;
    std::filesystem::create_directory(hostPath(name, pc), ec);
    return ec ? -kENOENT : 0;
}

std::int32_t Iop::fioDopen(const std::string& name, std::uint32_t pc) {
    if (std::string rel; cdPath(name, rel)) {
        IsoImage& iso = cdvd_->image("dopen(\"" + name + "\")", pc);
        const auto e = iso.lookup(rel);
        if (!e || !e->isDir) return -kENOENT;
        OpenFile f;
        f.path = rel;
        f.isDir = true;
        f.cd = true;
        for (const auto& c : iso.list(*e)) f.entries.push_back(c.name);
        const std::int32_t fd = nextFd_++;
        files_[fd] = std::move(f);
        return fd;
    }
    const auto path = hostPath(name, pc);
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec)) return -kENOENT;
    OpenFile f;
    f.path = path.string();
    f.isDir = true;
    for (const auto& e : std::filesystem::directory_iterator(path, ec)) {
        f.entries.push_back(e.path().filename().string());
    }
    const std::int32_t fd = nextFd_++;
    files_[fd] = std::move(f);
    return fd;
}

std::int32_t Iop::fioDclose(std::int32_t fd) {
    auto it = files_.find(fd);
    if (it == files_.end() || !it->second.isDir) return -kEBADF;
    files_.erase(it);
    return 0;
}

std::int32_t Iop::fioDread(std::int32_t fd, std::string& name, std::vector<std::uint8_t>& stat, std::uint32_t pc) {
    auto it = files_.find(fd);
    if (it == files_.end() || !it->second.isDir) return -kEBADF;
    OpenFile& d = it->second;
    if (d.nextEntry >= d.entries.size()) return 0;
    name = d.entries[d.nextEntry++];
    if (d.cd) {
        const auto e = cdvd_->image("dread", pc).lookup(d.path + "\\" + name);
        stat = e ? cdStat(*e) : std::vector<std::uint8_t>(40, 0);
    } else {
        bool ok = false;
        stat = ioStat(std::filesystem::path(d.path) / name, ok);
    }
    return 1;
}

// ---- Protocolo do ps2sdk (fileio-common.h) ---------------------------------

std::vector<std::uint8_t> Iop::fileioSdk(std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc) {
    Memory& m = rt_.memory();
    const auto fd = static_cast<std::int32_t>(rd32(in, 0));
    switch (fn) {
        case FIO_OPEN:  // {int mode; char name[256]}
            return result32(fioOpen(cstr(in, 4, 256), rd32(in, 0), pc));
        case FIO_CLOSE: return result32(fioClose(fd));
        case FIO_WRITE: {  // {fd, ptr, size, mis, aligned[16]}
            const std::uint32_t mis = std::min<std::uint32_t>(rd32(in, 12), 16);
            const std::uint8_t* head = in.size() >= 16 + mis ? in.data() + 16 : nullptr;
            return result32(fioWrite(fd, head, head ? mis : 0, rd32(in, 4), rd32(in, 8), pc));
        }
        case FIO_READ: {  // {fd, ptr, size, _fio_read_data*}
            const std::int32_t got = fioRead(fd, rd32(in, 4), rd32(in, 8), pc);
            // _fio_read_intr copia buf1/buf2 para as pontas desalinhadas; já
            // escrevemos tudo, então os tamanhos são zero.
            if (const std::uint32_t readData = rd32(in, 12); readData && got >= 0) {
                const std::uint32_t zero[4] = {0, 0, 0, 0};
                m.copyToGuest(readData, zero, sizeof(zero), pc);
            }
            return result32(got);
        }
        case FIO_LSEEK: return result32(fioLseek(fd, static_cast<std::int32_t>(rd32(in, 4)), rd32(in, 8)));
        case FIO_GETSTAT: {  // {io_stat_t* buf; char name[256]}
            std::vector<std::uint8_t> st;
            const std::int32_t r = fioGetstat(cstr(in, 4, 256), st, pc);
            if (r == 0) m.copyToGuest(rd32(in, 0), st.data(), static_cast<std::uint32_t>(st.size()), pc);
            return result32(r);
        }
        case FIO_REMOVE:
        case FIO_RMDIR: return result32(fioRemove(cstr(in, 0, 256), pc));
        case FIO_MKDIR: return result32(fioMkdir(cstr(in, 0, 256), pc));  // union {name; result}
        case FIO_DOPEN: return result32(fioDopen(cstr(in, 0, 256), pc));
        case FIO_DCLOSE: return result32(fioDclose(fd));
        case FIO_DREAD: {  // {int fd; io_dirent_t* buf} -> 1 se leu, 0 no fim
            std::string name;
            std::vector<std::uint8_t> dirent;  // io_stat_t + nome
            const std::int32_t r = fioDread(fd, name, dirent, pc);
            if (r == 1) {
                dirent.resize(40 + 256 + 4, 0);
                std::memcpy(dirent.data() + 40, name.c_str(), std::min<std::size_t>(name.size(), 255));
                m.copyToGuest(rd32(in, 4), dirent.data(), static_cast<std::uint32_t>(dirent.size()), pc);
            }
            return result32(r);
        }
        default:
            throw Unimplemented(std::string("fileio.") + fioName(fn) + " ainda não implementado no HLE do IOP", pc);
    }
}

// ---- Protocolo do fileio da Sony (SDK 3.0, módulo FILEIO_service 2.x) -------
//
// O EE registra dois buffers de conclusão (função 255) e um handler para o
// comando SIF 0x80000011. Cada pedido começa com {sema, endereço do
// resultado, tamanho do resultado}; a resposta do RPC só diz "aceito". Ao
// terminar, o IOP escreve no buffer {sema, função, endereço, tamanho,
// resultado, extras da função} e manda 0x80000011 com o índice do buffer em
// `opt`; o handler do EE copia o resultado (e dirent/stat) e faz iSignalSema
// (ou, com sema < 0 — modo NOWAIT —, só marca o pedido como concluído).
// No HLE a operação termina na hora e os dados de read vão direto ao destino.

namespace {
constexpr std::uint32_t kCmdFileioDone = 0x80000011u;

// io_stat_t (bits FIO_SO_* do ioman antigo) → iox_stat_t de 64 bytes (bits
// FIO_S_* do iomanX: 0x1000 diretório, 0x2000 arquivo, rwx em 0x1FF).
std::vector<std::uint8_t> ioxStat(std::vector<std::uint8_t> st) {
    st.resize(64, 0);
    const std::uint32_t old = readLE32(st, 0);
    std::uint32_t mode = (old & 0x20) ? 0x1000u : (old & 0x10) ? 0x2000u : 0;
    if (old & 4) mode |= 0x124;  // r--r--r--
    if (old & 2) mode |= 0x092;  // -w--w--w-
    if (old & 1) mode |= 0x049;  // --x--x--x
    writeLE32(st, 0, mode);
    return st;
}
}  // namespace

std::vector<std::uint8_t> Iop::fileioSce(std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc) {
    const auto fd = static_cast<std::int32_t>(rd32(in, 12));
    std::int32_t result = 0;
    std::vector<std::uint8_t> extra;  // a partir do offset 20 do buffer de conclusão
    switch (fn) {
        case FIO_OPEN:  // [12] flags, [16] modo, [20..1043] nome, [1044] slot do EE
            result = fioOpen(cstr(in, 20, 1024), rd32(in, 12), pc);
            break;
        case FIO_CLOSE: result = fioClose(fd); break;
        case FIO_READ:  // [12] fd, [16] buffer, [20] tamanho
            result = fioRead(fd, rd32(in, 16), rd32(in, 20), pc);
            extra.assign(16, 0);  // tamanhos/destinos das pontas desalinhadas: nenhuma
            break;
        case FIO_WRITE: {  // [12] fd, [16] buffer, [20] tamanho, [24] cabeça desalinhada, [28..43] bytes dela
            const std::uint32_t mis = std::min<std::uint32_t>(rd32(in, 24), 16);
            const std::uint8_t* head = in.size() >= 28 + mis ? in.data() + 28 : nullptr;
            result = fioWrite(fd, head, head ? mis : 0, rd32(in, 16), rd32(in, 20), pc);
            break;
        }
        case FIO_LSEEK:  // [12] fd, [16] offset, [20] whence
            result = fioLseek(fd, static_cast<std::int32_t>(rd32(in, 16)), rd32(in, 20));
            break;
        case FIO_REMOVE:
        case FIO_RMDIR: result = fioRemove(cstr(in, 12, 1024), pc); break;  // [12..] nome
        case FIO_DOPEN: result = fioDopen(cstr(in, 12, 1024), pc); break;
        case FIO_DCLOSE: result = fioDclose(fd); break;
        case FIO_DREAD: {  // [12] fd, [16] iox_dirent_t* → extras {destino, iox_dirent_t (324 bytes)}
            std::string name;
            std::vector<std::uint8_t> st;
            result = fioDread(fd, name, st, pc);
            extra.assign(4 + 324, 0);
            writeLE32(extra, 0, rd32(in, 16));
            if (result == 1) {
                const auto x = ioxStat(st);
                std::memcpy(extra.data() + 4, x.data(), x.size());
                std::memcpy(extra.data() + 4 + 64, name.c_str(), std::min<std::size_t>(name.size(), 255));
            }
            break;
        }
        case FIO_GETSTAT: {  // [12] iox_stat_t*, [16..] nome → extras {destino, iox_stat_t}
            std::vector<std::uint8_t> st;
            result = fioGetstat(cstr(in, 16, 1024), st, pc);
            extra.assign(4 + 64, 0);
            writeLE32(extra, 0, rd32(in, 12));
            if (result == 0) {
                const auto x = ioxStat(st);
                std::memcpy(extra.data() + 4, x.data(), x.size());
            }
            break;
        }
        case 23: {  // devctl: [12] nome[1024], [1036] comando, [1040] arg[1024], [2064] tam. arg, [2068] tam. saída
            const std::string name = cstr(in, 12, 1024);
            const std::string device = name.substr(0, name.find(':'));
            const std::uint32_t cmd = rd32(in, 1036);
            // Console sem adaptador de rede/HDD: o dev9 não registra esses
            // dispositivos e o iomanX responde -ENODEV.
            if (device == "dev9x" || device == "hdd" || device == "hdd0" || device == "pfs" || device == "pfs0") {
                if (rt_.options().traceIop) {
                    std::fprintf(stderr, "[iop] devctl(\"%s\", 0x%x): sem adaptador de rede/HDD -> -ENODEV\n",
                                 name.c_str(), cmd);
                }
                result = -19;  // ENODEV
                extra.assign(8, 0);  // {destino, tamanho} da saída: nada
                break;
            }
            throw Unimplemented("devctl(\"" + name + "\", " + anyps2::hex(cmd) +
                                    ") do fileio (SDK 3.0) ainda não implementado no HLE do IOP",
                                pc);
        }
        default: {
            static const char* kNames[] = {"open", "close", "read", "write", "lseek", "ioctl", "remove",
                                           "mkdir", "rmdir", "dopen", "dclose", "dread", "getstat", "chstat",
                                           "format", "adddrv", "deldrv", "rename", "chdir", "sync", "mount",
                                           "umount", "lseek64", "devctl", "symlink", "readlink", "ioctl2"};
            // Diagnóstico: começo do pedido e o texto em [12] (onde ficam os nomes).
            std::string dump;
            for (std::size_t i = 0; i < std::min<std::size_t>(in.size(), 32); ++i) {
                char b[4];
                std::snprintf(b, sizeof(b), "%02x", in[i]);
                dump += b;
                if (i % 4 == 3) dump += ' ';
            }
            throw Unimplemented(std::string("fileio (protocolo da Sony, SDK 3.0): função ") + std::to_string(fn) +
                                    (fn < 27 ? std::string(" (") + kNames[fn] + ")" : std::string()) +
                                    " ainda não implementada no HLE do IOP; pedido de " + std::to_string(in.size()) +
                                    " bytes: " + dump + "... texto em [12]: \"" + cstr(in, 12, 64) + "\"",
                                pc);
        }
    }
    // Buffer de conclusão: {sema, função, destino, 4, resultado, extras}
    std::vector<std::uint8_t> done(20, 0);
    wr32(done, 0, rd32(in, 0));
    wr32(done, 4, fn);
    wr32(done, 8, rd32(in, 4));
    wr32(done, 12, std::min<std::uint32_t>(rd32(in, 8), 4));
    wr32(done, 16, static_cast<std::uint32_t>(result));
    done.insert(done.end(), extra.begin(), extra.end());
    const unsigned index = fioSceNext_;
    fioSceNext_ ^= 1;
    std::vector<std::uint8_t> packet(16, 0);
    wr32(packet, 12, index);  // opt: qual dos dois buffers
    sendToEe(kCmdFileioDone, std::move(packet), done.data(), static_cast<std::uint32_t>(done.size()),
             fioSceBuffers_[index], pc);
    return result32(1);  // pedido aceito
}

void Iop::registerFileio() {
    registerServer(0x80000001u, "fileio", [this](std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                 std::uint32_t pc) -> std::vector<std::uint8_t> {
        if (rt_.options().traceIop) std::fprintf(stderr, "[iop] fileio.%s (%u)\n", fioName(fn), fn);
        switch (fn) {
            case 255: {  // Sony: registra os buffers de conclusão; responde {versão, nº de buffers}
                fioSce_ = true;
                fioSceBuffers_[0] = rd32(in, 0);
                fioSceBuffers_[1] = rd32(in, 4);
                fioSceNext_ = 0;
                std::vector<std::uint8_t> out = {'3', '0', '0', '0'};
                wr32(out, 4, 2);
                return out;
            }
            case 254:  // Sony: tamanho/quantidade de buffers do lado do IOP — nada a fazer no HLE
            case 253:
                return result32(0);
            default:
                return fioSce_ ? fileioSce(fn, in, pc) : fileioSdk(fn, in, pc);
        }
    });
}

}  // namespace anyps2::rt
