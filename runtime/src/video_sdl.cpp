// Janela SDL2. A janela, o renderer e o laço de eventos vivem numa thread
// própria: as threads do guest rodam em várias threads do host, e no Windows
// só a thread que criou a janela recebe as mensagens dela.
//
// A mesma thread lê o teclado e os controles (SDL_GameController) e
// atualiza o Input: cada porta (1 e 2) tem as ligações de RuntimeOptions::pad
// (arquivo de configuração; padrões: setas, Z ✕, X ○, A □, S △, Q/W = L1/R1,
// 1/2 = L2/R2, Enter = START, Backspace = SELECT no teclado da porta 1, e o
// controle SDL: 1º controle = porta 1, 2º = porta 2).
//
// F1 abre o menu de configuração (video_menu.cpp, Dear ImGui). Com o menu
// aberto o jogo recebe o controle neutro; a captura de uma nova ligação
// consome a próxima tecla, botão ou eixo.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <array>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "anyps2/runtime/config.h"
#include "anyps2/runtime/input.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/video.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"
#include "imgui.h"
#include "video_menu.h"

namespace anyps2::rt {

namespace {

// Nome do SDL para tecla ("Z", "Up", "Return"); letra única vira minúscula,
// porque é assim que o SDL a reconhece.
SDL_Keycode keycodeFromName(std::string name) {
    if (name.size() == 1) name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
    return SDL_GetKeyFromName(name.c_str());
}

// Eixo do SDL (-32768..32767) -> analógico do DualShock (0..255, 0x80 = centro).
std::uint8_t axisByte(Sint16 v) {
    return static_cast<std::uint8_t>((static_cast<int>(v) + 32768) >> 8);
}

// Campo analógico de PadInput: 0 = lx, 1 = ly, 2 = rx, 3 = ry.
std::uint8_t& analogField(PadInput& s, int index) {
    switch (index) {
        case 0: return s.lx;
        case 1: return s.ly;
        case 2: return s.rx;
        default: return s.ry;
    }
}

// Gatilhos são eixos que só dão valores positivos: a captura não põe sinal neles.
bool isTriggerAxis(int axis) {
    return axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
}

class SdlVideo final : public Video {
public:
    SdlVideo(const RuntimeOptions& options, std::string title, Input* input)
        : title_(std::move(title)),
          requireDisplay_(options.video.empty()),
          scale_(options.videoScale < 1 ? 1 : options.videoScale),
          input_(input) {
        menu_.edit = options;
        bind(menu_.edit.pad);
    }

    bool start(std::string& error) {
        std::unique_lock lock(mutex_);
        thread_ = std::thread([this] { loop(); });
        cv_.wait(lock, [this] { return started_; });
        if (!ok_) {
            error = error_;
            lock.unlock();
            thread_.join();
            return false;
        }
        return true;
    }

