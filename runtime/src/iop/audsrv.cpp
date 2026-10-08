#include "anyps2/runtime/iop/audsrv.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/iop/iop.h"
#include "anyps2/runtime/iop/spu2.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

using namespace iopio;

namespace {

enum AudsrvFunction : std::uint32_t {
    AUDSRV_INIT = 0x00, AUDSRV_QUIT = 0x01, AUDSRV_FORMAT_OK = 0x02, AUDSRV_SET_FORMAT = 0x03,
    AUDSRV_PLAY_AUDIO = 0x04, AUDSRV_WAIT_AUDIO = 0x05, AUDSRV_STOP_AUDIO = 0x06, AUDSRV_SET_VOLUME = 0x07,
    AUDSRV_SET_THRESHOLD = 0x08, AUDSRV_PLAY_CD = 0x09, AUDSRV_PAUSE_CD = 0x11, AUDSRV_GET_CD_TYPE = 0x15,
    AUDSRV_INIT_ADPCM = 0x16, AUDSRV_LOAD_ADPCM = 0x17, AUDSRV_PLAY_ADPCM = 0x18, AUDSRV_SET_ADPCM_VOL = 0x19,
    AUDSRV_AVAILABLE = 0x1a, AUDSRV_QUEUED = 0x1b, AUDSRV_FREE_ADPCM = 0x1c, AUDSRV_IS_ADPCM_PLAYING = 0x1d,
};
constexpr std::int32_t kErrNotInitialized = 1, kErrFormatNotSupported = 3, kErrOutOfMemory = 4, kErrArgs = 5,
                       kErrNoMoreChannels = 7;
constexpr std::int32_t kMaxVolume = 0x3FFF;
constexpr unsigned kCore1 = 24;  // vozes do núcleo 1 no Spu2

bool formatOk(std::uint32_t freq, std::uint32_t bits, std::uint32_t channels) {
    // Mesma lista de conversores do audsrv original.
    struct F {
        std::uint32_t freq, bits, channels;
    };
    static const F kFormats[] = {{11025, 8, 1},  {11025, 8, 2},  {11025, 16, 1}, {11025, 16, 2},
                                 {12000, 16, 2}, {22050, 8, 1},  {22050, 16, 1}, {22050, 16, 2},
                                 {24000, 16, 2}, {32000, 8, 1},  {32000, 16, 1}, {32000, 16, 2},
                                 {44100, 8, 1},  {44100, 16, 1}, {44100, 16, 2}, {48000, 16, 1},
                                 {48000, 16, 2}};
    for (const F& f : kFormats) {
        if (f.freq == freq && f.bits == bits && f.channels == channels) return true;
    }
    return false;
}

}  // namespace

AudSrv::AudSrv(Iop& iop) : iop_(iop) {
    adpcmVol_.fill({0x3FFF, 0x3FFF});
}

void AudSrv::reset() {
    initialized_ = playing_ = false;
    freq_ = 48000;
    bits_ = 16;
    channels_ = 2;
    volume_ = kMaxVolume;
    ring_.clear();
    ringSize_ = 20480;
    phase_ = 0;
    haveFrame_ = false;
    waits_.clear();
    samples_.clear();
    nextSpuAddr_ = 0x5010;
    adpcmVol_.fill({0x3FFF, 0x3FFF});
}

void AudSrv::registerServer() {
    iop_.registerServer(0x0870884Eu, "audsrv",
                        [this](std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc) {
                            return rpc(fn, in, pc);
                        });
}

std::int32_t AudSrv::setFormat(std::uint32_t freq, std::uint32_t bits, std::uint32_t channels) {
    if (!formatOk(freq, bits, channels)) return -kErrFormatNotSupported;
    freq_ = freq;
    bits_ = bits;
    channels_ = channels;
    // Como o original: 10 blocos de 512 amostras de saída (~107 ms).
    std::uint32_t shift = 0;
    if (bits == 16) ++shift;
    if (channels == 2) ++shift;
    ringSize_ = ((512 * freq / 48000) << shift) * 10;
    ring_.clear();
    phase_ = 0;
    haveFrame_ = false;
    return 0;
}

