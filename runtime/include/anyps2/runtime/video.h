#pragma once

#include <memory>
#include <string>

#include "anyps2/runtime/gs/gs.h"

namespace anyps2::rt {

struct RuntimeOptions;

// Saída de vídeo. O GS compõe a imagem exibida em software; o Video só a
// apresenta (janela SDL2) ou a descarta (modo sem janela, usado nos testes).
class Video {
public:
    virtual ~Video() = default;
    // Chamado a cada VBlank com a imagem composta pelo GS.
    virtual void present(const gs::Frame& frame) = 0;
    // false quando o usuário fechou a janela.
    virtual bool pollEvents() = 0;
};

// Escolhe o backend conforme RuntimeOptions::video ("sdl", "none" ou "").
std::unique_ptr<Video> createVideo(const RuntimeOptions& options, const std::string& title);
// Backend SDL2; nullptr se o runtime foi compilado sem SDL ou não há display.
// requireDisplay: recusa drivers sem tela (offscreen/dummy) — modo automático.
std::unique_ptr<Video> createSdlVideo(const std::string& title, bool requireDisplay, std::string& error);

// Grava a imagem como PNG RGB de 8 bits. A compressão é determinística: a
// mesma imagem gera sempre os mesmos bytes (os testes comparam arquivos).
void writePng(const std::string& path, const gs::Frame& frame);

}  // namespace anyps2::rt
