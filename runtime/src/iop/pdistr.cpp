// Serviço de streaming da Polyphony Digital (PDI_Streaming_service,
// pdistr.irx) em HLE. RPC "STRP" (0x53545250), pedidos de 128 bytes com três
// argumentos em [0], [4], [8] e um caminho opcional em [20]; a resposta é um
// int em [0]. Protocolo levantado do lado do EE (Gran Turismo 4):
//   fn 3 (abrir): {setor, tamanho em bytes, bloco (0x8000)} sem caminho — um
//   trecho do disco por LSN absoluto (o jogo lê assim o GT4.VOL); devolve um
//   handle, 0 = erro.
//   fn 4 (ler): {handle, destino no EE, tamanho}: o IOP escreve os dados na
//   memória do EE e avança a posição; sem resposta.
//   fn 5 (ler para o IOP): {handle, destino na RAM do IOP, tamanho} — dados
//   que ficam no IOP (som); o HLE do som é mudo, mas os dados são copiados.
//   fn 2 (fechar): {handle}.
// O resto (abrir por caminho, fn 1/6/8/9) ainda não foi visto em uso e para
// com erro claro.

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>

#include "anyps2/common/error.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/iop/cdvd.h"
#include "anyps2/runtime/iop/iop.h"
#include "anyps2/runtime/memory.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

using namespace iopio;

namespace {

constexpr std::uint32_t kServerStrp = 0x53545250u;  // "STRP"

struct Stream {
    std::uint32_t lsn = 0, size = 0, pos = 0;
};

struct PdiStrState {
    std::map<std::uint32_t, Stream> streams;
    std::uint32_t nextHandle = 1;
};

std::string requestDump(const std::vector<std::uint8_t>& in) {
    std::string s;
    for (std::size_t i = 0; i < std::min<std::size_t>(in.size(), 12); ++i) {
        char b[4];
        std::snprintf(b, sizeof(b), "%02x", in[i]);
        s += b;
        if (i % 4 == 3) s += ' ';
    }
    return s;
}

std::string pathOf(const std::vector<std::uint8_t>& in) {
    std::string p;
    for (std::size_t i = 20; i < in.size() && in[i]; ++i) p += static_cast<char>(in[i]);
    return p;
}

}  // namespace

void registerPdiStr(Iop& iop) {
    auto st = std::make_shared<PdiStrState>();
    iop.registerServer(
        kServerStrp, "pdistr",
        [&iop, st](std::uint32_t fn, const std::vector<std::uint8_t>& in,
                   std::uint32_t pc) -> std::optional<std::vector<std::uint8_t>> {
            const bool trace = iop.runtime().options().traceIop;
            std::vector<std::uint8_t> out(64, 0);
            switch (fn) {
                case 3: {
                    const std::string path = pathOf(in);
                    if (!path.empty()) {
                        throw Unimplemented("pdistr: abrir stream pelo caminho \"" + path +
                                                "\" ainda não implementado no HLE",
                                            pc);
                    }
                    const std::uint32_t h = st->nextHandle++;
                    st->streams[h] = Stream{rd32(in, 0), rd32(in, 4), 0};
                    if (trace) {
                        std::fprintf(stderr, "[iop] pdistr: stream %u = setor %u, %u bytes\n", h, rd32(in, 0),
                                     rd32(in, 4));
                    }
                    wr32(out, 0, h);
                    return out;
                }
                case 4:
                case 5: {
                    const bool toIop = fn == 5;
                    const std::uint32_t h = rd32(in, 0), dest = rd32(in, 4), size = rd32(in, 8);
                    auto it = st->streams.find(h);
                    if (it == st->streams.end()) {
                        throw GuestError("pdistr: leitura do stream " + std::to_string(h) + ", que não está aberto",
                                         pc);
                    }
                    if (!dest && !toIop) {
                        throw Unimplemented("pdistr: leitura de " + std::to_string(size) +
                                                " bytes sem destino (endereço 0) — semântica desconhecida",
                                            pc);
                    }
                    Stream& s = it->second;
                    std::vector<std::uint8_t> buf(size);
                    IsoImage& iso = iop.cdvd().image("pdistr: leitura do setor " + std::to_string(s.lsn), pc);
                    if (size && !iso.readBytes(s.lsn, s.pos, size, buf.data())) {
                        throw GuestError("pdistr: leitura de " + std::to_string(size) + " bytes do setor " +
                                             std::to_string(s.lsn) + " + " + std::to_string(s.pos) +
                                             " passa do fim da imagem de disco",
                                         pc);
                    }
                    if (trace) {
                        std::fprintf(stderr, "[iop] pdistr: stream %u lê %u bytes (posição %u) para 0x%08x (%s)\n", h,
                                     size, s.pos, dest, toIop ? "IOP" : "EE");
                    }
                    if (size && toIop) std::memcpy(iop.iopPointer(dest, size, pc), buf.data(), size);
                    else if (size) iop.runtime().memory().copyToGuest(dest, buf.data(), size, pc);
                    s.pos += size;
                    return out;
                }
                case 2:
                    st->streams.erase(rd32(in, 0));
                    return out;
                default:
                    throw Unimplemented("pdistr: função " + std::to_string(fn) +
                                            " do RPC \"STRP\" ainda não implementada no HLE (pedido: " +
                                            requestDump(in) + "...)",
                                        pc);
            }
        });
}

}  // namespace anyps2::rt
