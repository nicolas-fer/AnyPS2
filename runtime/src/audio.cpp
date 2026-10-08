#include "anyps2/runtime/audio.h"

#include <algorithm>
#include <cstring>

#include "anyps2/common/error.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

namespace {

void le16(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void le32(std::uint8_t* p, std::uint32_t v) {
    le16(p, v & 0xFFFF);
    le16(p + 2, v >> 16);
}

// Cabeçalho RIFF/WAVE de PCM 16 bits estéreo 48 kHz.
void wavHeader(std::uint8_t h[44], std::uint64_t frames) {
    const auto data = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames * 4, 0xFFFFFFFFull - 36));
    std::memcpy(h, "RIFF", 4);
    le32(h + 4, 36 + data);
    std::memcpy(h + 8, "WAVEfmt ", 8);
    le32(h + 16, 16);
    le16(h + 20, 1);      // PCM
    le16(h + 22, 2);      // canais
    le32(h + 24, 48000);  // taxa
    le32(h + 28, 48000 * 4);
    le16(h + 32, 4);
    le16(h + 34, 16);
    std::memcpy(h + 36, "data", 4);
    le32(h + 40, data);
}

}  // namespace

Audio::Audio(const RuntimeOptions& options) {
    if (!options.audioWav.empty()) {
        wav_ = std::fopen(options.audioWav.c_str(), "wb");
        if (!wav_) throw anyps2::Error("não foi possível criar o WAV '" + options.audioWav + "' (ANYPS2_AUDIO_WAV)");
        wavPath_ = options.audioWav;
        std::uint8_t h[44];
        wavHeader(h, 0);
        std::fwrite(h, 1, sizeof(h), wav_);
    }
    if (options.audio == "none") return;
    if (!options.audio.empty() && options.audio != "sdl") {
        throw anyps2::Error("ANYPS2_AUDIO='" + options.audio + "' inválido (use sdl ou none)");
    }
    std::string error;
    device_ = createSdlAudio(options.audio.empty(), error);
    if (!device_ && options.audio == "sdl") throw anyps2::Error("não foi possível abrir o áudio: " + error);
}

Audio::~Audio() {
    finishWav();
}

void Audio::finishWav() {
    if (!wav_) return;
    std::uint8_t h[44];
    wavHeader(h, wavFrames_);
    std::fseek(wav_, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof(h), wav_);
    std::fclose(wav_);
    wav_ = nullptr;
}

void Audio::write(const std::int16_t* stereo, std::size_t frames) {
    if (wav_) {
        // Little-endian explícito: o WAV sai igual em qualquer host.
        std::uint8_t buf[4096];
        std::size_t done = 0;
        while (done < frames) {
            const std::size_t n = std::min<std::size_t>(frames - done, sizeof(buf) / 4);
            for (std::size_t i = 0; i < n * 2; ++i) {
                le16(buf + 2 * i, static_cast<std::uint16_t>(stereo[2 * done + i]));
            }
            std::fwrite(buf, 1, n * 4, wav_);
            done += n;
        }
        wavFrames_ += frames;
    }
    if (device_) device_->write(stereo, frames);
}

}  // namespace anyps2::rt
