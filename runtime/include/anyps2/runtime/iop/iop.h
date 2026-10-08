#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "anyps2/runtime/iop/irx.h"

namespace anyps2::rt {

class Runtime;
class PadMan;
class McServ;
class Cdvd;
class Spu2;
class AudSrv;
class DbcMan;

// IOP em HLE, visto pelo EE através do SIF.
//
// O EE fala com o IOP por "comandos SIF": pacotes enviados por DMA (syscall
// sceSifSetDma) para um buffer na RAM do IOP. Aqui o IOP não executa código:
// cada pacote é interpretado na hora (protocolo de ps2sdk/sifcmd e sifrpc) e
// a resposta é escrita direto no buffer de recepção do EE, seguida da
// interrupção do canal DMA SIF0 — o handler do próprio programa
// (_SifCmdIntHandler) roda e sinaliza os semáforos de quem espera.
//
// Os módulos do IOP (IRX) são reimplementados em HLE e identificados pelo
// nome gravado no próprio IRX (ou pelo nome em rom0:). Carregar um módulo
// registra os servidores RPC dele. Presentes desde o boot (como no console):
// fileio, iopheap, loadfile e cdvdman/cdvdfsv. Módulo sem implementação HLE
// é erro explícito com nome e versão.
class Iop {
public:
    static constexpr std::uint32_t kRamSize = 2u * 1024 * 1024;

    explicit Iop(Runtime& rt);
    ~Iop();

    Runtime& runtime() { return rt_; }

    // ---- Syscalls do EE --------------------------------------------------
    std::uint32_t sifSetDma(std::uint32_t transfers, std::uint32_t count, std::uint32_t pc);
    std::uint32_t sifSetReg(std::uint32_t reg, std::uint32_t value);
    std::uint32_t sifGetReg(std::uint32_t reg) const;

    // ---- Registradores SBUS (0x1000F200...) ------------------------------
    std::uint32_t sifReadRegister(std::uint32_t addr) const;
    void sifWriteRegister(std::uint32_t addr, std::uint32_t value);

    std::uint8_t* ram() { return ram_.data(); }
    std::uint8_t* iopPointer(std::uint32_t addr, std::uint32_t size, std::uint32_t pc);

    // Entrega ao EE os pacotes pendentes (se a interrupção SIF0 puder ser
    // atendida agora). Chamado após DMAs, syscalls e EI.
    void deliverPending(std::uint32_t pc);

    // A cada VBlank: módulos que o IOP real atualiza periodicamente (pad),
    // e o áudio avança até o tempo atual.
    void vblank(std::uint32_t pc);
    // Mixa o som até o instante atual (também chamado no fim do programa).
    void flushAudio(std::uint32_t pc);

    // ---- Servidores RPC em HLE -------------------------------------------
    // Recebe o número da função e os dados enviados pelo EE; devolve os dados
    // de resposta. std::nullopt = a resposta vem depois: o handler chamou
    // deferCurrentCall() e mais tarde chama completeDeferred().
    using RpcHandler = std::function<std::optional<std::vector<std::uint8_t>>(
        std::uint32_t function, const std::vector<std::uint8_t>& data, std::uint32_t pc)>;
    void registerServer(std::uint32_t sid, std::string name, RpcHandler handler);
    bool hasServer(std::uint32_t sid) const { return servers_.count(sid) != 0; }
    std::uint32_t deferCurrentCall();
    void completeDeferred(std::uint32_t token, const std::vector<std::uint8_t>& out, std::uint32_t pc);

    // ---- Módulos -----------------------------------------------------------
    // Carrega um módulo pelo nome do IRX (ex.: "padman") ou rom0:NOME.
    // Devolve o id do módulo (> 0). Erro claro se não houver HLE.
    std::int32_t loadModule(const std::string& name, std::uint16_t version, const std::string& origin,
                            std::uint32_t pc);
    bool moduleLoaded(const std::string& hleName) const { return loaded_.count(hleName) != 0; }
    // Lê um arquivo dos dispositivos do IOP (host:, cdrom0:) inteiro.
    std::optional<std::vector<std::uint8_t>> readDeviceFile(const std::string& path, std::uint32_t pc);
    // Aloca na RAM do IOP (heap do HLE). 0 se não couber.
    std::uint32_t iopAlloc(std::uint32_t size);

    PadMan& pad() { return *pad_; }
    McServ& mc() { return *mc_; }
    Cdvd& cdvd() { return *cdvd_; }
    Spu2& spu2() { return *spu2_; }
    AudSrv& audsrv() { return *audsrv_; }
    DbcMan& dbc() { return *dbc_; }

private:
    struct Server {
        std::uint32_t sid;
        std::string name;
        RpcHandler handler;
        std::uint32_t buffer;      // buffer de recepção na RAM do IOP
        std::uint32_t serverData;  // "endereço" do SifRpcServerData_t no IOP
    };
    struct Deferred {
        std::vector<std::uint8_t> reply;  // RPC_END já montado
        std::uint32_t recvBuf = 0, recvSize = 0, rmode = 0;
    };

    void handleCommand(const std::vector<std::uint8_t>& packet, std::uint32_t pc);
    void sendToEe(std::uint32_t cid, std::vector<std::uint8_t> packet, const std::uint8_t* extra,
                  std::uint32_t extraSize, std::uint32_t extraDest, std::uint32_t pc);
    void finishCall(const Deferred& d, const std::vector<std::uint8_t>& out, std::uint32_t pc);
    void registerFileio();
    void registerIopHeap();
    void registerLoadfile();
    void resetModules();
    // Resolve "host:caminho" para um caminho do host (relativo a ANYPS2_HOST_DIR).
    std::filesystem::path hostPath(const std::string& name, std::uint32_t pc) const;

