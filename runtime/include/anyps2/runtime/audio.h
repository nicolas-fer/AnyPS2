#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace anyps2::rt {

struct RuntimeOptions;

// Destino das amostras mixadas pelo SPU2 (estéreo, 16 bits, 48 kHz).
class AudioSink {
public:
    virtual ~AudioSink() = default;
    virtual void write(const std::int16_t* stereo, std::size_t frames) = 0;
};

// Saída de áudio do runtime. O mixer roda no tempo emulado (a cada VBlank
// gera as amostras do intervalo), então com ANYPS2_CLOCK=virtual o WAV é
// idêntico em toda execução.
//   ANYPS2_AUDIO     = "sdl" (alto-falantes), "none" ou "" (automático:
//                      SDL se houver dispositivo de áudio)
//   ANYPS2_AUDIO_WAV = grava também um WAV com tudo o que foi tocado
class Audio {
public:
    explicit Audio(const RuntimeOptions& options);
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    void write(const std::int16_t* stereo, std::size_t frames);

private:
    void finishWav();

    std::unique_ptr<AudioSink> device_;
    std::FILE* wav_ = nullptr;
    std::string wavPath_;
    std::uint64_t wavFrames_ = 0;
};

// Backend SDL2; nullptr (com o motivo em error) se não houver SDL ou
// dispositivo. requireDevice: recusa drivers sem som (dummy/disk).
std::unique_ptr<AudioSink> createSdlAudio(bool requireDevice, std::string& error);

}  // namespace anyps2::rt
