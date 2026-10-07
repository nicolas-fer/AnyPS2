// Threads do EE e escalonador (parte do Kernel HLE).

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/runtime.h"
#include "kernel_internal.h"

namespace anyps2::rt {

std::int32_t Kernel::currentThreadId() const {
    return current_ ? current_->id : 0;
}

Kernel::Thread* Kernel::thread(std::int32_t id, std::uint32_t, bool allowSelf) {
    if (id == 0 && allowSelf) return current_;
    auto it = threads_.find(id);
    if (it == threads_.end() || it->second->deleted) return nullptr;
    return it->second.get();
}

std::int32_t Kernel::createThread(std::uint32_t param, std::uint32_t pc) {
    Memory& m = rt_.memory();
    auto t = std::make_unique<Thread>();
    t->id = nextThreadId_++;
    t->entry = m.read<std::uint32_t>(param + 0x04, pc);
    t->stack = m.read<std::uint32_t>(param + 0x08, pc);
    t->stackSize = m.read<std::uint32_t>(param + 0x0C, pc);
    t->gp = m.read<std::uint32_t>(param + 0x10, pc);
    t->initPriority = static_cast<std::int32_t>(m.read<std::uint32_t>(param + 0x14, pc));
    t->attr = m.read<std::uint32_t>(param + 0x1C, pc);
    t->option = m.read<std::uint32_t>(param + 0x20, pc);
    if (t->initPriority < 0 || t->initPriority > 127 || t->stackSize < 0x300 || t->entry == 0) return -1;
    t->priority = t->initPriority;
    t->status = THS_DORMANT;
    const std::int32_t id = t->id;
    threads_[id] = std::move(t);
    return id;
}

std::int32_t Kernel::startThread(std::int32_t id, std::uint32_t arg, std::uint32_t pc) {
    Thread* t = thread(id, pc, false);
    if (!t || t->status != THS_DORMANT) return -1;
    // Contexto inicial: registradores zerados, como o kernel real.
    Context& cur = rt_.context();
    Context init{};
    std::memset(static_cast<void*>(&init), 0, sizeof(Context));
    init.mem = cur.mem;
    init.rt = cur.rt;
    init.fcr31 = 0x01000001u;
    init.vf[0].uw[3] = 0x3F800000u;
    std::memcpy(init.cop0, cur.cop0, sizeof(init.cop0));
    init.r[4].sd[0] = static_cast<std::int32_t>(arg);
    init.r[28].sd[0] = static_cast<std::int32_t>(t->gp);
    init.r[29].sd[0] = static_cast<std::int32_t>(t->stack + t->stackSize - 0x2A0);
    init.r[31].sd[0] = static_cast<std::int32_t>(kThreadExitAddress);
    init.pc = t->entry;
    t->saved = init;
    t->priority = t->initPriority;
    t->wakeupCount = 0;
    t->suspended = false;
    makeReady(t);
    return id;
}

void Kernel::makeReady(Thread* t, bool front) {
    t->status = THS_READY;
    auto& q = ready_[static_cast<std::size_t>(t->priority)];
    if (front) q.push_front(t);
    else q.push_back(t);
    reschedulePending_ = true;
}

void Kernel::removeReady(Thread* t) {
    for (auto& q : ready_) std::erase(q, t);
}

Kernel::Thread* Kernel::bestReady() const {
    for (const auto& q : ready_) {
        if (!q.empty()) return q.front();
    }
    return nullptr;
}

void Kernel::reschedule(std::uint32_t pc) {
    reschedulePending_ = false;
    Thread* best = bestReady();
    if (!best) return;
    if (current_->status == THS_RUN && best->priority >= current_->priority) return;
    if (current_->status == THS_RUN) {
        // Preemptada: volta para a frente da fila da sua prioridade.
        current_->status = THS_READY;
        ready_[static_cast<std::size_t>(current_->priority)].push_front(current_);
    }
    switchTo(best, pc);
}

void Kernel::blockCurrent(std::uint32_t waitType, std::int32_t waitId, std::uint32_t pc) {
    if (interruptDepth_ > 0) {
        throw GuestError("syscall bloqueante chamada de dentro de um handler de interrupção", pc);
    }
    current_->status = THS_WAIT | (current_->suspended ? THS_SUSPEND : 0);
    current_->waitType = waitType;
    current_->waitId = waitId;
    Thread* next = bestReady();
    if (!next) deadlock(pc);
    switchTo(next, pc);
}

void Kernel::switchTo(Thread* next, std::uint32_t pc) {
    (void)pc;
    Thread* self = current_;
    if (next == self) {
        removeReady(self);
        self->status = THS_RUN;
        return;
    }
    Context& ctx = rt_.context();
    self->saved = ctx;
    removeReady(next);
    next->status = THS_RUN;
    current_ = next;
    if (rt_.options().traceSyscalls) {
        std::fprintf(stderr, "[sched] thread %d -> thread %d\n", self->id, next->id);
    }
    {
        std::unique_lock lk(batonMutex_);
        if (!next->host) {
            Thread* t = next;
            next->host = std::make_unique<HostThread>([this, t] { hostThreadMain(t); }, kGuestThreadStack);
        }
        next->go = true;
        next->cv.notify_one();
        self->go = false;
        self->cv.wait(lk, [&] { return self->go || shutdown_; });
        if (shutdown_) throw ShutdownSignal{};
    }
    // Recebemos o bastão de volta.
    ctx = self->saved;
    if (self->unwind) {
        self->unwind = false;
        throw ThreadRestartSignal{};
    }
}

void Kernel::waitForBaton(Thread* self) {
    std::unique_lock lk(batonMutex_);
    self->cv.wait(lk, [&] { return self->go || shutdown_; });
    if (shutdown_) throw ShutdownSignal{};
}

void Kernel::hostThreadMain(Thread* t) {
    try {
        waitForBaton(t);
    } catch (const ShutdownSignal&) {
        return;
    }
    for (;;) {
        Context& ctx = rt_.context();
        try {
            ctx = t->saved;
            rt_.runUntil(kThreadExitAddress);
        } catch (const ThreadExitSignal&) {
        } catch (const ThreadRestartSignal&) {
            continue;  // StartThread após TerminateThread: contexto novo já carregado
        } catch (const ShutdownSignal&) {
            return;
        } catch (const ProgramExit& e) {
            requestShutdown(nullptr, e.code);
            return;
        } catch (...) {
            requestShutdown(std::current_exception(), 0);
            return;
        }
        // A thread terminou (retorno da função de entrada ou ExitThread).
        t->status = THS_DORMANT;
        try {
            Thread* next = bestReady();
            if (!next) deadlock(ctx.pc);
            switchTo(next, ctx.pc);
            // Fomos reiniciados por StartThread: o laço carrega o contexto novo.
        } catch (const ThreadRestartSignal&) {
            continue;
        } catch (const ShutdownSignal&) {
            return;
        } catch (...) {
            requestShutdown(std::current_exception(), 0);
            return;
        }
    }
}

void Kernel::requestShutdown(std::exception_ptr error, int code) {
    std::lock_guard lk(batonMutex_);
    if (shutdown_) return;
    shutdown_ = true;
    fatal_ = error;
    exitCode_ = code;
    for (auto& [id, t] : threads_) t->cv.notify_all();
}

int Kernel::runMain(std::uint32_t entry) {
    auto main = std::make_unique<Thread>();
    main->id = nextThreadId_++;
    main->status = THS_RUN;
    main->priority = main->initPriority = 0;
    main->entry = entry;
    Context& ctx = rt_.context();
    ctx.pc = entry;
    main->saved = ctx;
    main->go = true;
    Thread* m = main.get();
    current_ = m;
    threads_[m->id] = std::move(main);
    m->host = std::make_unique<HostThread>([this, m] { hostThreadMain(m); }, kGuestThreadStack);
    // Espera o fim do programa (Exit ou erro em qualquer thread).
    m->host->join();
    // A thread principal terminou; se outra pediu o fim, o estado já está pronto.
    {
        std::lock_guard lk(batonMutex_);
        shutdown_ = true;
        for (auto& [id, t] : threads_) t->cv.notify_all();
    }
    for (auto& [id, t] : threads_) {
        if (t->host) t->host->join();
    }
    if (fatal_) std::rethrow_exception(fatal_);
    return exitCode_;
}

std::string Kernel::describeThreads() const {
    std::string s;
    for (const auto& [id, t] : threads_) {
        if (t->deleted) continue;
        char buf[160];
        const char* st = t->status == THS_RUN ? "RUN" : t->status == THS_READY ? "READY"
                       : (t->status & THS_WAIT) ? (t->waitType == TSW_SEMA ? "WAIT(sema)" : "WAIT(sleep)")
                       : t->status == THS_SUSPEND ? "SUSPEND" : "DORMANT";
        std::snprintf(buf, sizeof(buf), "\n  thread %d: %s prioridade %d entrada %s", id, st, t->priority,
                      rt_.describe(t->entry).c_str());
        s += buf;
        if ((t->status & THS_WAIT) && t->waitType == TSW_SEMA) s += " semáforo " + std::to_string(t->waitId);
    }
    return s;
}

void Kernel::deadlock(std::uint32_t pc) {
    throw GuestError("deadlock: todas as threads do EE estão bloqueadas e nenhuma interrupção "
                     "pendente pode acordá-las (timers/alarmes chegam na Fase 3):" + describeThreads(),
                     pc);
}

}  // namespace anyps2::rt
