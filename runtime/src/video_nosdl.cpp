// Runtime compilado sem SDL2 (ANYPS2_WITH_SDL=OFF): só o modo sem janela.

#include "anyps2/runtime/config.h"
#include "anyps2/runtime/video.h"

namespace anyps2::rt {

std::unique_ptr<Video> createSdlVideo(const RuntimeOptions&, const std::string&, Input*, std::string& error) {
    error = "o runtime foi compilado sem SDL2 (ANYPS2_WITH_SDL=OFF)";
    return nullptr;
}

// Sem SDL não há como conferir os nomes: aceita todos (e não há janela).
bool knownSdlKey(const std::string&) { return true; }
bool knownSdlButton(const std::string&) { return true; }
bool knownSdlAxis(const std::string&) { return true; }

}  // namespace anyps2::rt
