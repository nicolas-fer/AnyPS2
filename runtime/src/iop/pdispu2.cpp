// Driver de som da Polyphony Digital (PDISPU2.IRX, "PDI_SPU2_Manager" v1.18)
// em HLE. Dois servidores RPC com o mesmo despacho: "SPUP" (0x53505550, os
// registradores, chamado todo quadro) e "SPUT" (0x53505554, transferências de
// dados de som; fica numa thread à parte para não travar os registradores).
// Levantado do código do IRX (a tabela de funções em 0x302c, as rotinas em
// 0x10d0, 0x1188, 0x2f30...) e do cliente no EE (GT4, 0x55e3xx–0x55f5xx).
//
// O módulo é um espelho dos registradores do SPU2 (libsd): o EE mantém uma
// cópia de 960 bytes, marca o que mudou e manda tudo de uma vez; o IOP só
// traduz as marcas em sceSdSetParam/SetSwitch/SetAddr. Funções (as mesmas nos
// dois servidores; o jogo usa 3–6 em SPUP e 1–2 em SPUT):
//   fn 0  informações do módulo: {versão 0x112, "SPUP", "SPUT"}.
//   fn 1  ler a RAM do SPU2 para o EE: {endereço no SPU2, endereço no EE,
//         bytes}. (O IRX usa sceSdVoiceTrans em modo leitura e um FIFO de 128 KB;
//         o HLE copia direto.) Resposta vazia.
//   fn 2  escrever na RAM do SPU2 a partir do EE: mesmos três campos. É por
//         aqui que sobem as amostras ADPCM. Sem resposta útil.
//   fn 3  estado das vozes: 104 bytes (ver abaixo), em um buffer de 128.
//   fn 4  aplicar o bloco de registradores de 960 bytes (ver abaixo); a
//         resposta é o mesmo estado da fn 3.
//   fn 5  {a, b, c}: modo da saída digital (SPDIF) do núcleo 0 — o IRX só
//         guarda e reescreve o atributo do núcleo; sem efeito no PC.
//   fn 6  {d}: idem (quarto valor do mesmo atributo).
//
// Estado (fn 3 e resposta da fn 4), por núcleo (0 e 1), 52 bytes: u32 ENDX (um
// bit por voz que chegou ao fim do sample) e 24 × u16 ENVX (nível do envelope
// de cada voz). O jogo olha isso para saber quando uma voz acabou.
//
// Bloco de 960 bytes (fn 4): dois núcleos de 464 bytes (+ 32 bytes que o IRX
// não lê). Em cada núcleo, deslocamentos:
//   +0    u32  marcas do núcleo: 0x2 EVOL, 0x4 AVOL, 0x8 BVOL, 0x10 PMON,
//              0x20 NON, 0x40 VMIXL, 0x80 VMIXEL, 0x100 VMIXR, 0x200 VMIXER,
//              0x400 MMIX, 0x800 há key-on (+436), 0x1000 há key-off (+440),
//              0x2000 atributo do núcleo (+444)
//   +4    u32  vozes com mudança (bit i = voz i)
//   +8+16*i    24 vozes de 16 bytes: u16 marcas (0x1 pitch, 0x2 VOLL, 0x4
//              VOLR, 0x8 endereço inicial, 0x10 ADSR1, 0x20 ADSR2, 0x40
//              "zerar o volume antes": volume 0 durante a atualização e,
//              no fim, o VOLL/VOLR do bloco volta), u16 pitch (0x1000 = 48
//              kHz), u16 VOLL, u16 VOLR, u32 endereço inicial (bytes na RAM do
//              SPU2; o padrão do jogo é 0x5000), u16 ADSR1, u16 ADSR2
//   +396/+398 EVOL L/R, +400/+402 AVOL L/R, +404/+406 BVOL L/R (u16)
//   +408 PMON, +412 NON, +416 VMIXL, +420 VMIXEL, +424 VMIXR, +428 VMIXER (u32,
//        máscaras de vozes), +432 MMIX (u16)
//   +436 u32 vozes para key-on, +440 u32 vozes para key-off
//   +444 u16 atributo do núcleo, +448 u8 efeito alterado, +449 u8 efeito ligado,
//   +450 u8 limpar a área do efeito, +460 u32 fim da área do efeito (reverb)
// O key-off sai antes do key-on e ignora vozes que também levam key-on no mesmo
// bloco (o IRX aplica os dois numa thread própria, ~100 µs depois). Os valores
// dos registradores de volume/ADSR/pitch são os do hardware (libsd) e o Spu2 do
// runtime os entende como tal; voz n do núcleo c = voz 24·c + n.
//
// O que o Spu2 do runtime não tem fica explícito: reverb (efeito ligado),
// modulação de pitch (PMON), ruído (NON) e volumes em "sweep" (bit 15 do
// VOLL/VOLR) geram um aviso na primeira vez e são ignorados; mixer por voz
// (VMIX*), volumes de efeito/entrada e atributo do núcleo são guardados.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#include "anyps2/common/error.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/iop/iop.h"
#include "anyps2/runtime/iop/pdispu2.h"
#include "anyps2/runtime/iop/spu2.h"
#include "anyps2/runtime/memory.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