    ~SdlVideo() override {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void present(const gs::Frame& frame) override {
        {
            std::lock_guard lock(mutex_);
            frame_ = frame;
            hasFrame_ = true;
        }
        cv_.notify_all();
    }

    bool pollEvents() override { return !closed_.load(); }

private:
    // Uma tecla do teclado (porta indica quem recebe o botão).
    struct KeyBind {
        SDL_Keycode key;
        unsigned port;
        std::uint16_t bit;
    };
    // Um botão do controle da porta; bit = botão do DS2.
    struct ButtonBind {
        SDL_GameControllerButton button;
        unsigned port;
        std::uint16_t bit;
    };
    // Um eixo do controle: analog >= 0 é o analógico (0..3); senão o eixo
    // aciona o botão digital bit conforme o sentido dir (padAxisPressed).
    struct AxisBind {
        SDL_GameControllerAxis axis;
        unsigned port;
        int analog;
        std::uint16_t bit;
        int dir;  // 0 = os dois sentidos, +1, -1
    };
    struct Pad {
        SDL_GameController* handle = nullptr;
        SDL_JoystickID id = -1;
        PadInput state;
    };

    // Resolve os nomes da configuração para códigos do SDL. O arquivo já foi
    // conferido ao ser lido (config.cpp); o que não resolve aqui é ignorado.
    void bind(const PadBindings& pad) {
        for (unsigned port = 0; port < 2; ++port) {
            for (unsigned i = 0; i < 16; ++i) {
                const std::uint16_t bit = static_cast<std::uint16_t>(1u << i);
                const std::string& key = pad.key[port][i];
                if (!key.empty()) {
                    if (const SDL_Keycode k = keycodeFromName(key); k != SDLK_UNKNOWN) {
                        keyBinds_.push_back({k, port, bit});
                    }
                }
                const std::string& button = pad.button[port][i];
                if (!button.empty()) {
                    int dir = 0;
                    const std::string name = padAxisName(button, dir);
                    if (const SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(name.c_str());
                        b != SDL_CONTROLLER_BUTTON_INVALID) {
                        buttonBinds_.push_back({b, port, bit});
                    } else if (const SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(name.c_str());
                               a != SDL_CONTROLLER_AXIS_INVALID) {
                        axisBinds_.push_back({a, port, -1, bit, dir});
                    }
                }
            }
            for (int a = 0; a < 4; ++a) {
                const std::string& axis = pad.analog[port][a];
                if (axis.empty()) continue;
                if (const SDL_GameControllerAxis s = SDL_GameControllerGetAxisFromString(axis.c_str());
                    s != SDL_CONTROLLER_AXIS_INVALID) {
                    axisBinds_.push_back({s, port, a, 0, 0});
                }
            }
        }
    }

    // Refaz as ligações depois de uma mudança no menu.
    void rebind() {
        keyBinds_.clear();
        buttonBinds_.clear();
        axisBinds_.clear();
        bind(menu_.edit.pad);
    }

    // Captura de uma nova ligação: a próxima tecla, botão ou eixo do tipo
    // pedido vira a ligação; Esc cancela. Devolve true se consumiu o evento.
    bool capture(const SDL_Event& ev) {
        ConfigMenu& m = menu_;
        const unsigned port = static_cast<unsigned>(m.listenPort);
        const int slot = m.listenSlot;
        std::string name;
        switch (ev.type) {
            case SDL_KEYDOWN:
                if (ev.key.repeat) return true;
                if (ev.key.keysym.sym == SDLK_ESCAPE) {
                    m.listenPort = -1;
                    m.status = "Cancelado.";
                    return true;
                }
                if (m.listenKind == ConfigMenu::Kind::Key) name = SDL_GetKeyName(ev.key.keysym.sym);
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (m.listenKind != ConfigMenu::Kind::Key) {
                    const char* b = SDL_GameControllerGetStringForButton(
                        static_cast<SDL_GameControllerButton>(ev.cbutton.button));
                    if (b && m.listenKind == ConfigMenu::Kind::Pad) name = b;
                }
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (m.listenKind != ConfigMenu::Kind::Key && std::abs(static_cast<int>(ev.caxis.value)) > 16000) {
                    const char* a = SDL_GameControllerGetStringForAxis(
                        static_cast<SDL_GameControllerAxis>(ev.caxis.axis));
                    if (a) {
                        name = a;
                        // Botão digital com eixo guarda o sentido: "+leftx" ou "-leftx".
                        if (m.listenKind == ConfigMenu::Kind::Pad && !isTriggerAxis(ev.caxis.axis)) {
                            name = (ev.caxis.value < 0 ? "-" : "+") + name;
                        }
                    }
                }
                break;
            default:
                return false;
        }
        if (name.empty() || name == "Unknown") return true;  // não serve: continua esperando
        switch (m.listenKind) {
            case ConfigMenu::Kind::Key: m.edit.pad.key[port][slot] = name; break;
            case ConfigMenu::Kind::Pad: m.edit.pad.button[port][slot] = name; break;
            case ConfigMenu::Kind::Analog: m.edit.pad.analog[port][slot] = name; break;
        }
        m.listenPort = -1;
        m.changed = true;
        m.status = "Ligação alterada. Clique em Salvar para gravar o arquivo.";
        return true;
    }

    void fail(const std::string& what) {
        std::lock_guard lock(mutex_);
        error_ = what + ": " + SDL_GetError();
        started_ = true;
        ok_ = false;
        cv_.notify_all();
    }

    void loop() {
        SDL_SetMainReady();
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
            fail("SDL_Init");
            return;
        }
        // Modo automático: um driver sem display de verdade não serve.
        const std::string driver = SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "";
        if (requireDisplay_ && (driver == "offscreen" || driver == "dummy")) {
            SDL_SetError("sem display (driver %s)", driver.c_str());
            fail("SDL");
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return;
        }
        const int width = 640 * scale_, height = 480 * scale_;
        SDL_Window* window = SDL_CreateWindow(("AnyPS2 — " + title_).c_str(), SDL_WINDOWPOS_CENTERED,
                                              SDL_WINDOWPOS_CENTERED, width, height, SDL_WINDOW_RESIZABLE);
        if (!window) {
            fail("SDL_CreateWindow");
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return;
        }
        SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, 0);
        if (!renderer) {
            fail("SDL_CreateRenderer");
            SDL_DestroyWindow(window);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            return;
        }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
        // Controles: falha aqui só deixa sem gamepad (teclado continua).
        const bool controllers = SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == 0;
        SDL_RenderSetLogicalSize(renderer, 640, 480);  // proporção 4:3 da TV
        // Menu de configuração: o ImGui trabalha em coordenadas lógicas (640x480).
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;  // sem imgui.ini
        ImGui::StyleColorsDark();
        ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
        ImGui_ImplSDLRenderer2_Init(renderer);
        {
            std::lock_guard lock(mutex_);
            started_ = true;
            ok_ = true;
        }
        cv_.notify_all();

