// Janela SDL2. A janela, o renderer e o laço de eventos vivem numa thread
// própria: as threads do guest rodam em várias threads do host, e no Windows
// só a thread que criou a janela recebe as mensagens dela.

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "anyps2/runtime/video.h"

namespace anyps2::rt {

namespace {

class SdlVideo final : public Video {
public:
    SdlVideo(std::string title, bool requireDisplay) : title_(std::move(title)), requireDisplay_(requireDisplay) {}

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
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }

    std::string title_;
    bool requireDisplay_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool started_ = false, ok_ = false, stop_ = false, hasFrame_ = false;
    std::string error_;
    gs::Frame frame_;
    std::atomic<bool> closed_{false};
};

}  // namespace

std::unique_ptr<Video> createSdlVideo(const std::string& title, bool requireDisplay, std::string& error) {
    auto v = std::make_unique<SdlVideo>(title, requireDisplay);
    if (!v->start(error)) return nullptr;
    return v;
}

}  // namespace anyps2::rt