using namespace iopio;

namespace {

constexpr std::uint32_t kServerSpup = 0x53505550u;  // "SPUP"
constexpr std::uint32_t kServerSput = 0x53505554u;  // "SPUT"
constexpr std::uint32_t kModuleVersion = 0x112;

// Marcas do núcleo (+0) e da voz.
constexpr std::uint32_t kCoreKeyOn = 0x800, kCoreKeyOff = 0x1000;
constexpr std::uint32_t kCoreNoise = 0x20, kCorePitchMod = 0x10;
constexpr std::uint32_t kVoicePitch = 0x1, kVoiceVolL = 0x2, kVoiceVolR = 0x4, kVoiceAddr = 0x8, kVoiceAdsr1 = 0x10,
                        kVoiceAdsr2 = 0x20, kVoiceMute = 0x40;

// Avisos (um bit por tipo, para dizer cada coisa uma vez só).
constexpr unsigned kWarnReverb = 1u << 0, kWarnPitchMod = 1u << 1, kWarnNoise = 1u << 2, kWarnSweep = 1u << 3;

std::uint16_t r16(const std::uint8_t* p, std::size_t off) {
    return static_cast<std::uint16_t>(p[off] | (p[off + 1] << 8));
}
std::uint32_t r32(const std::uint8_t* p, std::size_t off) {
    return std::uint32_t{p[off]} | (std::uint32_t{p[off + 1]} << 8) | (std::uint32_t{p[off + 2]} << 16) |
           (std::uint32_t{p[off + 3]} << 24);
}
void w16(std::vector<std::uint8_t>& v, std::size_t off, std::uint16_t x) {
    v[off] = static_cast<std::uint8_t>(x);
    v[off + 1] = static_cast<std::uint8_t>(x >> 8);
}

}  // namespace

void PdiSpu2::warnOnce(unsigned bit, const char* text) {
    if (warned_ & bit) return;
    warned_ |= bit;
    std::fprintf(stderr, "[aviso] pdispu2: %s (ignorado; o Spu2 do runtime não tem isso)\n", text);
}

// Repassa à voz que já toca o que pode mudar com ela tocando.
void PdiSpu2::pushLive(unsigned voice) {
    if (!spu_.active(voice)) return;
    const VoiceRegs& r = regs_[voice];
    spu_.setVolume(voice, r.volL, r.volR);
    spu_.setPitch(voice, r.pitch);
    spu_.setEnvelope(voice, r.adsr1, r.adsr2);
}