        SDL_Texture* texture = nullptr;
        std::uint32_t texW = 0, texH = 0;
        gs::Frame local;
        for (;;) {
            bool draw = false;
            {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(16), [this] { return stop_ || hasFrame_; });
                if (stop_) break;
                if (hasFrame_) {
                    local.width = frame_.width;
                    local.height = frame_.height;
                    local.pixels.swap(frame_.pixels);
                    hasFrame_ = false;
                    draw = local.width > 0 && local.height > 0;
                }
            }
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) closed_ = true;
                if (menu_.open) ImGui_ImplSDL2_ProcessEvent(&ev);
                handleInput(ev);
            }
            if (draw) {
                if (!texture || texW != local.width || texH != local.height) {
                    if (texture) SDL_DestroyTexture(texture);
                    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                                static_cast<int>(local.width), static_cast<int>(local.height));
                    texW = local.width;
                    texH = local.height;
                }
                if (texture) SDL_UpdateTexture(texture, nullptr, local.pixels.data(), static_cast<int>(local.width * 4));
            }
            SDL_RenderClear(renderer);
            if (texture) SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            if (menu_.open) drawMenu(renderer);
            SDL_RenderPresent(renderer);
        }
        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        if (texture) SDL_DestroyTexture(texture);
        for (auto& c : pads_) {
            if (c.handle) SDL_GameControllerClose(c.handle);
        }
        if (controllers) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    // Um quadro do menu por cima da imagem do jogo.
    void drawMenu(SDL_Renderer* renderer) {
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::GetIO().DisplaySize = ImVec2(640, 480);
        ImGui::NewFrame();
        drawConfigMenu(menu_);
        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (menu_.changed) {
            menu_.changed = false;
            rebind();
        }
    }

    void handleInput(const SDL_Event& ev) {
        if (!input_) return;
        if (menu_.listenPort >= 0 && capture(ev)) return;
        bool changed = false;
        switch (ev.type) {
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F1 && !ev.key.repeat) {
                    menu_.open = !menu_.open;
                    menu_.listenPort = -1;
                    changed = true;
                    break;
                }
                for (const KeyBind& kb : keyBinds_) {
                    if (kb.key != ev.key.keysym.sym) continue;
                    if (ev.type == SDL_KEYDOWN) keys_[kb.port] |= kb.bit;
                    else keys_[kb.port] = static_cast<std::uint16_t>(keys_[kb.port] & ~kb.bit);
                    changed = true;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
                for (auto& c : pads_) {
                    if (!c.handle) {
                        c.handle = SDL_GameControllerOpen(ev.cdevice.which);
                        c.id = c.handle ? SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c.handle)) : -1;
                        c.state = PadInput{};
                        break;
                    }
                }
                changed = true;
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                for (auto& c : pads_) {
                    if (c.handle && c.id == ev.cdevice.which) {
                        SDL_GameControllerClose(c.handle);
                        c = Pad{};
                    }
                }
                changed = true;
                break;
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
                for (unsigned p = 0; p < 2; ++p) {
                    Pad& c = pads_[p];
                    if (!c.handle || c.id != ev.cbutton.which) continue;
                    for (const ButtonBind& bb : buttonBinds_) {
                        if (bb.port != p || bb.button != ev.cbutton.button) continue;
                        if (ev.type == SDL_CONTROLLERBUTTONDOWN) c.state.buttons |= bb.bit;
                        else c.state.buttons = static_cast<std::uint16_t>(c.state.buttons & ~bb.bit);
                        changed = true;
                    }
                }
                break;
            case SDL_CONTROLLERAXISMOTION:
                for (unsigned p = 0; p < 2; ++p) {
                    Pad& c = pads_[p];
                    if (!c.handle || c.id != ev.caxis.which) continue;
                    const Sint16 v = ev.caxis.value;
                    for (const AxisBind& ab : axisBinds_) {
                        if (ab.port != p || ab.axis != ev.caxis.axis) continue;
                        if (ab.analog >= 0) {
                            analogField(c.state, ab.analog) = axisByte(v);
                        } else if (padAxisPressed(ab.dir, static_cast<int>(v))) {
                            c.state.buttons |= ab.bit;
                        } else {
                            c.state.buttons = static_cast<std::uint16_t>(c.state.buttons & ~ab.bit);
                        }
                        changed = true;
                    }
                }
                break;
            default:
                break;
        }
        if (changed) flush();
    }

    // Manda o estado das duas portas para o padman. Com o menu aberto, o jogo
    // recebe o controle neutro.
    void flush() {
        for (unsigned p = 0; p < 2; ++p) {
            PadInput s = menu_.open ? PadInput{} : pads_[p].state;
            if (!menu_.open) s.buttons |= keys_[p];
            // A porta 1 existe sempre (o teclado a usa); a 2 só com controle.
            s.connected = p == 0 || pads_[p].handle != nullptr;
            input_->setHost(p, s);
        }
    }

    std::string title_;
    bool requireDisplay_;
    int scale_;
    Input* input_;
    ConfigMenu menu_;  // cópia editável das opções: as ligações saem daqui
    std::vector<KeyBind> keyBinds_;
    std::vector<ButtonBind> buttonBinds_;
    std::vector<AxisBind> axisBinds_;
    std::array<std::uint16_t, 2> keys_{};
    std::array<Pad, 2> pads_{};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool started_ = false, ok_ = false, stop_ = false, hasFrame_ = false;
    std::string error_;
    gs::Frame frame_;
    std::atomic<bool> closed_{false};
};

}  // namespace

std::unique_ptr<Video> createSdlVideo(const RuntimeOptions& options, const std::string& title, Input* input,
                                      std::string& error) {
    auto v = std::make_unique<SdlVideo>(options, title, input);
    if (!v->start(error)) return nullptr;
    return v;
}

// Conferência dos nomes do arquivo de configuração (o SDL não precisa estar
// iniciado para isso).
bool knownSdlKey(const std::string& name) {
    return keycodeFromName(name) != SDLK_UNKNOWN;
}

bool knownSdlButton(const std::string& name) {
    return SDL_GameControllerGetButtonFromString(name.c_str()) != SDL_CONTROLLER_BUTTON_INVALID;
}

bool knownSdlAxis(const std::string& name) {
    return SDL_GameControllerGetAxisFromString(name.c_str()) != SDL_CONTROLLER_AXIS_INVALID;
}

}  // namespace anyps2::rt
