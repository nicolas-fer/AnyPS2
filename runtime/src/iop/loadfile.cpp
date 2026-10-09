// Servidor loadfile (0x80000006). A tabela de módulos com HLE fica em
// modules.cpp.
//
// SifLoadModule("rom0:PADMAN"), SifLoadModule("host:audsrv.irx") e
// SifExecModuleBuffer(irx embutido no ELF) chegam aqui. O módulo é
// identificado pelo nome gravado no IRX (ou pelo nome do arquivo em rom0:)
// e "carregar" significa registrar os servidores RPC da implementação HLE.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/iop/audsrv.h"
#include "anyps2/runtime/iop/cdvd.h"
#include "anyps2/runtime/iop/dbcman.h"
#include "anyps2/runtime/iop/pdicdvd.h"
#include "anyps2/runtime/iop/iop.h"
#include "anyps2/runtime/iop/mcserv.h"
#include "anyps2/runtime/iop/pad.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

using namespace iopio;

namespace {

// loadfile-common.h
enum LfFunction : std::uint32_t {
    LF_F_MOD_LOAD = 0, LF_F_ELF_LOAD, LF_F_SET_ADDR, LF_F_GET_ADDR, LF_F_MG_MOD_LOAD, LF_F_MG_ELF_LOAD,
    LF_F_MOD_BUF_LOAD, LF_F_MOD_STOP, LF_F_MOD_UNLOAD, LF_F_SEARCH_MOD_BY_NAME, LF_F_SEARCH_MOD_BY_ADDRESS,
    LF_F_GET_VERSION = 0xFF,
};
constexpr std::int32_t kKeUnknownModule = -202;
constexpr std::int32_t kKeFileError = -203;
constexpr std::int32_t kKeIllegalObject = -205;

std::string versionText(std::uint16_t v) {
    return std::to_string(v >> 8) + "." + std::to_string(v & 0xFF);
}

}  // namespace

std::int32_t Iop::loadModule(const std::string& name, std::uint16_t version, const std::string& origin,
                             std::uint32_t pc) {
    const char* def = nullptr;
    if (origin.rfind("rom0:", 0) == 0 || origin.rfind("rom1:", 0) == 0) {
        def = hleModuleForRom(origin);
        if (!def) {
            throw Unimplemented("módulo do IOP " + origin + " (ROM do console) não tem implementação HLE", pc);
        }
    } else {
        def = hleModuleForIrx(name);
        if (!def) {
            // Exploração (desenvolvimento): aceita o módulo sem nenhum serviço, com
            // aviso. Quem usar o RPC dele para no erro de servidor inexistente.
            if (exploring_) {
                std::fprintf(stderr,
                             "[aviso] módulo IRX \"%s\" v%s (%s) aceito SEM implementação HLE "
                             "(ANYPS2_IOP_ACCEPT_MISSING): os serviços dele não existem\n",
                             name.c_str(), versionText(version).c_str(), origin.c_str());
                return nextModuleId_++;
            }
            throw Unimplemented("módulo IRX \"" + name + "\" v" + versionText(version) + " (" + origin +
                                    ") não tem implementação HLE — drivers próprios do jogo exigem "
                                    "executar o código do IOP, ainda não suportado (para explorar além "
                                    "deste ponto: ANYPS2_IOP_ACCEPT_MISSING=1)",
                                pc);
        }
    }
    const std::string hle = def;
    if (rt_.options().traceIop) {
        std::fprintf(stderr, "[iop] módulo %s (%s) → HLE %s\n", name.c_str(), origin.c_str(), hle.c_str());
    }
    if (!loaded_.count(hle)) {
        if (hle == "padman") pad_->load(true);
        else if (hle == "padman-rom") pad_->load(false);
        else if (hle == "mcserv") mc_->registerServer();
        else if (hle == "audsrv") audsrv_->registerServer();
        else if (hle == "dbcman") dbc_->load();
        else if (hle == "pdicdvd") pdiCdvd_->load();
        else if (hle == "lgdev") registerLgDev(*this);
        else if (hle == "pdistr") registerPdiStr(*this);
        else if (hle == "pdispu2") registerPdiSpu2(*this);
        loaded_.insert(hle);
    }
    return nextModuleId_++;
}

std::optional<std::vector<std::uint8_t>> Iop::readDeviceFile(const std::string& path, std::uint32_t pc) {
    const auto colon = path.find(':');
    const std::string device = colon == std::string::npos ? "" : path.substr(0, colon);
    if (device == "host" || device == "host0") {
        std::ifstream f(hostPath(path, pc), std::ios::binary);
        if (!f) return std::nullopt;
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    }
    if (device == "cdrom0" || device == "cdrom") return cdvd_->readFile(path.substr(colon + 1), pc);
    throw Unimplemented("leitura de \"" + path + "\" pelo IOP: dispositivo '" + device + "' não suportado no HLE",
                        pc);
}

