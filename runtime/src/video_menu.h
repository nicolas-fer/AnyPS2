#pragma once

#include <string>

#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

// Estado do menu de configuração (F1 na janela). Edita uma cópia das opções;
// "Salvar" grava essa cópia no arquivo de configuração usado (configPath).
struct ConfigMenu {
    enum class Kind { Key, Pad, Analog };
    bool open = false;
    RuntimeOptions edit;
    // Captura em andamento: porta (-1 = nenhuma), tipo e índice do botão ou
    // do analógico. A janela (video_sdl.cpp) é quem recebe a próxima entrada.
    int listenPort = -1;
    Kind listenKind = Kind::Key;
    int listenSlot = -1;
    bool changed = false;  // as ligações mudaram: a janela refaz as ligações
    std::string status;    // última mensagem (gravado, cancelado, erro)
};

// Desenha o menu (entre ImGui::NewFrame e ImGui::Render).
void drawConfigMenu(ConfigMenu& menu);

}  // namespace anyps2::rt
