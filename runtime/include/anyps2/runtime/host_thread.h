#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace anyps2::rt {

// Thread do host com tamanho de pilha configurável (std::thread não permite
// escolher; no Windows o padrão de 1 MB é pouco para código recompilado
// recursivo).
class HostThread {
public:
    HostThread(std::function<void()> body, std::size_t stackBytes);
    ~HostThread();
    HostThread(const HostThread&) = delete;
    HostThread& operator=(const HostThread&) = delete;
    void join();

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

inline constexpr std::size_t kGuestThreadStack = 64u * 1024 * 1024;

}  // namespace anyps2::rt
