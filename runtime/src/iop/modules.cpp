// Tabela de módulos do IOP com implementação HLE. Usada pelo loadfile
// (decide se um SifLoadModule/SifExecModuleBuffer pode "carregar") e pela
// triagem de discos da CLI ("anyps2 disc").

#include <cctype>
#include <vector>

#include "anyps2/runtime/iop/irx.h"

namespace anyps2::rt {

namespace {

// `irx` são os nomes que aparecem no cabeçalho dos IRX (ps2sdk e SCE); `rom`
// são os arquivos de rom0: do console.
struct ModuleDef {
    const char* hle;
    std::vector<std::string> irx;
    std::vector<std::string> rom;
};
const std::vector<ModuleDef>& moduleTable() {
    static const std::vector<ModuleDef> kTable = {
        {"sio2man", {"sio2man"}, {"SIO2MAN", "XSIO2MAN"}},
        {"padman", {"padman"}, {"XPADMAN"}},
        {"padman-rom", {}, {"PADMAN"}},
        {"mcman", {"mcman_cex", "mcman"}, {"MCMAN", "XMCMAN"}},
        {"mcserv", {"mcserv"}, {"MCSERV", "XMCSERV"}},
        {"cdvdman", {"cdvd_driver"}, {"CDVDMAN"}},
        {"cdvdfsv", {"cdvd_ee_driver"}, {"CDVDFSV"}},
        {"libsd", {"freesd", "libsd"}, {"LIBSD"}},
        {"clearspu", {"clearspu"}, {"CLEARSPU"}},
        {"audsrv", {"audsrv"}, {}},
        {"ioman", {"IO/File_Manager", "FILEIO_service"}, {"IOMAN", "FILEIO"}},
        {"eesync", {"SyncEE"}, {"EESYNC"}},
        // Multitap: sem multitap conectado. Carregar é aceito (jogos carregam o
        // módulo mesmo sem usá-lo); a RPC dele (libmtap, 0x800009xx) não existe,
        // então quem tentar usá-la para com o erro de servidor inexistente.
        {"mtapman", {"multitap_manager"}, {"MTAPMAN", "XMTAPMAN"}},
        // Pilha de controles do SDK 3.0 (libdbc/libpad2): o HLE do dbcman
        // cobre os três; sio2d e ds2u_d não têm RPC próprio.
        {"dbcman", {"Dbc_Manager"}, {}},
        {"sio2d", {"sio2d"}, {}},
        {"ds2u_d", {"ds2u_d"}, {}},
    };
    return kTable;
}

}  // namespace

const char* hleModuleForIrx(const std::string& irxName) {
    for (const auto& m : moduleTable()) {
        for (const auto& n : m.irx) {
            if (n == irxName) return m.hle;
        }
    }
    return nullptr;
}

std::string romModuleName(const std::string& romPath) {
    std::string n = romPath.substr(romPath.find(':') + 1);
    while (!n.empty() && (n[0] == '/' || n[0] == '\\')) n.erase(0, 1);
    const auto dot = n.find('.');
    if (dot != std::string::npos) n.resize(dot);
    for (char& c : n) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return n;
}

const char* hleModuleForRom(const std::string& romPath) {
    const std::string rom = romModuleName(romPath);
    for (const auto& m : moduleTable()) {
        for (const auto& r : m.rom) {
            if (r == rom) return m.hle;
        }
    }
    return nullptr;
}

}  // namespace anyps2::rt
