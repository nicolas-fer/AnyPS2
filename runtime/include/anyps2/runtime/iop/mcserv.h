#pragma once

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace anyps2::rt {

class Iop;

// mcman/mcserv em HLE (libmc: RPC 0x80000400, numeração XMCSERV e a do
// MCSERV antigo). Os memory cards são diretórios do host:
//   $ANYPS2_MC_DIR/mc0 e mc1 (padrão: $ANYPS2_HOST_DIR/memcard)
// O sistema de arquivos do cartão (FAT próprio da Sony, clusters de 1 KB,
// 8 MB) não é emulado em nível de bloco: arquivos e pastas do cartão são
// arquivos e pastas do host. O espaço livre é calculado como no cartão de
// 8 MB (clusters de 1 KB). Um cartão "sem formatação" é um diretório
// ausente; mcFormat o cria.
class McServ {
public:
    explicit McServ(Iop& iop);
    ~McServ();
    void registerServer();
    void reset();

private:
    struct File {
        std::FILE* fp = nullptr;
        unsigned port = 0;
        std::string path;  // caminho no cartão ("/SAVE/DATA.BIN")
    };
    struct DirListing {
        std::vector<std::vector<std::uint8_t>> entries;  // sceMcTblGetDir (64 bytes)
        std::size_t next = 0;
    };
    std::optional<std::vector<std::uint8_t>> rpc(std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                 std::uint32_t pc);
    std::filesystem::path root(unsigned port) const;
    // Caminho do cartão absoluto a partir do diretório atual da porta.
    std::string resolve(unsigned port, const std::string& name) const;
    std::filesystem::path hostPath(unsigned port, const std::string& cardPath) const;
    std::int32_t freeClusters(unsigned port) const;
    std::vector<std::uint8_t> tableEntry(const std::filesystem::path& p, const std::string& name) const;

    Iop& iop_;
    std::map<std::int32_t, File> files_;
    std::int32_t nextFd_ = 1;
    std::string curDir_[2] = {"/", "/"};
    bool infoSeen_[2] = {false, false};
    DirListing listing_[2];
};

}  // namespace anyps2::rt
