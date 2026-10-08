// Runtime compilado sem SDL2 (ANYPS2_WITH_SDL=OFF): sem saída de som (o WAV
// continua disponível).

#include "anyps2/runtime/audio.h"

namespace anyps2::rt {

std::unique_ptr<AudioSink> createSdlAudio(bool, std::string& error) {
    error = "o runtime foi compilado sem SDL2 (ANYPS2_WITH_SDL=OFF)";
    return nullptr;
}

}  // namespace anyps2::rt
