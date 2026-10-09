// Menu de configuração desenhado com Dear ImGui sobre a janela SDL (F1). Só
// desenha e edita a cópia em ConfigMenu::edit; a captura de teclas e botões
// e a troca das ligações ficam em video_sdl.cpp.

#include "video_menu.h"

#include <cstdio>
#include <exception>
#include <string>

#include "anyps2/runtime/config.h"
#include "anyps2/runtime/input.h"
#include "imgui.h"

namespace anyps2::rt {

namespace {

// Mesma ordem de kPadButtonNames (bit 0 = select).
const char* const kButtonLabels[16] = {"Select", "L3",      "R3",     "Start",   "Cima",     "Direita",
                                       "Baixo",  "Esquerda", "L2",   "R2",      "L1",       "R1",
                                       "Triângulo", "Círculo", "Xis", "Quadrado"};
const char* const kAnalogLabels[4] = {"Analógico esquerdo, horizontal", "Analógico esquerdo, vertical",
                                      "Analógico direito, horizontal", "Analógico direito, vertical"};

// Campo de texto ligado a uma string (o ImGui edita um buffer de C).
void textField(const char* label, std::string& value) {
    char buf[512];
    std::snprintf(buf, sizeof buf, "%s", value.c_str());
    if (ImGui::InputText(label, buf, sizeof buf)) value = buf;
}

// Lista de modos (sdl, none, auto); "auto" é o vazio em RuntimeOptions.
void modeCombo(const char* label, std::string& value) {
    const std::string current = value.empty() ? "auto" : value;
    if (ImGui::BeginCombo(label, current.c_str())) {
        for (const char* mode : {"sdl", "none", "auto"}) {
            if (ImGui::Selectable(mode, current == mode)) {
                value = std::string(mode) == "auto" ? std::string() : std::string(mode);
            }
        }
        ImGui::EndCombo();
    }
}

// Botão que mostra a ligação atual. Clicar põe a janela em "aperte..." para a
// próxima tecla, botão ou eixo; "Limpar" desliga a ligação.
void bindingCell(ConfigMenu& m, unsigned port, ConfigMenu::Kind kind, int slot, std::string& value) {
    const std::string id = "##" + std::to_string(port) + "_" + std::to_string(static_cast<int>(kind)) + "_" +
                           std::to_string(slot);
    const bool listening = m.listenPort == static_cast<int>(port) && m.listenKind == kind && m.listenSlot == slot;
    const std::string shown = listening ? std::string("aperte...") : (value.empty() ? std::string("-") : value);
    if (ImGui::Button((shown + id).c_str(), ImVec2(170, 0))) {
        m.listenPort = static_cast<int>(port);
        m.listenKind = kind;
        m.listenSlot = slot;
        m.status = "Aperte a tecla ou o botão (Esc cancela).";
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(("Limpar" + id).c_str())) {
        value.clear();
        m.changed = true;
    }
}

// Tabela de uma porta: uma linha por botão do DS2 e uma por analógico.
void controlsTab(ConfigMenu& m, unsigned port) {
    const std::string tableId = "ligacoes" + std::to_string(port);
    if (!ImGui::BeginTable(tableId.c_str(), 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) return;
    ImGui::TableSetupColumn("Botão");
    ImGui::TableSetupColumn("Teclado");
    ImGui::TableSetupColumn("Controle");
    ImGui::TableHeadersRow();
    for (int i = 0; i < 16; ++i) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(kButtonLabels[i]);
        ImGui::TableSetColumnIndex(1);
        bindingCell(m, port, ConfigMenu::Kind::Key, i, m.edit.pad.key[port][i]);
        ImGui::TableSetColumnIndex(2);
        bindingCell(m, port, ConfigMenu::Kind::Pad, i, m.edit.pad.button[port][i]);
    }
    for (int a = 0; a < 4; ++a) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(kAnalogLabels[a]);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted("-");
        ImGui::TableSetColumnIndex(2);
        bindingCell(m, port, ConfigMenu::Kind::Analog, a, m.edit.pad.analog[port][a]);
    }
    ImGui::EndTable();
}

void videoAudioTab(ConfigMenu& m) {
    RuntimeOptions& o = m.edit;
    modeCombo("Janela", o.video);
    int scale = o.videoScale;
    if (ImGui::SliderInt("Escala da janela", &scale, 1, 4)) o.videoScale = scale;
    ImGui::Spacing();
    modeCombo("Som", o.audio);
    textField("Gravar o som em WAV", o.audioWav);
    ImGui::Spacing();
    ImGui::TextUnformatted("Janela, escala e som valem na próxima execução.");
}

}  // namespace

void drawConfigMenu(ConfigMenu& m) {
    ImGui::SetNextWindowPos(ImVec2(8, 8), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(624, 464), ImGuiCond_Always);
    ImGui::Begin("Configuração do AnyPS2", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);
    ImGui::TextUnformatted("F1 fecha o menu. Com ele aberto, o jogo não recebe entradas.");
    if (ImGui::BeginTabBar("abas")) {
        if (ImGui::BeginTabItem("Controles porta 1")) {
            controlsTab(m, 0);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controles porta 2")) {
            controlsTab(m, 1);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Vídeo e som")) {
            videoAudioTab(m);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Cartões")) {
            textField("Pasta dos cartões (mc0 e mc1)", m.edit.memcardDir);
            ImGui::TextUnformatted("Vazia = $ANYPS2_HOST_DIR/memcard.");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Disco")) {
            textField("Imagem do disco (ISO ou BIN)", m.edit.iso);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Salvar anyps2.ini")) {
        try {
            saveConfigFile(m.edit, m.edit.configPath);
            m.status = "Gravado em " + m.edit.configPath;
        } catch (const std::exception& e) {
            m.status = e.what();
        }
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(m.status.c_str());
    ImGui::End();
}

}  // namespace anyps2::rt
