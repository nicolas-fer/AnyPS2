#pragma once

// Estruturas internas do kernel HLE compartilhadas entre kernel.cpp e
// kernel_threads.cpp.

#include <condition_variable>
#include <memory>

#include "anyps2/runtime/context.h"
#include "anyps2/runtime/host_thread.h"
#include "anyps2/runtime/kernel.h"

namespace anyps2::rt {

struct Kernel::Thread {
    std::int32_t id = 0;
    std::uint32_t status = THS_DORMANT;
    std::int32_t initPriority = 0;
    std::int32_t priority = 0;
    std::uint32_t entry = 0;
    std::uint32_t stack = 0;
    std::uint32_t stackSize = 0;
    std::uint32_t gp = 0;
    std::uint32_t attr = 0;
    std::uint32_t option = 0;
    std::uint32_t waitType = 0;
    std::int32_t waitId = 0;
    std::int32_t wakeupCount = 0;
    std::int64_t waitResult = 0;  // valor devolvido pela syscall que bloqueou
    bool suspended = false;
    bool deleted = false;
    bool unwind = false;  // terminada enquanto bloqueada: reinicia ao ganhar o bastão

    Context saved{};  // registradores enquanto não está rodando
    std::unique_ptr<HostThread> host;
    std::condition_variable cv;
    bool go = false;  // tem o bastão (protegido por batonMutex_)
};

// Sinal interno: a thread corrente foi terminada e reiniciada (StartThread
// depois de TerminateThread) — desenrola até o laço da thread do host.
struct ThreadRestartSignal {};

}  // namespace anyps2::rt
