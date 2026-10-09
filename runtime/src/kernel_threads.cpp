// Threads do EE e escalonador (parte do Kernel HLE).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

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
    switchTo(waitForReady(pc), pc);
}

Kernel::Thread* Kernel::waitForReady(std::uint32_t pc) {
    Timing& timing = rt_.timing();
    const std::uint64_t start = timing.now();
    for (;;) {
        if (Thread* next = bestReady()) return next;
        // No relógio virtual, 60 s do EE sem nenhuma thread pronta é deadlock
        // (no modo real o programa pode estar esperando entrada para sempre).
        if (timing.mode() == Timing::Mode::Virtual && timing.now() - start > 60 * Timing::kEeHz) deadlock(pc);
        if (!timing.advanceToNextEvent(pc)) deadlock(pc);
        serviceInterrupts(pc, false, true);
    }
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
    // Recebemos o bastão de volta. O orçamento de safepoint é global, não da thread.
    const std::int32_t budget = ctx.budget, reload = ctx.budgetReload;
    ctx = self->saved;
    ctx.budget = budget;
    ctx.budgetReload = reload;
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
        } catch (const ExecRequest& e) {
            requestExec(e);
            return;
        } catch (...) {
            requestShutdown(std::current_exception(), 0);
            return;
        }
        // A thread terminou (retorno da função de entrada ou ExitThread).
        t->status = THS_DORMANT;
        try {
            switchTo(waitForReady(ctx.pc), ctx.pc);
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

void Kernel::requestExec(const ExecRequest& request) {
    std::lock_guard lk(batonMutex_);
    if (shutdown_) return;
    shutdown_ = true;
    pendingExec_ = request;
    for (auto& [id, t] : threads_) t->cv.notify_all();
}

// ExecPS2(entrada, gp, argc, argv): executa um programa que já está na
// memória (jogos descomprimem o executável principal e saltam para ele).
// Se a entrada é código recompilado (o programa foi incluído no projeto com
// "anyps2 recomp --extra"), o programa atual termina e o novo começa. Senão,
// erro claro — e, com ANYPS2_EXEC_DUMP, a RAM é gravada para gerar o ELF.
void Kernel::execPs2(Context* c, std::uint32_t pc) {
    Memory& m = rt_.memory();
    ExecRequest req;
    req.entry = c->r[4].uw[0];
    req.gp = c->r[5].uw[0];
    const auto argc = static_cast<std::int32_t>(c->r[6].uw[0]);
    const std::uint32_t argv = c->r[7].uw[0];
    for (std::int32_t i = 0; i < argc && i < 16; ++i) {
        req.args.push_back(m.readCString(m.read<std::uint32_t>(argv + 4u * static_cast<std::uint32_t>(i), pc), 256, pc));
    }
    if (!rt_.lookup(req.entry)) {
        std::string where;
        if (const char* dir = std::getenv("ANYPS2_EXEC_DUMP"); dir && *dir) where = dumpExecImage(dir, req, pc);
        throw Unimplemented(
            "ExecPS2(" + anyps2::hex(req.entry) + ", gp " + anyps2::hex(req.gp) + ", " + std::to_string(argc) +
                " argumento(s)): o programa executa código que ele mesmo carregou na memória e que não foi "
                "recompilado. " +
                (where.empty() ? std::string("Grave a memória com ANYPS2_EXEC_DUMP=<pasta> e inclua o código no "
                                             "projeto (anyps2 ram2elf + anyps2 recomp --extra)")
                               : "Memória gravada em " + where + "; gere o ELF com \"anyps2 ram2elf " + where +
                                     " --entry " + anyps2::hex(req.entry) +
                                     " --text INICIO FIM [--data INICIO FIM] -o programa.elf\" e recompile com --extra programa.elf"),
            pc);
    }
    throw req;
}

// RAM inteira (32 MB) + um .txt com entrada/gp/argumentos.
std::string Kernel::dumpExecImage(const std::string& dir, const ExecRequest& req, std::uint32_t pc) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    char base[32];
    std::snprintf(base, sizeof(base), "exec_%08x", req.entry);
    const fs::path ram = fs::path(dir) / (std::string(base) + ".ram");
    std::ofstream out(ram, std::ios::binary);
    out.write(reinterpret_cast<const char*>(rt_.memory().ram()), Memory::kRamSize);
    if (!out) throw GuestError("ANYPS2_EXEC_DUMP: não foi possível gravar " + ram.string(), pc);
    std::ofstream info(fs::path(dir) / (std::string(base) + ".txt"));
    info << "entrada " << anyps2::hex(req.entry) << "\ngp " << anyps2::hex(req.gp) << "\n";
    for (const auto& a : req.args) info << "arg " << a << "\n";
    return ram.string();
}

