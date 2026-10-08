// Janela SDL2. A janela, o renderer e o laço de eventos vivem numa thread
// própria: as threads do guest rodam em várias threads do host, e no Windows
// só a thread que criou a janela recebe as mensagens dela.
//
// A mesma thread lê o teclado e os controles (SDL_GameController) e
// atualiza o Input: teclado + 1º controle = porta 1, 2º controle = porta 2.
// Teclado: setas = direcional, Z = ✕, X = ○, A = □, S = △, Q/W = L1/R1,
// 1/2 = L2/R2, Enter = START, Backspace = SELECT.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "anyps2/runtime/input.h"
#include "anyps2/runtime/video.h"

namespace anyps2::rt {

namespace {

std::uint16_t keyButton(SDL_Keycode k) {
    switch (k) {
        case SDLK_UP: return padbtn::UP;
        case SDLK_DOWN: return padbtn::DOWN;
        case SDLK_LEFT: return padbtn::LEFT;
        case SDLK_RIGHT: return padbtn::RIGHT;
        case SDLK_z: return padbtn::CROSS;
        case SDLK_x: return padbtn::CIRCLE;
        case SDLK_a: return padbtn::SQUARE;
        case SDLK_s: return padbtn::TRIANGLE;
        case SDLK_q: return padbtn::L1;
        case SDLK_w: return padbtn::R1;
        case SDLK_1: return padbtn::L2;
        case SDLK_2: return padbtn::R2;
        case SDLK_RETURN: return padbtn::START;
        case SDLK_BACKSPACE: return padbtn::SELECT;
        default: return 0;
    }
}

std::uint16_t controllerButton(Uint8 b) {
    switch (b) {
        case SDL_CONTROLLER_BUTTON_A: return padbtn::CROSS;
        case SDL_CONTROLLER_BUTTON_B: return padbtn::CIRCLE;
        case SDL_CONTROLLER_BUTTON_X: return padbtn::SQUARE;
        case SDL_CONTROLLER_BUTTON_Y: return padbtn::TRIANGLE;
        case SDL_CONTROLLER_BUTTON_BACK: return padbtn::SELECT;
        case SDL_CONTROLLER_BUTTON_START: return padbtn::START;
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return padbtn::L3;
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return padbtn::R3;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return padbtn::L1;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return padbtn::R1;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return padbtn::UP;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return padbtn::DOWN;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return padbtn::LEFT;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return padbtn::RIGHT;
        default: return 0;
    }
}

// Eixo do SDL (-32768..32767) -> analógico do DualShock (0..255, 0x80 = centro).
std::uint8_t axisByte(Sint16 v) {
    return static_cast<std::uint8_t>((static_cast<int>(v) + 32768) >> 8);
}

class SdlVideo final : public Video {
public:
    SdlVideo(std::string title, bool requireDisplay, Input* input)
        : title_(std::move(title)), requireDisplay_(requireDisplay), input_(input) {}

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
        SDL_Window* window = SDL_CreateWindow(("AnyPS2 — " + title_).c_str(), SDL_WINDOWPOS_CENTERED,
                                              SDL_WINDOWPOS_CENTERED, 640, 480, SDL_WINDOW_RESIZABLE);
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
                if (const std::uint16_t b = keyButton(ev.key.keysym.sym)) {
                    if (ev.type == SDL_KEYDOWN) keys_ |= b;
                    else keys_ = static_cast<std::uint16_t>(keys_ & ~b);
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
                for (auto& c : pads_) {
                    if (c.handle && c.id == ev.cbutton.which) {
                        const std::uint16_t b = controllerButton(ev.cbutton.button);
                        if (ev.type == SDL_CONTROLLERBUTTONDOWN) c.state.buttons |= b;
                        else c.state.buttons = static_cast<std::uint16_t>(c.state.buttons & ~b);
                        changed = true;
                    }
                }
                break;
            case SDL_CONTROLLERAXISMOTION:
                for (auto& c : pads_) {
                    if (!c.handle || c.id != ev.caxis.which) continue;
                    const Sint16 v = ev.caxis.value;
                    switch (ev.caxis.axis) {
                        case SDL_CONTROLLER_AXIS_LEFTX: c.state.lx = axisByte(v); break;
                        case SDL_CONTROLLER_AXIS_LEFTY: c.state.ly = axisByte(v); break;
                        case SDL_CONTROLLER_AXIS_RIGHTX: c.state.rx = axisByte(v); break;
                        case SDL_CONTROLLER_AXIS_RIGHTY: c.state.ry = axisByte(v); break;
                        case SDL_CONTROLLER_AXIS_TRIGGERLEFT:
                        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: {
                            const std::uint16_t b =
                                ev.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ? padbtn::L2 : padbtn::R2;
                            if (v > 8000) c.state.buttons |= b;
                            else c.state.buttons = static_cast<std::uint16_t>(c.state.buttons & ~b);
                            break;
                        }
                        default: break;
                    }
                    changed = true;
                }
                break;
            default:
                break;
        }
        if (!changed) return;
        PadInput p0 = pads_[0].state;
        p0.buttons |= keys_;
        input_->setHost(0, p0);
        PadInput p1 = pads_[1].state;
        p1.connected = pads_[1].handle != nullptr;
        input_->setHost(1, p1);
    }

    struct Pad {
        SDL_GameController* handle = nullptr;
        SDL_JoystickID id = -1;
        PadInput state;
    };

    std::string title_;
    bool requireDisplay_;
    Input* input_;
    std::uint16_t keys_ = 0;
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

std::unique_ptr<Video> createSdlVideo(const std::string& title, bool requireDisplay, Input* input,
                                      std::string& error) {
    auto v = std::make_unique<SdlVideo>(title, requireDisplay, input);
    if (!v->start(error)) return nullptr;
    return v;
}

}  // namespace anyps2::rt