std::optional<std::vector<std::uint8_t>> AudSrv::rpc(std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                     std::uint32_t pc) {
    Spu2& spu = iop_.spu2();
    auto ret = [](std::int32_t r) { return std::optional<std::vector<std::uint8_t>>(result32(r)); };
    switch (fn) {
        case AUDSRV_INIT:
            if (!initialized_) {
                initialized_ = true;
                playing_ = false;
                setFormat(48000, 16, 2);
            }
            return ret(0);
        case AUDSRV_QUIT:
            playing_ = false;
            initialized_ = false;
            for (unsigned v = 0; v < 24; ++v) spu.keyOff(kCore1 + v);
            samples_.clear();
            return ret(0);
        case AUDSRV_FORMAT_OK:
            return ret(formatOk(rd32(in, 0), rd32(in, 4), rd32(in, 8)) ? 1 : 0);
        case AUDSRV_SET_FORMAT:
            return ret(setFormat(rd32(in, 0), rd32(in, 4), rd32(in, 8)));
        case AUDSRV_PLAY_AUDIO: {  // {size, dados}
            if (!initialized_) return ret(-kErrNotInitialized);
            if (in.size() < 4) return ret(kErrArgs);
            playing_ = true;
            const std::uint32_t size = std::min<std::uint32_t>(rd32(in, 0), static_cast<std::uint32_t>(in.size() - 4));
            const std::uint32_t n = std::min(size, available());
            ring_.insert(ring_.end(), in.begin() + 4, in.begin() + 4 + n);
            return ret(static_cast<std::int32_t>(n));
        }
        case AUDSRV_WAIT_AUDIO: {
            const std::uint32_t bytes = rd32(in, 0);
            if (bytes > ringSize_) return ret(kErrArgs);
            if (available() >= bytes) return ret(0);
            waits_.push_back({iop_.deferCurrentCall(), bytes});
            return std::nullopt;
        }
        case AUDSRV_STOP_AUDIO:
            playing_ = false;
            return ret(0);
        case AUDSRV_SET_VOLUME: {
            const auto v = static_cast<std::int32_t>(rd32(in, 0));
            if (v < 0 || v > kMaxVolume) return ret(kErrArgs);
            volume_ = v;
            return ret(0);
        }
        case AUDSRV_SET_THRESHOLD:
            if (rd32(in, 0) == 0) return ret(0);
            throw Unimplemented("audsrv_on_fillbuf: o callback de \"buffer com espaço\" exige o IOP chamar um "
                                "servidor RPC do EE, ainda não suportado — use audsrv_wait_audio",
                                pc);
        case AUDSRV_AVAILABLE:
            return ret(static_cast<std::int32_t>(available()));
        case AUDSRV_QUEUED:
            return ret(static_cast<std::int32_t>(ring_.size()));
        case AUDSRV_INIT_ADPCM:
            for (unsigned v = 0; v < 24; ++v) {
                spu.keyOff(kCore1 + v);
                spu.setVolume(kCore1 + v, 0x3FFF, 0x3FFF);
            }
            adpcmVol_.fill({0x3FFF, 0x3FFF});
            samples_.clear();
            nextSpuAddr_ = 0x5010;
            return ret(0);
        case AUDSRV_LOAD_ADPCM: {  // {endereço no IOP, tamanho, id} -> {r, pitch, loop, canais}
            const std::uint32_t addr = rd32(in, 0), size = rd32(in, 4), id = rd32(in, 8);
            std::vector<std::uint8_t> out(16, 0);
            auto it = samples_.find(id);
            if (it == samples_.end()) {
                if (size < 16) return ret(kErrArgs);
                // Cabeçalho de 16 bytes do audsrv: [1] = canais<<8 | loop<<16, [2] = pitch.
                const std::uint8_t* src = iop_.iopPointer(addr, size, pc);
                std::uint32_t h1 = 0, h2 = 0;
                std::memcpy(&h1, src + 4, 4);
                std::memcpy(&h2, src + 8, 4);
                Sample s;
                s.spuAddr = nextSpuAddr_;
                s.size = size - 16;
                s.pitch = h2;
                s.loop = (h1 >> 16) & 0xFF;
                s.channels = (h1 >> 8) & 0xFF;
                if (!spu.writeRam(s.spuAddr, src + 16, s.size)) {
                    wr32(out, 0, static_cast<std::uint32_t>(-kErrOutOfMemory));
                    return out;
                }
                nextSpuAddr_ = s.spuAddr + s.size;
                it = samples_.emplace(id, s).first;
            }
            wr32(out, 4, it->second.pitch);
            wr32(out, 8, it->second.loop);
            wr32(out, 12, it->second.channels);
            return out;
        }
        case AUDSRV_PLAY_ADPCM: {  // {canal (<0: qualquer), id}
            const auto ch = static_cast<std::int32_t>(rd32(in, 0));
            auto it = samples_.find(rd32(in, 4));
            if (it == samples_.end()) return ret(kErrArgs);
            std::int32_t channel = -1;
            if (ch >= 0 && ch < 24) {
                if (spu.active(kCore1 + static_cast<unsigned>(ch))) return ret(-kErrNoMoreChannels);
                channel = ch;
            } else {
                for (unsigned v = 1; v < 24 && channel < 0; ++v) {
                    if (!spu.active(kCore1 + v)) channel = static_cast<std::int32_t>(v);
                }
                if (channel < 0) return ret(-kErrNoMoreChannels);
            }
            const auto voice = kCore1 + static_cast<unsigned>(channel);
            Spu2::VoiceSetup setup;
            setup.start = it->second.spuAddr;
            setup.pitch = static_cast<std::uint16_t>(it->second.pitch);
            // Volume e ADSR ficam como o programa/audsrv deixaram (ADSR 0 após sceSdInit).
            setup.volL = setup.volR = 0x3FFF;
            spu.keyOn(voice, setup);
            const auto& vol = adpcmVol_[static_cast<std::size_t>(channel)];
            spu.setVolume(voice, vol.first, vol.second);
            return ret(channel);
        }
        case AUDSRV_SET_ADPCM_VOL: {  // {canal, volL, volR}
            const auto ch = rd32(in, 0);
            const auto l = static_cast<std::int32_t>(rd32(in, 4)), r = static_cast<std::int32_t>(rd32(in, 8));
            if (l < 0 || l > kMaxVolume || r < 0 || r > kMaxVolume || ch >= 24) return ret(kErrArgs);
            adpcmVol_[ch] = {static_cast<std::uint16_t>(l), static_cast<std::uint16_t>(r)};
            spu.setVolume(kCore1 + ch, static_cast<std::uint16_t>(l), static_cast<std::uint16_t>(r));
            return ret(0);
        }
        case AUDSRV_IS_ADPCM_PLAYING: {  // {canal, id}
            const std::uint32_t ch = rd32(in, 0);
            if (ch >= 24) return ret(0);
            if (!spu.active(kCore1 + ch)) return ret(0);
            auto it = samples_.find(rd32(in, 4));
            if (it == samples_.end()) return ret(kErrArgs);
            return ret(spu.startAddress(kCore1 + ch) == it->second.spuAddr ? 1 : 0);
        }
        case AUDSRV_FREE_ADPCM:
            samples_.erase(rd32(in, 0));  // como o original, a RAM do SPU2 não é reaproveitada
            return ret(0);
        default:
            if (fn >= AUDSRV_PLAY_CD && fn <= AUDSRV_GET_CD_TYPE) {
                throw Unimplemented("audsrv: função de CD-DA " + std::to_string(fn) +
                                        " (faixas de áudio do disco) não suportada no HLE",
                                    pc);
            }
            throw Unimplemented("audsrv: função " + std::to_string(fn) + " desconhecida", pc);
    }
}