    Runtime& rt_;
    std::vector<std::uint8_t> ram_;
    std::uint32_t mscom_ = 0, smcom_ = 0, msflg_ = 0, ctrl_ = 0, bd6_ = 0;
    mutable std::uint32_t smflg_ = 0;
    mutable bool rebootPending_ = false;  // SifIopReset recebido, flags ainda não voltaram
    std::uint32_t smflag() const;
    std::map<std::uint32_t, std::uint32_t> sysregs_;
    std::uint32_t eeCmdBuffer_ = 0;  // onde o EE quer receber os pacotes
    std::map<std::uint32_t, Server> servers_;
    std::uint32_t nextServerBuffer_ = 0x00010000u;
    std::uint32_t nextServerData_ = 0x00008000u;
    std::uint32_t nextDmaId_ = 1;
    std::uint32_t heapNext_ = 0x00100000u;  // heap do IOP no HLE (1 MB..2 MB)
    // Pacotes para o EE aguardando entrega (um por vez no buffer do EE).
    struct Pending {
        std::vector<std::uint8_t> packet;
        std::vector<std::uint8_t> extra;  // dados copiados para extraDest na entrega
        std::uint32_t extraDest = 0;
    };
    std::vector<Pending> pending_;
    bool delivering_ = false;
    // Chamada RPC em andamento (para deferCurrentCall) e as adiadas.
    std::optional<Deferred> currentCall_;
    bool currentDeferred_ = false;
    std::map<std::uint32_t, Deferred> deferred_;
    std::uint32_t nextDeferred_ = 1;
    // Binds a servidores inexistentes seguidos (o EE repete até o servidor
    // aparecer; sem módulo que o registre, isso é um laço infinito).
    std::map<std::uint32_t, std::uint32_t> missingBinds_;

    // Módulos
    std::set<std::string> loaded_;      // nomes HLE carregados
    std::set<std::uint32_t> bootServers_;
    std::int32_t nextModuleId_ = 32;
    std::uint64_t mixedFrames_ = 0;  // amostras de 48 kHz já mixadas

    std::unique_ptr<PadMan> pad_;
    std::unique_ptr<McServ> mc_;
    std::unique_ptr<Cdvd> cdvd_;
    std::unique_ptr<Spu2> spu2_;
    std::unique_ptr<AudSrv> audsrv_;
    std::unique_ptr<DbcMan> dbc_;

    // fileio
    struct OpenFile {
        std::FILE* fp = nullptr;
        bool console = false;
        std::string path;
        bool isDir = false;
        std::vector<std::string> entries;
        std::size_t nextEntry = 0;
        // Arquivo do disco (cdrom0:): lido da imagem ISO.
        bool cd = false;
        std::uint32_t cdLsn = 0, cdSize = 0, cdPos = 0;
        // Arquivo sintético da ROM (rom0:ROMVER): conteúdo em memória; usa
        // cdSize/cdPos como tamanho e posição.
        const std::vector<std::uint8_t>* rom = nullptr;
    };
    static OpenFile makeFile(std::FILE* fp, bool console, std::string path);
    std::map<std::int32_t, OpenFile> files_;
    std::int32_t nextFd_ = 3;
    // Operações comuns aos dois protocolos (resultado < 0 = -errno).
    std::int32_t fioOpen(const std::string& name, std::uint32_t flags, std::uint32_t pc);
    std::int32_t fioClose(std::int32_t fd);
    std::int32_t fioRead(std::int32_t fd, std::uint32_t ptr, std::uint32_t size, std::uint32_t pc);
    std::int32_t fioWrite(std::int32_t fd, const std::uint8_t* head, std::uint32_t headSize, std::uint32_t ptr,
                          std::uint32_t size, std::uint32_t pc);
    std::int32_t fioLseek(std::int32_t fd, std::int32_t offset, std::uint32_t whence);
    std::int32_t fioGetstat(const std::string& name, std::vector<std::uint8_t>& stat, std::uint32_t pc);
    std::int32_t fioRemove(const std::string& name, std::uint32_t pc);
    std::int32_t fioMkdir(const std::string& name, std::uint32_t pc);
    std::int32_t fioDopen(const std::string& name, std::uint32_t pc);
    std::int32_t fioDclose(std::int32_t fd);
    // 1 = leu uma entrada (nome e io_stat_t), 0 = fim, < 0 = erro.
    std::int32_t fioDread(std::int32_t fd, std::string& name, std::vector<std::uint8_t>& stat, std::uint32_t pc);
    // Protocolos: ps2sdk (fileio-common.h) e Sony SDK 3.0 (buffers de
    // conclusão no EE, ativado pela função 255).
    std::vector<std::uint8_t> fileioSdk(std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc);
    std::vector<std::uint8_t> fileioSce(std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc);
    bool fioSce_ = false;
    std::uint32_t fioSceBuffers_[2] = {0, 0};
    unsigned fioSceNext_ = 0;
};

// Helpers de serialização little-endian usados pelos servidores HLE.
namespace iopio {
std::uint32_t rd32(const std::vector<std::uint8_t>& v, std::size_t off);
void wr32(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x);
std::string cstr(const std::vector<std::uint8_t>& in, std::size_t off, std::size_t max);
std::vector<std::uint8_t> result32(std::int32_t r);
}  // namespace iopio

}  // namespace anyps2::rt