// Estado do kernel do EE que um ExecPS2 descarta (as threads do host já
// terminaram).
void Kernel::resetForExec() {
    threads_.clear();
    nextThreadId_ = 1;
    current_ = nullptr;
    for (auto& q : ready_) q.clear();
    reschedulePending_ = false;
    shutdown_ = false;
    exitCode_ = 0;
    semas_.clear();
    nextSema_ = 1;
    intcHandlers_.clear();
    dmacHandlers_.clear();
    nextHandlerId_ = 1;
    intcMask_ = 0;
    pendingAlarms_.clear();
    rt_.timing().clearAlarms();
    interruptDepth_ = 0;
    heapStart_ = heapEnd_ = stackBottom_ = mainStack_ = mainStackSize_ = 0;
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
    for (;;) {
        runProgram(entry);
        if (fatal_) std::rethrow_exception(fatal_);
        if (!pendingExec_) return exitCode_;
        // ExecPS2: o programa novo começa do zero (registradores, threads...).
        const ExecRequest req = *pendingExec_;
        pendingExec_.reset();
        resetForExec();
        bootArgs_ = req.args;
        Context& ctx = rt_.context();
        for (auto& r : ctx.r) r.ud[0] = r.ud[1] = 0;
        ctx.r[28].sd[0] = static_cast<std::int32_t>(req.gp);
        if (rt_.options().traceSyscalls) {
            std::fprintf(stderr, "[exec] ExecPS2 -> %s\n", rt_.describe(req.entry).c_str());
        }
        entry = req.entry;
    }
}

void Kernel::runProgram(std::uint32_t entry) {
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
    HostThread* mainHost = nullptr;
    {
        // A thread do guest só começa depois de pegar o bastão (este mutex):
        // assim m->host já está atribuído quando outra thread do guest o
        // consultar em switchTo (sem isso, uma troca rápida via
        // ChangeThreadPriority/SleepThread criava uma segunda thread do host
        // para a mesma thread do guest).
        std::lock_guard lk(batonMutex_);
        m->host = std::make_unique<HostThread>([this, m] { hostThreadMain(m); }, kGuestThreadStack);
        mainHost = m->host.get();
    }
    // Espera o fim do programa (Exit ou erro em qualquer thread).
    mainHost->join();
    // A thread principal terminou; se outra pediu o fim, o estado já está pronto.
    {
        std::lock_guard lk(batonMutex_);
        shutdown_ = true;
        for (auto& [id, t] : threads_) t->cv.notify_all();
    }
    for (auto& [id, t] : threads_) {
        if (t->host) t->host->join();
    }
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

std::string Kernel::threadReport() const {
    std::string s = describeThreads();
    for (const auto& [id, t] : threads_) {
        if (t->deleted || t.get() == current_ || t->status == THS_DORMANT) continue;
        s += "\n  thread " + std::to_string(id) + " parada em " + rt_.describe(t->saved.pc) + ", chamada de " +
             rt_.describe(t->saved.r[31].uw[0]);
    }
    return s;
}

void Kernel::deadlock(std::uint32_t pc) {
    throw GuestError("deadlock: todas as threads do EE estão bloqueadas e nenhum evento (timer, "
                     "alarme, VBlank) as acordou:" + describeThreads(),
                     pc);
}

}  // namespace anyps2::rt