std::vector<std::uint8_t> PdiSpu2::apply(const std::uint8_t* block, std::size_t size) {
    if (size < kCores * kCoreSize) {
        throw GuestError("pdispu2: bloco de registradores com " + std::to_string(size) + " bytes (esperado " +
                             std::to_string(kBlockSize) + ")",
                         0);
    }
    std::uint32_t keyOn[kCores] = {}, keyOff[kCores] = {};
    for (unsigned c = 0; c < kCores; ++c) {
        const std::uint8_t* b = block + c * kCoreSize;
        const std::uint32_t coreFlags = r32(b, 0), mask = r32(b, 4);
        if (b[448] && b[449]) warnOnce(kWarnReverb, "reverb ligado pelo jogo");
        if ((coreFlags & kCorePitchMod) && r32(b, 408)) warnOnce(kWarnPitchMod, "modulação de pitch (PMON)");
        if ((coreFlags & kCoreNoise) && r32(b, 412)) warnOnce(kWarnNoise, "gerador de ruído (NON)");
        std::uint32_t muted = 0;
        for (unsigned i = 0; i < kVoicesPerCore; ++i) {
            if (!(mask >> i & 1)) continue;
            const std::uint8_t* v = b + 8 + 16 * i;
            const std::uint32_t f = r16(v, 0);
            VoiceRegs& r = regs_[c * kVoicesPerCore + i];
            if (f & kVoiceMute) {
                r.volL = r.volR = 0;
                muted |= 1u << i;
                if (f & kVoiceAddr) r.ssa = r32(v, 8);
            } else {
                if (f & kVoiceVolL) r.volL = r16(v, 4);
                if (f & kVoiceVolR) r.volR = r16(v, 6);
            }
            if (f & kVoicePitch) r.pitch = r16(v, 2);
            if (f & kVoiceAdsr1) r.adsr1 = r16(v, 12);
            if (f & kVoiceAdsr2) r.adsr2 = r16(v, 14);
        }
        // Segunda passada: quem zerou o volume o recebe de volta.
        for (unsigned i = 0; i < kVoicesPerCore; ++i) {
            if (!(muted >> i & 1)) continue;
            const std::uint8_t* v = b + 8 + 16 * i;
            VoiceRegs& r = regs_[c * kVoicesPerCore + i];
            r.volL = r16(v, 4);
            r.volR = r16(v, 6);
        }
        for (unsigned i = 0; i < kVoicesPerCore; ++i) {
            if (mask >> i & 1) pushLive(c * kVoicesPerCore + i);
        }
        if (coreFlags & kCoreKeyOn) keyOn[c] = r32(b, 436);
        if (coreFlags & kCoreKeyOff) keyOff[c] = r32(b, 440);
    }
    for (unsigned c = 0; c < kCores; ++c) {
        for (unsigned i = 0; i < kVoicesPerCore; ++i) {
            if (!((keyOff[c] & ~keyOn[c]) >> i & 1)) continue;
            if (trace_) std::fprintf(stderr, "[iop] pdispu2: key-off voz %u.%u\n", c, i);
            spu_.keyOff(c * kVoicesPerCore + i);
        }
    }
    for (unsigned c = 0; c < kCores; ++c) {
        for (unsigned i = 0; i < kVoicesPerCore; ++i) {
            if (!(keyOn[c] >> i & 1)) continue;
            const VoiceRegs& r = regs_[c * kVoicesPerCore + i];
            if ((r.volL | r.volR) & 0x8000) warnOnce(kWarnSweep, "volume em sweep (bit 15 do VOLL/VOLR)");
            if (trace_) {
                std::fprintf(stderr, "[iop] pdispu2: key-on voz %u.%u: SPU2 0x%x, pitch 0x%x, vol %04x/%04x, adsr %04x/%04x\n", c, i, r.ssa,
                              r.pitch, r.volL, r.volR, r.adsr1, r.adsr2);
            }
            Spu2::VoiceSetup s;
            s.start = r.ssa;
            s.pitch = r.pitch;
            s.volL = r.volL;
            s.volR = r.volR;
            s.adsr1 = r.adsr1;
            s.adsr2 = r.adsr2;
            spu_.keyOn(c * kVoicesPerCore + i, s);
        }
    }
    return status();
}

