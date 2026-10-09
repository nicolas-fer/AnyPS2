// Janela SDL2. A janela, o renderer e o laço de eventos vivem numa thread
// própria: as threads do guest rodam em várias threads do host, e no Windows
// só a thread que criou a janela recebe as mensagens dela.
//
// A mesma thread lê o teclado e os controles (SDL_GameController) e
// atualiza o Input: cada porta (1 e 2) tem as ligações de RuntimeOptions::pad
// (arquivo de configuração; padrões: setas, Z ✕, X ○, A □, S △, Q/W = L1/R1,
// 1/2 = L2/R2, Enter = START, Backspace = SELECT no teclado da porta 1, e o
// controle SDL: 1º controle = porta 1, 2º = porta 2).

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "anyps2/runtime/input.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/video.h"

namespace anyps2::rt {

namespace {

// Limiar para eixo usado como botão digital (L2/R2 ou qualquer ligação digital).
constexpr int kAxisButtonThreshold = 8000;

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

class SdlVideo final : public Video {
public:
    SdlVideo(const RuntimeOptions& options, std::string title, Input* input)
        : title_(std::move(title)),
          requireDisplay_(options.video.empty()),
          scale_(options.videoScale < 1 ? 1 : options.videoScale),
          input_(input) {
        bind(options.pad);
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
    // aciona o botão digital bit acima do limiar.
    struct AxisBind {
        SDL_GameControllerAxis axis;
        unsigned port;
        int analog;
        std::uint16_t bit;
    };

    // Resolve os nomes da configuração para códigos do SDL. Nome desconhecido
    // vira aviso e a ligação é ignorada (a janela continua funcionando).
    void bind(const PadBindings& pad) {
        for (unsigned port = 0; port < 2; ++port) {
            for (unsigned i = 0; i < 16; ++i) {
                const std::uint16_t bit = static_cast<std::uint16_t>(1u << i);
                const std::string& key = pad.key[port][i];
                if (!key.empty()) {
                    if (const SDL_Keycode k = keycodeFromName(key); k != SDLK_UNKNOWN) {
                        keyBinds_.push_back({k, port, bit});
                    } else {
                        warn("tecla", key, port, i);
                    }
                }
                const std::string& button = pad.button[port][i];
                if (!button.empty()) {
                    if (const SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(button.c_str());
                        b != SDL_CONTROLLER_BUTTON_INVALID) {
                        buttonBinds_.push_back({b, port, bit});
                    } else if (const SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(button.c_str());
                               a != SDL_CONTROLLER_AXIS_INVALID) {
                        axisBinds_.push_back({a, port, -1, bit});
                    } else {
                        warn("botão do controle", button, port, i);
                    }
                }
            }
            for (int a = 0; a < 4; ++a) {
                const std::string& axis = pad.analog[port][a];
                if (axis.empty()) continue;
                if (const SDL_GameControllerAxis s = SDL_GameControllerGetAxisFromString(axis.c_str());
                    s != SDL_CONTROLLER_AXIS_INVALID) {
                    axisBinds_.push_back({s, port, a, 0});
                } else {
                    std::cerr << "anyps2: aviso: eixo do controle desconhecido '" << axis << "' (porta "
                              << port + 1 << ", " << kPadAnalogNames[a] << "), ignorado\n";
                }
            }
        }
    }

    void warn(const char* what, const std::string& name, unsigned port, unsigned button) {
        std::cerr << "anyps2: aviso: " << what << " desconhecido '" << name << "' (porta " << port + 1 << ", "
                  << kPadButtonNames[button] << "), ignorado\n";
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
                handleInput(ev);
            }
            if (!draw) continue;
            if (!texture || texW != local.width || texH != local.height) {
                if (texture) SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
                                            static_cast<int>(local.width), static_cast<int>(local.height));
                texW = local.width;
                texH = local.height;
            }
            if (!texture) continue;
            SDL_UpdateTexture(texture, nullptr, local.pixels.data(), static_cast<int>(local.width * 4));
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
            SDL_RenderPresent(renderer);
        }
        if (texture) SDL_DestroyTexture(texture);
        for (auto& c : pads_) {
            if (c.handle) SDL_GameControllerClose(c.handle);
        }
        if (controllers) SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    void handleInput(const SDL_Event& ev) {
        if (!input_) return;
        bool changed = false;
        switch (ev.type) {
            case SDL_KEYDOWN:
            case SDL_KEYUP:
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
                        } else if (std::abs(static_cast<int>(v)) > kAxisButtonThreshold) {
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
        if (!changed) return;
        for (unsigned p = 0; p < 2; ++p) {
            PadInput s = pads_[p].state;
            s.buttons |= keys_[p];
            // A porta 1 existe sempre (o teclado a usa); a 2 só com controle.
            s.connected = p == 0 || pads_[p].handle != nullptr;
            input_->setHost(p, s);
        }
    }

    struct Pad {
        SDL_GameController* handle = nullptr;
        SDL_JoystickID id = -1;
        PadInput state;
    };

    std::string title_;
    bool requireDisplay_;
    int scale_;
    Input* input_;
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

}  // namespace anyps2::rt