void AudSrv::render(std::int32_t* mix, std::size_t frames) {
    if (!playing_) return;
    const std::uint32_t fb = frameBytes();
    auto sample16 = [&](std::size_t off) -> std::int32_t {
        if (bits_ == 8) return static_cast<std::int32_t>(ring_[off]) * 257 - 32768;
        return static_cast<std::int16_t>(static_cast<std::uint16_t>(ring_[off] | (ring_[off + 1] << 8)));
    };
    const std::int32_t vol = static_cast<std::int16_t>(static_cast<std::uint16_t>(volume_ << 1));
    for (std::size_t f = 0; f < frames; ++f) {
        // Avança na origem: freq/48000 amostras por amostra de saída.
        while (phase_ >= Spu2::kRate || !haveFrame_) {
            if (ring_.size() < fb) {
                haveFrame_ = false;
                break;
            }
            curL_ = sample16(0);
            curR_ = channels_ == 2 ? sample16(bits_ / 8) : curL_;
            ring_.erase(ring_.begin(), ring_.begin() + fb);
            if (haveFrame_) phase_ -= Spu2::kRate;
            haveFrame_ = true;
        }
        if (!haveFrame_) {
            phase_ = 0;
            return;  // buffer vazio: silêncio até o EE mandar mais
        }
        mix[2 * f] += curL_ * vol >> 15;
        mix[2 * f + 1] += curR_ * vol >> 15;
        phase_ += freq_;
    }
}

void AudSrv::update(std::uint32_t pc) {
    for (std::size_t i = 0; i < waits_.size();) {
        if (available() >= waits_[i].bytes) {
            const std::uint32_t token = waits_[i].token;
            waits_.erase(waits_.begin() + static_cast<std::ptrdiff_t>(i));
            iop_.completeDeferred(token, result32(0), pc);
        } else {
            ++i;
        }
    }
}

}  // namespace anyps2::rt