std::vector<std::uint8_t> PdiSpu2::status() const {
    std::vector<std::uint8_t> out(kStatusSize, 0);
    for (unsigned c = 0; c < kCores; ++c) {
        const std::size_t base = c * (4 + 2 * kVoicesPerCore);
        std::uint32_t endx = 0;
        for (unsigned i = 0; i < kVoicesPerCore; ++i) {
            const unsigned voice = c * kVoicesPerCore + i;
            if (spu_.reachedEnd(voice)) endx |= 1u << i;
            w16(out, base + 4 + 2 * i, spu_.envelopeLevel(voice));
        }
        wr32(out, base, endx);
    }
    return out;
}

void registerPdiSpu2(Iop& iop) {
    auto st = std::make_shared<PdiSpu2>(iop.spu2());
    st->setTrace(iop.runtime().options().traceIop);
    auto handler = [&iop, st](std::uint32_t fn, const std::vector<std::uint8_t>& in,
                              std::uint32_t pc) -> std::optional<std::vector<std::uint8_t>> {
        const bool trace = iop.runtime().options().traceIop;
        std::vector<std::uint8_t> out(128, 0);
        switch (fn) {
            case 0:
                wr32(out, 0, kModuleVersion);
                wr32(out, 4, kServerSpup);
                wr32(out, 8, kServerSput);
                return out;
            case 1:
            case 2: {  // {endereço no SPU2, endereço no EE, bytes}
                const std::uint32_t spuAddr = rd32(in, 0), eeAddr = rd32(in, 4), size = rd32(in, 8);
                if (std::uint64_t{spuAddr} + size > Spu2::kRamSize) {
                    throw GuestError("pdispu2: transferência de " + std::to_string(size) + " bytes no endereço 0x" +
                                         anyps2::hex(spuAddr) + " passa dos 2 MB da RAM do SPU2",
                                     pc);
                }
                if (trace) {
                    std::fprintf(stderr, "[iop] pdispu2: %s %u bytes: SPU2 0x%x %s EE 0x%08x\n",
                                 fn == 2 ? "escreve" : "lê", size, spuAddr, fn == 2 ? "<-" : "->", eeAddr);
                }
                Memory& mem = iop.runtime().memory();
                if (fn == 2) {
                    std::vector<std::uint8_t> data(size);
                    mem.copyFromGuest(data.data(), eeAddr, size, pc);
                    iop.spu2().writeRam(spuAddr, data.data(), size);
                } else {
                    mem.copyToGuest(eeAddr, iop.spu2().ram() + spuAddr, size, pc);
                }
                return out;
            }
            case 3: {
                const auto s = st->status();
                std::copy(s.begin(), s.end(), out.begin());
                return out;
            }
            case 4: {
                const auto s = st->apply(in.data(), in.size());
                std::copy(s.begin(), s.end(), out.begin());
                return out;
            }
            case 5:  // modo da saída digital (SPDIF): sem efeito no PC
            case 6:
                if (trace) {
                    std::fprintf(stderr, "[iop] pdispu2: fn %u (saída digital) %08x %08x %08x\n", fn, rd32(in, 0),
                                 rd32(in, 4), rd32(in, 8));
                }
                return out;
            default:
                throw Unimplemented("pdispu2: função " + std::to_string(fn) + " do RPC \"SPUP\"/\"SPUT\" desconhecida", pc);
        }
    };
    iop.registerServer(kServerSpup, "pdispu2:SPUP", handler);
    iop.registerServer(kServerSput, "pdispu2:SPUT", handler);
}

}  // namespace anyps2::rt
