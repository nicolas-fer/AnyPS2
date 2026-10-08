// Som pelo SDL2: as amostras mixadas a cada VBlank entram na fila do
// dispositivo (SDL_QueueAudio).

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <string>

#include "anyps2/runtime/audio.h"

namespace anyps2::rt {

namespace {

class SdlAudio final : public AudioSink {
public:
    explicit SdlAudio(SDL_AudioDeviceID dev) : dev_(dev) { SDL_PauseAudioDevice(dev_, 0); }
    ~SdlAudio() override {
        SDL_CloseAudioDevice(dev_);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    void write(const std::int16_t* stereo, std::size_t frames) override {
        // Se o programa roda mais rápido que o tempo real (relógio virtual),
        // não acumula atraso: com mais de 250 ms na fila, descarta.
        if (SDL_GetQueuedAudioSize(dev_) > kMaxQueued) return;
        SDL_QueueAudio(dev_, stereo, static_cast<Uint32>(frames * 4));
    }

private:
    static constexpr Uint32 kMaxQueued = 48000 * 4 / 4;
    SDL_AudioDeviceID dev_;
};

}  // namespace

std::unique_ptr<AudioSink> createSdlAudio(bool requireDevice, std::string& error) {
    SDL_SetMainReady();
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        error = std::string("SDL_Init(áudio): ") + SDL_GetError();
        return nullptr;
    }
    const std::string driver = SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "";
    if (requireDevice && (driver == "dummy" || driver == "disk" || driver.empty())) {
        error = "sem dispositivo de áudio (driver " + driver + ")";
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return nullptr;
    }
    SDL_AudioSpec want{};
    want.freq = 48000;
    want.format = AUDIO_S16LSB;
    want.channels = 2;
    want.samples = 1024;
    SDL_AudioSpec have{};
    const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (dev == 0) {
        error = std::string("SDL_OpenAudioDevice: ") + SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return nullptr;
    }
    return std::make_unique<SdlAudio>(dev);
}

}  // namespace anyps2::rt
