// Serviço de streaming da Polyphony Digital (PDI_Streaming_service,
// pdistr.irx) em HLE. RPC "STRP" (0x53545250), pedidos de 128 bytes com três
// argumentos em [0], [4], [8] e um caminho opcional em [20]; a resposta é um
// int em [0]. Protocolo levantado do lado do EE (Gran Turismo 4):
//   fn 3 (abrir): {setor, tamanho em bytes, bloco (0x8000)} sem caminho — um
//   trecho do disco por LSN absoluto (o jogo lê assim o GT4.VOL); devolve um
//   handle, 0 = erro.
//   fn 4 (ler): {handle, destino no EE, tamanho}: o IOP escreve os dados na
//   memória do EE e avança a posição; sem resposta.
//   fn 5 (ler para o SPU2): {handle, destino na RAM do SPU2, tamanho} — os
//   bancos de som (amostras ADPCM). O IRX chama pdispu2_35 (PDISPU2.IRX), que
//   passa os dados por um FIFO e os envia ao SPU2 por sceSdVoiceTrans; os
//   destinos vistos (0x5040, 0x71ef0, 0x8b060, 0xa7040, 0x1e0000) são
//   endereços de som, a partir de 0x5000, e não da RAM do IOP.
//   fn 2 (fechar): {handle}.
// O resto (abrir por caminho, fn 1/6/8/9) ainda não foi visto em uso e para
// com erro claro.
//
// O mesmo módulo registra "MPG1"/"MPG2" (0x4D504731/2), o leitor de vídeos:
// um MPEG-2 Program Stream lido do disco, com os pacotes de vídeo (0xE0) indo
// ao EE e o áudio (stream privado 0xBD) ao SPU2 — mudo aqui, só pulado.
// Levantado do código do IRX (PDISTR.IRX) e do cliente no EE (GT4):
//   fn 1 (abrir): {flags, LSN, setores, nome em [12]}; sem nome, um trecho do
//   disco. flags: 0x10 repete o vídeo (no fim, o leitor volta ao começo),
//   0x20/0x40 ligam os dois canais de áudio.
//   fn 2 (ler, síncrono): {destino no EE, tamanho}: cada fatia de 5120 bytes
//   recebe um pacote de vídeo em registros de 16 bytes {bytes, desalinhamento
//   (0–3), bytes que faltam do pacote, 0} seguidos dos dados (múltiplo de 16),
//   e um registro zerado no fim. O EE remonta "00 00 01 E0 tamanho" + dados.
//   No fim do stream, a fatia corrente recebe só o registro zerado.
//   fn 3 (volume do áudio) e fn 4 (parar).

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>

#include "anyps2/common/error.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/iop/cdvd.h"
#include "anyps2/runtime/iop/iop.h"
#include "anyps2/runtime/iop/movie.h"
#include "anyps2/runtime/iop/spu2.h"
#include "anyps2/runtime/memory.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

namespace movie {

bool ProgramStream::read(std::uint32_t n, std::uint8_t* dst) {
    if (pos_ + n > size_ || !read_(pos_, n, dst)) return false;
    pos_ += n;
    return true;
}

bool ProgramStream::nextVideo(std::vector<std::uint8_t>& payload, std::uint32_t pc) {
    while (!ended_) {
        std::uint8_t b[10];
        if (!read(4, b)) break;
        const std::uint32_t code = (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) |
                                   (std::uint32_t{b[2]} << 8) | b[3];
        if ((code >> 8) != 1 || code <= 0x1B9) break;  // sem start code, ou fim do programa
        if (code == 0x1BA) {  // pack header do MPEG-2: 10 bytes + enchimento
            if (!read(10, b)) break;
            if ((b[0] & 0xC0) != 0x40) {
                throw Unimplemented("MPG: pack header de MPEG-1 no vídeo (só MPEG-2 é suportado)", pc);
            }
            pos_ += b[9] & 7;
            continue;
        }
        if (!read(2, b)) break;
        const std::uint32_t len = (std::uint32_t{b[0]} << 8) | b[1];
        if (code != 0x1E0) {  // system header, áudio, padding...
            pos_ += len;
            continue;
        }
        payload.resize(len);
        if (!read(len, payload.data())) break;
        videoThisPass_ = true;
        return true;
    }
    if (loop_ && videoThisPass_ && !ended_) {
        // Fim de uma passada de um vídeo em repetição: de volta ao começo.
        pos_ = 0;
        videoThisPass_ = false;
        return nextVideo(payload, pc);
    }
    ended_ = true;
    return false;
}

std::uint32_t buildSlot(const std::vector<std::uint8_t>* payload, std::uint8_t (&slot)[kSlotSize], std::uint32_t pc) {
    std::memset(slot, 0, 16);
    if (!payload) return 16;
    const auto len = static_cast<std::uint32_t>(payload->size());
    const std::uint32_t padded = (len + 15) & ~15u;
    if (32 + padded > kSlotSize) {
        throw GuestError("MPG: pacote de vídeo de " + std::to_string(len) + " bytes não cabe na fatia de 5120",
                         pc);
    }
    for (unsigned i = 0; i < 4; ++i) {
        slot[i] = static_cast<std::uint8_t>(len >> (8 * i));      // bytes deste registro
        slot[8 + i] = static_cast<std::uint8_t>(len >> (8 * i));  // que faltam do pacote
    }
    std::memset(slot + 16, 0, padded + 16);
    std::memcpy(slot + 16, payload->data(), len);
    return 32 + padded;
}

}  // namespace movie

