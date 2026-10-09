#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "anyps2/runtime/context.h"
#include "anyps2/runtime/timing.h"

namespace anyps2::rt {

class Runtime;
class HostThread;

// Nome da syscall do kernel do EE (números do ps2sdk/syscallnr.h). Números
// negativos são as variantes "i" (chamadas de dentro de interrupções).
std::string syscallName(std::int32_t number);

// Kernel do EE em HLE.
//
// Threads: cada thread do EE roda numa thread do host com pilha própria, mas
// só uma executa por vez (o "bastão"), escolhida pelo escalonador com as
// regras do kernel real: prioridade estrita (0 = mais alta), FIFO por
// prioridade, troca apenas em syscalls/interrupções (sem fatia de tempo).
// Todo o estado do guest fica no único Context compartilhado, que é salvo e
// restaurado na troca.
class Kernel {
public:
    // Pilha usada quando o runtime chama código do guest (handlers): fica na
    // área de kernel (0x0000_0000–0x0007_FFFF), que programas não usam.
    static constexpr std::uint32_t kHandlerStackTop = 0x00080000u - 0x10;
    // Endereço de retorno das funções de entrada de thread: "jr ra" para cá
    // encerra a thread (ExitThread implícito).
    static constexpr std::uint32_t kThreadExitAddress = 0xFFFFFFE0u;

    // Tabela de syscalls visível ao guest, na RAM do kernel, como no console:
    // SetSyscall/GetEntryAddress a alteram e leem, e a libkernel da Sony acha
    // a base dela procurando na memória do kernel um handler que acabou de
    // instalar (GetSyscallHandler). Cada entrada começa com um sentinela que
    // representa o handler HLE daquela syscall; na hora da syscall vale o que
    // estiver na tabela (ver runSyscall).
    static constexpr std::uint32_t kSyscallTable = 0x80000800u;
    static constexpr std::uint32_t kSyscallCount = 512;
    static constexpr std::uint32_t kSyscallStubBase = 0x80001000u;  // entrada n: base + 8n
    static constexpr std::uint32_t syscallStubAddress(std::uint32_t index) {
        return kSyscallStubBase + index * 8u;
    }
    // Número da syscall de um sentinela (std::nullopt se não for um).
    static std::optional<std::uint32_t> syscallStub(std::uint32_t address);

    explicit Kernel(Runtime& rt);
    ~Kernel();

    // Preenche a tabela de syscalls na RAM do guest (antes do programa rodar).
    void initSyscallTable();

    void setBootArguments(const std::vector<std::string>& args) { bootArgs_ = args; }
    void syscall(Context* c, std::uint32_t pc);
    // Salto/chamada direta a um sentinela da tabela (handler obtido com
    // GetEntryAddress ou lido da tabela): executa a syscall e volta para ra.
    void callSyscallStub(Context* c, std::uint32_t index, std::uint32_t pc);

    // Executa o programa a partir do ponto de entrada na thread principal do
    // EE e espera até Exit (ou erro em qualquer thread). Retorna o código de saída.
    int runMain(std::uint32_t entry);

    // Interrupções (chamadas pelo HLE de hardware/IOP).
    void raiseDmacInterrupt(unsigned channel, std::uint32_t pc);
    bool canDeliverDmac(unsigned channel) const;
    // Marca a causa no INTC_STAT; a entrega acontece em serviceInterrupts.
    void raiseIntc(unsigned cause) { intcStat_ |= 1u << cause; }
    void queueAlarm(const Timing::Alarm& alarm);
    // Entrega interrupções INTC pendentes e alarmes vencidos (se permitido) e
    // troca de thread se alguma de prioridade maior ficou pronta.
    // idle = chamado com todas as threads bloqueadas (ignora Status.EIE, como
    // a thread ociosa do kernel real).
    void serviceInterrupts(std::uint32_t pc, bool allowReschedule = true, bool idle = false);
    // Custo (ciclos) contabilizado por syscall no relógio virtual.
    static constexpr std::int64_t kSyscallCycles = 200;
    static constexpr unsigned kIntcDmac = 1;

    // Registradores INTC_STAT/INTC_MASK (acessados pelo hardware).
    std::uint32_t intcStat() const { return intcStat_; }
    void clearIntcStat(std::uint32_t bits) { intcStat_ &= ~bits; }
    std::uint32_t intcMask() const { return intcMask_; }
    void toggleIntcMask(std::uint32_t bits) { intcMask_ ^= bits; }
    bool inInterrupt() const { return interruptDepth_ > 0; }

    struct Semaphore {
        std::int32_t count = 0;
        std::int32_t maxCount = 0;
        std::int32_t initCount = 0;
        std::uint32_t attr = 0;
        std::uint32_t option = 0;
        std::deque<std::int32_t> waiters;
    };

    // Estados (ps2sdk kernel.h)
    static constexpr std::uint32_t THS_RUN = 0x01, THS_READY = 0x02, THS_WAIT = 0x04,
                                   THS_SUSPEND = 0x08, THS_DORMANT = 0x10;
    static constexpr std::uint32_t TSW_SLEEP = 1, TSW_SEMA = 2;