void Iop::registerLoadfile() {
    registerServer(0x80000006u, "loadfile", [this](std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                   std::uint32_t pc) -> std::vector<std::uint8_t> {
        std::vector<std::uint8_t> out = in;
        out.resize(std::max<std::size_t>(out.size(), 8));
        auto reply = [&](std::int32_t result, std::int32_t modres) {
            wr32(out, 0, static_cast<std::uint32_t>(result));
            wr32(out, 4, static_cast<std::uint32_t>(modres));
            return out;
        };
        switch (fn) {
            case LF_F_MOD_LOAD:
            case LF_F_MG_MOD_LOAD: {  // _lf_module_load_arg: {arg_len, modres, path[252], args[252]}
                const std::string path = cstr(in, 8, 252);
                if (rt_.options().traceIop && rd32(in, 0) > 0) {
                    // Argumentos do módulo: strings separadas por '\0'.
                    std::string args;
                    const std::uint32_t len = std::min<std::uint32_t>(rd32(in, 0), 252);
                    for (std::uint32_t i = 0; i < len && 260 + i < in.size(); ++i) {
                        const char ch = static_cast<char>(in[260 + i]);
                        args += ch ? ch : ' ';
                    }
                    std::fprintf(stderr, "[iop] SifLoadModule(\"%s\") argumentos: \"%s\"\n", path.c_str(), args.c_str());
                }
                if (path.rfind("rom0:", 0) == 0 || path.rfind("rom1:", 0) == 0) {
                    return reply(loadModule(romModuleName(path), 0, path, pc), 0);
                }
                const auto data = readDeviceFile(path, pc);
                if (!data) return reply(kKeFileError, 0);
                const auto info = parseIrx(data->data(), data->size());
                if (!info) {
                    throw GuestError("SifLoadModule(\"" + path + "\"): o arquivo não é um módulo IRX", pc);
                }
                return reply(loadModule(info->name, info->version, path, pc), 0);
            }
            case LF_F_MOD_BUF_LOAD: {  // {ptr na RAM do IOP, arg_len, ...}
                const std::uint32_t ptr = rd32(in, 0);
                const std::uint32_t avail = kRamSize - (ptr & 0x1FFFFFu);
                const std::uint8_t* d = iopPointer(ptr, std::min<std::uint32_t>(avail, 52), pc);
                const auto info = parseIrx(d, avail);
                if (!info) return reply(kKeIllegalObject, 0);
                char where[64];
                std::snprintf(where, sizeof(where), "buffer na RAM do IOP em 0x%06x", ptr);
                return reply(loadModule(info->name, info->version, where, pc), 0);
            }
            case LF_F_SEARCH_MOD_BY_NAME: {  // {id, dummy, name[252], ...}
                const std::string name = cstr(in, 8, 252);
                const char* def = hleModuleForIrx(name);
                const bool found = def && loaded_.count(def);
                wr32(out, 0, static_cast<std::uint32_t>(found ? 1 : kKeUnknownModule));
                return out;
            }
            case LF_F_GET_ADDR:
            case LF_F_SET_ADDR: {  // _lf_iop_val_arg: {iop_addr, type, val}
                const std::uint32_t addr = rd32(in, 0), type = rd32(in, 4);
                const std::uint32_t bytes = type == 0 ? 1 : type == 1 ? 2 : 4;
                std::uint8_t* p = iopPointer(addr, bytes, pc);
                if (fn == LF_F_SET_ADDR) {
                    const std::uint32_t v = rd32(in, 8);
                    std::memcpy(p, &v, bytes);  // little-endian
                    wr32(out, 0, 0);
                } else {
                    std::uint32_t v = 0;
                    std::memcpy(&v, p, bytes);
                    wr32(out, 0, v);
                }
                return out;
            }
            case LF_F_GET_VERSION:
                // A libsifdev da Sony compara estes 4 bytes com a versão do SDK
                // ("3000" = SDK 3.0, o do IOPRP300) e recusa carregar módulos
                // se não baterem; o ps2sdk não usa esta função.
                out.assign({'3', '0', '0', '0'});
                return out;
            case LF_F_ELF_LOAD:
            case LF_F_MG_ELF_LOAD:
                throw Unimplemented("SifLoadElf/SifLoadElfPart(\"" + cstr(in, 8, 252) +
                                        "\"): carregar outro executável do EE em tempo de execução não é "
                                        "suportado (só o ELF recompilado existe)",
                                    pc);
            default:
                throw Unimplemented("loadfile: função " + std::to_string(fn) +
                                        (fn == LF_F_MOD_STOP || fn == LF_F_MOD_UNLOAD ? " (parar/descarregar módulo)"
                                                                                       : "") +
                                        " não implementada no HLE",
                                    pc);
        }
    });
}

}  // namespace anyps2::rt