using namespace iopio;

namespace {

constexpr std::uint32_t kServerStrp = 0x53545250u;  // "STRP"

struct Stream {
    std::uint32_t lsn = 0, size = 0, pos = 0;
};

constexpr std::uint32_t kServerMpg1 = 0x4D504731u, kServerMpg2 = 0x4D504732u;  // "MPG1", "MPG2"

// Vídeo aberto pelo fn 1 do MPG1/MPG2.
struct Movie {
    std::uint32_t lsn = 0;
    std::unique_ptr<movie::ProgramStream> ps;
};

void registerMovie(Iop& iop, std::uint32_t sid, const char* name) {
    auto mv = std::make_shared<Movie>();
    iop.registerServer(
        sid, name,
        [&iop, mv, name](std::uint32_t fn, const std::vector<std::uint8_t>& in,
                         std::uint32_t pc) -> std::optional<std::vector<std::uint8_t>> {
            const bool trace = iop.runtime().options().traceIop;
            std::vector<std::uint8_t> out(64, 0);
            switch (fn) {
                case 1: {
                    const std::uint32_t flags = rd32(in, 0);
                    if (in.size() > 12 && in[12]) {
                        throw Unimplemented(std::string(name) + ": abrir vídeo pelo caminho ainda não implementado",
                                            pc);
                    }
                    const std::uint32_t lsn = rd32(in, 4);
                    const std::uint64_t size = std::uint64_t{rd32(in, 8)} * IsoImage::kSectorSize;
                    IsoImage& iso = iop.cdvd().image(std::string(name) + ": abrir vídeo", pc);
                    mv->lsn = lsn;
                    mv->ps = std::make_unique<movie::ProgramStream>(
                        [&iso, lsn](std::uint64_t pos, std::uint32_t n, std::uint8_t* dst) {
                            return iso.readBytes(lsn, pos, n, dst);
                        },
                        size, (flags & 0x10) != 0);
                    if (trace) {
                        std::fprintf(stderr, "[iop] %s: vídeo no setor %u, %llu bytes (flags 0x%x)\n", name, lsn,
                                     static_cast<unsigned long long>(size), flags);
                    }
                    return out;
                }
                case 2: {
                    const std::uint32_t dest = rd32(in, 0), size = rd32(in, 4);
                    if (!mv->ps) throw GuestError(std::string(name) + ": leitura sem vídeo aberto", pc);
                    Memory& mem = iop.runtime().memory();
                    std::vector<std::uint8_t> payload;
                    unsigned packets = 0;
                    for (std::uint32_t at = 0; at + movie::kSlotSize <= size; at += movie::kSlotSize) {
                        std::uint8_t slot[movie::kSlotSize];
                        const bool got = mv->ps->nextVideo(payload, pc);
                        mem.copyToGuest(dest + at, slot, movie::buildSlot(got ? &payload : nullptr, slot, pc), pc);
                        if (!got) break;
                        ++packets;
                    }
                    if (trace) {
                        std::fprintf(stderr, "[iop] %s: %u pacotes de vídeo para 0x%08x (posição %llu%s)\n", name,
                                     packets, dest, static_cast<unsigned long long>(mv->ps->position()),
                                     mv->ps->ended() ? ", fim" : "");
                    }
                    return out;
                }
                case 3:  // volume dos canais de áudio (som mudo no HLE)
                    return out;
                case 4:
                    mv->ps.reset();
                    return out;
                default:
                    throw Unimplemented(std::string(name) + ": função " + std::to_string(fn) +
                                            " ainda não implementada no HLE",
                                        pc);
            }
        });
}

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
                    const bool toSpu = fn == 5;
                    const std::uint32_t h = rd32(in, 0), dest = rd32(in, 4), size = rd32(in, 8);
                    auto it = st->streams.find(h);
                    if (it == st->streams.end()) {
                        throw GuestError("pdistr: leitura do stream " + std::to_string(h) + ", que não está aberto",
                                         pc);
                    }
                    if (!dest && !toSpu) {
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
                                     size, s.pos, dest, toSpu ? "SPU2" : "EE");
                    }
                    if (size && toSpu) {
                        if (!iop.spu2().writeRam(dest, buf.data(), size)) {
                            throw GuestError("pdistr: " + std::to_string(size) + " bytes para o endereço 0x" +
                                                 anyps2::hex(dest) + " passam dos 2 MB da RAM do SPU2",
                                             pc);
                        }
                    } else if (size) {
                        iop.runtime().memory().copyToGuest(dest, buf.data(), size, pc);
                    }
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
    registerMovie(iop, kServerMpg1, "MPG1");
    registerMovie(iop, kServerMpg2, "MPG2");
}

}  // namespace anyps2::rt