    std::int32_t currentThreadId() const;
    // Estado de cada thread; as bloqueadas com o PC e o ra salvos (de onde
    // chamaram a espera).
    std::string threadReport() const;

private:
    struct Thread;
    struct Handler {
        std::int32_t id;
        unsigned cause;
        std::uint32_t function;
        std::uint32_t arg;
        std::uint32_t gp;
        bool enabled = true;
    };
    struct ShutdownSignal {};  // desenrola threads do host no fim do programa
    // ExecPS2: o programa atual termina e outro, já na memória, começa (com
    // threads, semáforos, handlers e alarmes novos; memória, IOP e tabela de
    // syscalls ficam).
    struct ExecRequest {
        std::uint32_t entry = 0, gp = 0;
        std::vector<std::string> args;
    };
    struct ThreadExitSignal {};  // desenrola a thread corrente até o laço dela

    // Semântica HLE da syscall; std::nullopt se o HLE não a implementa.
    std::optional<std::uint64_t> dispatch(Context* c, std::int32_t number, std::uint32_t pc);
    void runSyscall(Context* c, std::int32_t number, std::uint32_t pc);
    std::uint32_t syscallEntry(std::uint32_t index, std::uint32_t pc);
    [[noreturn]] void notImplemented(std::int32_t number, std::uint32_t pc, const char* phase);

    std::int32_t setupThread(Context* c, std::uint32_t pc);
    std::int32_t addHandler(std::vector<Handler>& list, unsigned cause, std::uint32_t fn,
                            std::int32_t next, std::uint32_t arg, std::uint32_t gp);
    void runHandlers(std::vector<Handler>& list, std::uint32_t enabledMask, unsigned cause,
                     std::uint32_t pc, bool ignoreEie = false);
    // Chama um handler do programa com Status.IE desligado (contexto de interrupção).
    std::uint64_t invokeHandler(std::uint32_t function, const std::vector<std::uint32_t>& args, std::uint32_t pc);

    // ---- Threads ---------------------------------------------------------
    Thread* thread(std::int32_t id, std::uint32_t pc, bool allowSelf = true);
    std::int32_t createThread(std::uint32_t param, std::uint32_t pc);
    std::int32_t startThread(std::int32_t id, std::uint32_t arg, std::uint32_t pc);
    void makeReady(Thread* t, bool front = false);
    void removeReady(Thread* t);
    Thread* bestReady() const;
    // Ponto de escalonamento: troca de thread se outra de prioridade maior
    // estiver pronta (ou se a corrente bloqueou).
    void reschedule(std::uint32_t pc);
    void blockCurrent(std::uint32_t waitType, std::int32_t waitId, std::uint32_t pc);
    // Sem thread pronta: avança o relógio até algum evento acordar uma.
    Thread* waitForReady(std::uint32_t pc);
    void switchTo(Thread* next, std::uint32_t pc);
    void waitForBaton(Thread* self);
    void hostThreadMain(Thread* t);
    // Executa um programa até Exit, erro ou ExecPS2 (runMain repete no ExecPS2).
    void runProgram(std::uint32_t entry);
    void requestShutdown(std::exception_ptr error, int code);
    void requestExec(const ExecRequest& request);
    [[noreturn]] void execPs2(Context* c, std::uint32_t pc);
    std::string dumpExecImage(const std::string& dir, const ExecRequest& request, std::uint32_t pc);
    void resetForExec();
    [[noreturn]] void deadlock(std::uint32_t pc);
    std::string describeThreads() const;

    Runtime& rt_;
    std::vector<std::string> bootArgs_;
    std::uint32_t heapStart_ = 0;
    std::uint32_t heapEnd_ = 0;
    std::uint32_t stackBottom_ = 0;
    std::uint32_t mainStack_ = 0;
    std::uint32_t mainStackSize_ = 0;

    std::map<std::int32_t, std::unique_ptr<Thread>> threads_;
    std::int32_t nextThreadId_ = 1;
    Thread* current_ = nullptr;
    std::vector<std::deque<Thread*>> ready_;  // por prioridade (0..127)
    bool reschedulePending_ = false;

    std::mutex batonMutex_;
    bool shutdown_ = false;
    int exitCode_ = 0;
    std::exception_ptr fatal_;
    std::optional<ExecRequest> pendingExec_;

    std::map<std::int32_t, Semaphore> semas_;
    std::int32_t nextSema_ = 1;
    std::vector<Handler> intcHandlers_;
    std::vector<Handler> dmacHandlers_;
    std::int32_t nextHandlerId_ = 1;
    std::uint32_t intcMask_ = 0;
    std::uint32_t intcStat_ = 0;
    struct PendingAlarm {
        std::int32_t id;
        std::uint16_t lines;
        std::uint32_t handler, arg, gp;
    };
    std::vector<PendingAlarm> pendingAlarms_;
    std::uint32_t osdConfig_ = 0;
    std::uint32_t osdConfig2_[2] = {0, 0};
    unsigned interruptDepth_ = 0;
};

}  // namespace anyps2::rt
