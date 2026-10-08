// pdicdvd (driver de disco da Polyphony Digital) em HLE. Ver pdicdvd.h.

#include "anyps2/runtime/iop/pdicdvd.h"

#include <cstdio>
#include <string>

#include "anyps2/common/error.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/iop/cdvd.h"
#include "anyps2/runtime/iop/iop.h"
#include "anyps2/runtime/memory.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/timing.h"

namespace anyps2::rt {

using namespace iopio;

namespace {

constexpr std::uint32_t kServerPcdv = 0x50434456u;    // "PCDV"
constexpr std::uint32_t kServerStatus = 0x50636476u;  // "Pcdv"

// Relógio do buffer de status: segundos a partir de 1999-12-30 00:00 (o jogo
// soma o dia juliano 2451543,5) no fuso do Japão (desconta 32400 s). Começa
// em 2000-01-01 00:00 JST mais o tempo emulado, como o RTC do cdvdman no
// relógio virtual.
constexpr std::uint32_t kClockStart = 2u * 86400u + 32400u;

std::string dump(const std::vector<std::uint8_t>& in) {
    std::string s;
    for (std::size_t i = 0; i < std::min<std::size_t>(in.size(), 16); ++i) {
        char b[4];
        std::snprintf(b, sizeof(b), "%02x", in[i]);
        s += b;
        if (i % 4 == 3) s += ' ';
    }
    return s;
}

}  // namespace

PdiCdvd::PdiCdvd(Iop& iop) : iop_(iop) {}

void PdiCdvd::reset() {
    statusAddr_ = 0;
}

void PdiCdvd::load() {
    iop_.registerServer(kServerPcdv, "pdicdvd",
                        [this](std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc)
                            -> std::optional<std::vector<std::uint8_t>> { return pcdv(fn, in, pc); });
    iop_.registerServer(kServerStatus, "pdicdvd-status",
                        [this](std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc)
                            -> std::optional<std::vector<std::uint8_t>> { return status(fn, in, pc); });
}

void PdiCdvd::writeStatus(std::uint32_t pc) {
    if (!statusAddr_) return;
    std::vector<std::uint8_t> st(64, 0);
    const std::uint64_t seconds = iop_.runtime().timing().now() / Timing::kEeHz;
    wr32(st, 0, kClockStart + static_cast<std::uint32_t>(seconds));
    iop_.runtime().memory().copyToGuest(statusAddr_, st.data(), static_cast<std::uint32_t>(st.size()), pc);
}

void PdiCdvd::vblank(std::uint32_t pc) {
    ++vblanks_;
    if (statusAddr_ && iop_.moduleLoaded("pdicdvd")) writeStatus(pc);
}

std::vector<std::uint8_t> PdiCdvd::pcdv(std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc) {
    const bool trace = iop_.runtime().options().traceIop;
    std::vector<std::uint8_t> out(64, 0);
    switch (fn) {
        case 1:  // pronto
            return out;
        case 2:  // {setor do PVD, hash}: o driver real confere o disco; aqui só registra
            if (trace) std::fprintf(stderr, "[iop] pdicdvd: PVD no setor %u\n", rd32(in, 0));
            return out;
        case 3: {  // leitura {setor, tamanho, destino, modo}
            const std::uint32_t lsn = rd32(in, 0), size = rd32(in, 4), dest = rd32(in, 8);
            if (trace) {
                std::fprintf(stderr, "[iop] pdicdvd: lê %u bytes do setor %u para 0x%08x (modo %u)\n", size, lsn,
                             dest, rd32(in, 12));
            }
            std::vector<std::uint8_t> buf(size);
            IsoImage& iso = iop_.cdvd().image("pdicdvd: leitura do setor " + std::to_string(lsn), pc);
            if (size && !iso.readBytes(lsn, 0, size, buf.data())) {
                throw GuestError("pdicdvd: leitura de " + std::to_string(size) + " bytes do setor " +
                                     std::to_string(lsn) + " passa do fim da imagem de disco",
                                 pc);
            }
            if (size) iop_.runtime().memory().copyToGuest(dest, buf.data(), size, pc);
            wr32(out, 0, 0);  // sucesso
            return out;
        }
        case 4: {  // início da camada 1 do DVD
            IsoImage& iso = iop_.cdvd().image("pdicdvd: informação de camadas", pc);
            const auto base = iso.layer1Start();
            if (!base) {
                throw Unimplemented("pdicdvd: o jogo pede o início da camada 1, mas a imagem não é um DVD de "
                                    "camada dupla",
                                    pc);
            }
            wr32(out, 0, 1);
            wr32(out, 4, *base);
            return out;
        }
        default:
            throw Unimplemented("pdicdvd: função " + std::to_string(fn) + " do RPC \"PCDV\" ainda não implementada "
                                    "no HLE (pedido: " + dump(in) + "...)",
                                pc);
    }
}

std::vector<std::uint8_t> PdiCdvd::status(std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc) {
    std::vector<std::uint8_t> out(64, 0);
    switch (fn) {
        case 0:  // [0] = buffer de status no EE
            statusAddr_ = rd32(in, 0);
            writeStatus(pc);
            return out;
        case 2:  // pedido sem dados (assíncrono): aceito
            return out;
        default:
            throw Unimplemented("pdicdvd: função " + std::to_string(fn) + " do RPC \"Pcdv\" ainda não implementada "
                                    "no HLE (pedido: " + dump(in) + "...)",
                                pc);
    }
}

}  // namespace anyps2::rt
