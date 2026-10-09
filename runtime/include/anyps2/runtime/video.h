#pragma once

#include <memory>
#include <string>

#include "anyps2/runtime/gs/gs.h"

namespace anyps2::rt {

struct RuntimeOptions;
class Input;

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
// input: recebe teclado/controles da janela (pode ser nullptr).
std::unique_ptr<Video> createVideo(const RuntimeOptions& options, const std::string& title, Input* input);
// Backend SDL2; nullptr se o runtime foi compilado sem SDL ou não há display.
// Com options.video vazio (automático), recusa drivers sem tela (offscreen/dummy).
// options.videoScale e options.pad configuram a janela e as ligações.
std::unique_ptr<Video> createSdlVideo(const RuntimeOptions& options, const std::string& title, Input* input,
                                      std::string& error);

// Grava a imagem como PNG RGB de 8 bits. A compressão é determinística: a
// mesma imagem gera sempre os mesmos bytes (os testes comparam arquivos).
void writePng(const std::string& path, const gs::Frame& frame);

}  // namespace anyps2::rt
