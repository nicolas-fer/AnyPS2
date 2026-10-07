#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace anyps2::rt {

class Runtime;

// IOP em HLE, visto pelo EE através do SIF.
//
// O EE fala com o IOP por "comandos SIF": pacotes enviados por DMA (syscall
// sceSifSetDma) para um buffer na RAM do IOP. Aqui o IOP não executa código:
// cada pacote é interpretado na hora (protocolo de ps2sdk/sifcmd e sifrpc) e
// a resposta é escrita direto no buffer de recepção do EE, seguida da
// interrupção do canal DMA SIF0 — o handler do próprio programa
// (_SifCmdIntHandler) roda e sinaliza os semáforos de quem espera.
//
// Servidores RPC implementados (Fase 2): fileio (0x80000001) com tty:
// (stdout/stderr) e host: (arquivos do host, relativos a ANYPS2_HOST_DIR).
class Iop {
public:
    static constexpr std::uint32_t kRamSize = 2u * 1024 * 1024;

    explicit Iop(Runtime& rt);
    ~Iop();

    // ---- Syscalls do EE --------------------------------------------------
    std::uint32_t sifSetDma(std::uint32_t transfers, std::uint32_t count, std::uint32_t pc);
    std::uint32_t sifSetReg(std::uint32_t reg, std::uint32_t value);
    std::uint32_t sifGetReg(std::uint32_t reg) const;

    // ---- Registradores SBUS (0x1000F200...) ------------------------------
    std::uint32_t sifReadRegister(std::uint32_t addr) const;
    void sifWriteRegister(std::uint32_t addr, std::uint32_t value);

    std::uint8_t* ram() { return ram_.data(); }

    // Entrega ao EE os pacotes pendentes (se a interrupção SIF0 puder ser
    // atendida agora). Chamado após DMAs, syscalls e EI.
    void deliverPending(std::uint32_t pc);

    // Interface de um servidor RPC em HLE: recebe o número da função e os
    // dados enviados pelo EE; devolve os dados de resposta.
    using RpcHandler = std::function<std::vector<std::uint8_t>(std::uint32_t function,
                                                               const std::vector<std::uint8_t>& data,
                                                               std::uint32_t pc)>;
    void registerServer(std::uint32_t sid, std::string name, RpcHandler handler);

private:
    struct Server {
        std::uint32_t sid;
        std::string name;
        RpcHandler handler;
        std::uint32_t buffer;  // buffer de recepção na RAM do IOP
        std::uint32_t serverData;  // "endereço" do SifRpcServerData_t no IOP
    };

    void handleCommand(const std::vector<std::uint8_t>& packet, std::uint32_t pc);
    void sendToEe(std::uint32_t cid, std::vector<std::uint8_t> packet, const std::uint8_t* extra,
                  std::uint32_t extraSize, std::uint32_t extraDest, std::uint32_t pc);
    std::uint8_t* iopPointer(std::uint32_t addr, std::uint32_t size, std::uint32_t pc);
    void registerFileio();
    void registerIopHeap();
    // Resolve "host:caminho" para um caminho do host (relativo a ANYPS2_HOST_DIR).
    std::filesystem::path hostPath(const std::string& name, std::uint32_t pc) const;

    Runtime& rt_;
    std::vector<std::uint8_t> ram_;
    std::uint32_t mscom_ = 0, smcom_ = 0, msflg_ = 0, smflg_ = 0, ctrl_ = 0, bd6_ = 0;
    std::map<std::uint32_t, std::uint32_t> sysregs_;
    std::uint32_t eeCmdBuffer_ = 0;  // onde o EE quer receber os pacotes
    std::map<std::uint32_t, Server> servers_;
    std::uint32_t nextServerBuffer_ = 0x00010000u;
    std::uint32_t nextDmaId_ = 1;
    std::uint32_t heapNext_ = 0x00100000u;  // heap do IOP no HLE (1 MB..2 MB)
    // Pacotes para o EE aguardando entrega (um por vez no buffer do EE).
    struct Pending {
        std::vector<std::uint8_t> packet;
    };
    std::vector<Pending> pending_;
    bool delivering_ = false;

    // fileio
    struct OpenFile {
        std::FILE* fp = nullptr;
        bool console = false;
        std::string path;
        bool isDir = false;
        std::vector<std::string> entries;
        std::size_t nextEntry = 0;
    };
    static OpenFile makeFile(std::FILE* fp, bool console, std::string path);
    std::map<std::int32_t, OpenFile> files_;
    std::int32_t nextFd_ = 3;
};

}  // namespace anyps2::rt
