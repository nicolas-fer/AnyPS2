// dbcman + sio2d + ds2u_d em HLE (libdbc/libpad2 do SDK 3.0). Ver dbcman.h.

#include "anyps2/runtime/iop/dbcman.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "anyps2/common/error.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/input.h"
#include "anyps2/runtime/iop/iop.h"
#include "anyps2/runtime/memory.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

using namespace iopio;

namespace {

constexpr std::uint32_t kServerMain = 0x80001300u;
constexpr std::uint32_t kServerBulk = 0x8000131Cu;
constexpr std::uint32_t kServerCmd0 = 0x8000131Eu, kServerCmd1 = 0x8000131Fu;

enum : std::uint32_t {
    kFnCreateSocket = 0x80001301u,
    kFnDeleteSocket = 0x80001302u,
    kFnStartSocket = 0x80001303u,
    kFnSetWorkAddr = 0x80001304u,
    kFnEnd = 0x80001305u,
    kFnQuery = 0x8000131Au,
    kFnVersion = 0x80001363u,
};

// Versão declarada: a libdbc confere (versão >> 4) == 0x31 (dbcman 3.1x).
constexpr std::uint32_t kVersion = 0x0316;

// Quadro de dados de um socket (128 bytes no EE).
constexpr std::uint32_t kFrameSize = 128;
constexpr std::uint32_t kFrameData = 28, kFrameCounter = 124;
constexpr std::uint8_t kDataSize = 18;     // relatório do DS2
// Perfil: entradas 0–15 digitais, 16–31 analógicas (eixos + pressões).
constexpr std::uint8_t kProfile[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0x00};

// Comandos da consulta 0x8000131A (byte baixo da palavra de comando).
constexpr std::uint32_t kQueryState = 12, kQueryInfo = 2;
// Comando assíncrono de atuadores (vibração).
constexpr std::uint32_t kCmdActuator = 11;

std::string hexFn(std::uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08X", v);
    return b;
}

}  // namespace

DbcMan::DbcMan(Iop& iop) : iop_(iop) {}

void DbcMan::reset() {
    sockets_ = {};
    workArea_ = 0;
}

void DbcMan::load() {
    iop_.registerServer(kServerMain, "dbcman",
                        [this](std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc)
                            -> std::optional<std::vector<std::uint8_t>> { return main(fn, in, pc); });
    for (const std::uint32_t sid : {kServerCmd0, kServerCmd1}) {
        iop_.registerServer(sid, "dbcman-cmd",
                            [this](std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc)
                                -> std::optional<std::vector<std::uint8_t>> { return command(fn, in, pc); });
    }
    iop_.registerServer(kServerBulk, "dbcman-bulk",
                        [](std::uint32_t fn, const std::vector<std::uint8_t>&, std::uint32_t pc)
                            -> std::optional<std::vector<std::uint8_t>> {
                            throw Unimplemented("dbcman: transferência grande (RPC 0x8000131C, função " + hexFn(fn) +
                                                    ") ainda não implementada no HLE",
                                                pc);
                        });
}

bool DbcMan::connected(unsigned port) {
    return port < 2 && iop_.runtime().input().sample(port, vblanks_).connected;
}

// Palavra de estado por socket na área de trabalho do EE.
void DbcMan::writeStates(std::uint32_t pc) {
    if (!workArea_) return;
    std::uint8_t st[64] = {};
    for (unsigned i = 0; i < sockets_.size(); ++i) {
        const std::uint32_t v = sockets_[i].used && connected(sockets_[i].port) ? 1u : 0u;
        std::memcpy(st + 4 * i, &v, 4);
    }
    iop_.runtime().memory().copyToGuest(workArea_, st, sizeof(st), pc);
}

void DbcMan::writeFrame(Socket& s, std::uint32_t pc) {
    std::uint8_t f[kFrameSize] = {};
    const PadInput in = iop_.runtime().input().sample(s.port, vblanks_);
    if (in.connected) {
        f[0] = 1;  // pronto
        f[2] = kDataSize;
        f[3] = sizeof(kProfile);
        f[4] = 1;  // status: dados válidos
        ds2Report(in, f + kFrameData);
        std::memcpy(f + kFrameData + kDataSize, kProfile, sizeof(kProfile));
    }
    const std::uint32_t counter = ++s.counter;
    std::memcpy(f + kFrameCounter, &counter, 4);
    iop_.runtime().memory().copyToGuest(s.frames[s.next], f, kFrameSize, pc);
    s.next ^= 1;
}

