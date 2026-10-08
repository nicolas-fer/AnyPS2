// Runtime compilado sem SDL2 (ANYPS2_WITH_SDL=OFF): só o modo sem janela.

#include "anyps2/runtime/video.h"

namespace anyps2::rt {

std::unique_ptr<Video> createSdlVideo(const std::string&, bool, std::string& error) {
    error = "o runtime foi compilado sem SDL2 (ANYPS2_WITH_SDL=OFF)";
    return nullptr;
}

}  // namespace anyps2::rt