void DbcMan::vblank(std::uint32_t pc) {
    ++vblanks_;
    if (!iop_.moduleLoaded("dbcman")) return;
    writeStates(pc);
    for (auto& s : sockets_) {
        if (s.used && s.started) writeFrame(s, pc);
    }
}

std::vector<std::uint8_t> DbcMan::main(std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc) {
    std::vector<std::uint8_t> out = in;  // a libdbc usa o mesmo buffer de 0x90 bytes na ida e na volta
    out.resize(std::max<std::size_t>(out.size(), 0x90), 0);
    if (iop_.runtime().options().traceIop) std::fprintf(stderr, "[iop] dbcman %s\n", hexFn(fn).c_str());
    auto socketAt = [&](std::size_t off) -> Socket* {
        const std::uint32_t id = rd32(in, off);
        return id < sockets_.size() && sockets_[id].used ? &sockets_[id] : nullptr;
    };
    switch (fn) {
        case kFnVersion:
            wr32(out, 0, kVersion);
            return out;
        case kFnSetWorkAddr:  // [4] = área de trabalho no EE
            workArea_ = rd32(in, 4);
            writeStates(pc);
            wr32(out, 0, 0);
            return out;
        case kFnEnd:
            wr32(out, 4, 0);
            return out;
        case kFnCreateSocket: {  // [0] tipo|1, [8] porta, ..., [40]/[44] quadros no EE -> [36] socket
            std::int32_t id = -1;
            for (unsigned i = 0; i < sockets_.size(); ++i) {
                if (!sockets_[i].used) {
                    id = static_cast<std::int32_t>(i);
                    break;
                }
            }
            if (id >= 0) {
                Socket& s = sockets_[static_cast<std::size_t>(id)];
                s = Socket{};
                s.used = true;
                s.port = rd32(in, 8);
                s.frames = {rd32(in, 40), rd32(in, 44)};
                if (s.port > 1) {
                    throw Unimplemented("dbcman: socket na porta " + std::to_string(s.port) +
                                            " (só as portas 0 e 1 existem; sem multitap)",
                                        pc);
                }
                writeStates(pc);
            }
            wr32(out, 36, static_cast<std::uint32_t>(id));
            return out;
        }
        case kFnDeleteSocket: {
            Socket* s = socketAt(0);
            if (s) *s = Socket{};
            writeStates(pc);
            wr32(out, 4, s ? 0u : static_cast<std::uint32_t>(-1));
            return out;
        }
        case kFnStartSocket: {
            Socket* s = socketAt(0);
            if (s) {
                s->started = true;
                writeFrame(*s, pc);
            }
            wr32(out, 4, s ? 0u : static_cast<std::uint32_t>(-1));
            return out;
        }
        case kFnQuery: {  // [0] socket, [4] comando, [8] tamanho -> [8] tamanho, [12..] dados, [140] resultado
            Socket* s = socketAt(0);
            const std::uint32_t cmd = rd32(in, 4) & 0xFF;
            if (!s) {
                wr32(out, 140, static_cast<std::uint32_t>(-1));
                return out;
            }
            if (cmd == kQueryState) {
                wr32(out, 8, 1);
                out[12] = connected(s->port) ? 1 : 0;
            } else if (cmd == kQueryInfo) {
                wr32(out, 8, 4);
                wr32(out, 12, 0);
            } else {
                throw Unimplemented("dbcman: consulta ao dispositivo com comando " + std::to_string(cmd) +
                                        " (palavra " + hexFn(rd32(in, 4)) + ") ainda não implementada no HLE",
                                    pc);
            }
            wr32(out, 140, 0);
            return out;
        }
        default:
            throw Unimplemented("dbcman: função " + hexFn(fn) + " da libdbc ainda não implementada no HLE", pc);
    }
}

// {socket, palavra de comando, tamanho, dados...}: comandos ao dispositivo.
std::vector<std::uint8_t> DbcMan::command(std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc) {
    const std::uint32_t cmd = rd32(in, 4) & 0xFF;
    if (cmd != kCmdActuator) {
        throw Unimplemented("dbcman: comando assíncrono " + std::to_string(cmd) + " (função " + hexFn(fn) +
                                ", palavra " + hexFn(rd32(in, 4)) + ") ainda não implementado no HLE",
                            pc);
    }
    // Vibração: aceita (sem atuador no host).
    std::vector<std::uint8_t> out = in;
    out.resize(std::max<std::size_t>(out.size(), 0x90), 0);
    return out;
}

}  // namespace anyps2::rt
