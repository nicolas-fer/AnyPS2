#include "anyps2/runtime/kernel.h"

#include <cstdio>
#include <cstring>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/hardware.h"
#include "anyps2/runtime/iop.h"
#include "anyps2/runtime/ops.h"
#include "anyps2/runtime/runtime.h"
#include "kernel_internal.h"

namespace anyps2::rt {

namespace {

struct SyscallInfo {
    std::int32_t number;
    const char* name;
};

// ps2sdk/ee/include/syscallnr.h
constexpr SyscallInfo kSyscalls[] = {
    {0x01, "ResetEE"}, {0x02, "SetGsCrt"}, {0x04, "Exit"}, {0x05, "_ExceptionEpilogue"},
    {0x06, "LoadExecPS2"}, {0x07, "ExecPS2"}, {0x0A, "AddSbusIntcHandler"},
    {0x0B, "RemoveSbusIntcHandler"}, {0x0C, "Interrupt2Iop"}, {0x0D, "SetVTLBRefillHandler"},
    {0x0E, "SetVCommonHandler"}, {0x0F, "SetVInterruptHandler"}, {0x10, "AddIntcHandler"},
    {0x11, "RemoveIntcHandler"}, {0x12, "AddDmacHandler"}, {0x13, "RemoveDmacHandler"},
    {0x14, "_EnableIntc"}, {0x15, "_DisableIntc"}, {0x16, "_EnableDmac"}, {0x17, "_DisableDmac"},
    {0x18, "_SetAlarm"}, {0x19, "_ReleaseAlarm"}, {-0x1A, "_iEnableIntc"}, {-0x1B, "_iDisableIntc"},
    {-0x1C, "_iEnableDmac"}, {-0x1D, "_iDisableDmac"}, {-0x1E, "_iSetAlarm"}, {-0x1F, "_iReleaseAlarm"},
    {0x20, "CreateThread"}, {0x21, "DeleteThread"}, {0x22, "StartThread"}, {0x23, "ExitThread"},
    {0x24, "ExitDeleteThread"}, {0x25, "TerminateThread"}, {-0x26, "iTerminateThread"},
    {0x27, "DisableDispatchThread"}, {0x28, "EnableDispatchThread"}, {0x29, "ChangeThreadPriority"},
    {-0x2A, "iChangeThreadPriority"}, {0x2B, "RotateThreadReadyQueue"},
    {-0x2C, "_iRotateThreadReadyQueue"}, {0x2D, "ReleaseWaitThread"}, {-0x2E, "iReleaseWaitThread"},
    {0x2F, "GetThreadId"}, {-0x2F, "_iGetThreadId"}, {0x30, "ReferThreadStatus"},
    {-0x31, "iReferThreadStatus"}, {0x32, "SleepThread"}, {0x33, "WakeupThread"},
    {-0x34, "_iWakeupThread"}, {0x35, "CancelWakeupThread"}, {-0x36, "iCancelWakeupThread"},
    {0x37, "SuspendThread"}, {-0x38, "_iSuspendThread"}, {0x39, "ResumeThread"},
    {-0x3A, "iResumeThread"}, {0x3C, "SetupThread"}, {0x3D, "SetupHeap"}, {0x3E, "EndOfHeap"},
    {0x40, "CreateSema"}, {0x41, "DeleteSema"}, {0x42, "SignalSema"}, {-0x43, "iSignalSema"},
    {0x44, "WaitSema"}, {0x45, "PollSema"}, {-0x46, "iPollSema"}, {0x47, "ReferSemaStatus"},
    {-0x48, "iReferSemaStatus"}, {-0x49, "iDeleteSema"}, {0x4A, "SetOsdConfigParam"},
    {0x4B, "GetOsdConfigParam"}, {0x4C, "GetGsHParam"}, {0x4D, "GetGsVParam"},
    {0x4E, "SetGsHParam"}, {0x4F, "SetGsVParam"}, {0x50, "CreateEventFlag"},
    {0x51, "DeleteEventFlag"}, {0x52, "SetEventFlag"}, {0x53, "iSetEventFlag"},
    {0x55, "PutTLBEntry"}, {-0x55, "iPutTLBEntry"}, {0x56, "_SetTLBEntry"}, {-0x56, "iSetTLBEntry"},
    {0x57, "GetTLBEntry"}, {-0x57, "iGetTLBEntry"}, {0x58, "ProbeTLBEntry"},
    {-0x58, "iProbeTLBEntry"}, {0x59, "ExpandScratchPad"}, {0x5A, "Copy"},
    {0x5B, "GetEntryAddress"}, {0x5C, "EnableIntcHandler"}, {-0x5C, "iEnableIntcHandler"},
    {0x5D, "DisableIntcHandler"}, {-0x5D, "iDisableIntcHandler"}, {0x5E, "EnableDmacHandler"},
    {-0x5E, "iEnableDmacHandler"}, {0x5F, "DisableDmacHandler"}, {-0x5F, "iDisableDmacHandler"},
    {0x60, "KSeg0"}, {0x61, "EnableCache"}, {0x62, "DisableCache"}, {0x63, "GetCop0"},
    {0x64, "FlushCache"}, {0x66, "CpuConfig"}, {-0x67, "iGetCop0"}, {-0x68, "iFlushCache"},
    {-0x6A, "iCpuConfig"}, {0x6B, "sceSifStopDma"}, {0x6C, "SetCPUTimerHandler"},
    {0x6D, "SetCPUTimer"}, {0x6E, "SetOsdConfigParam2"}, {0x6F, "GetOsdConfigParam2"},
    {0x70, "GsGetIMR"}, {-0x70, "iGsGetIMR"}, {0x71, "GsPutIMR"}, {-0x71, "iGsPutIMR"},
    {0x72, "SetPgifHandler"}, {0x73, "SetVSyncFlag"}, {0x74, "SetSyscall"}, {0x75, "_print"},
    {0x76, "sceSifDmaStat"}, {-0x76, "isceSifDmaStat"}, {0x77, "sceSifSetDma"},
    {-0x77, "isceSifSetDma"}, {0x78, "sceSifSetDChain"}, {-0x78, "isceSifSetDChain"},
    {0x79, "sceSifSetReg"}, {0x7A, "sceSifGetReg"}, {0x7B, "ExecOSD"}, {0x7C, "Deci2Call"},
    {0x7D, "PSMode"}, {0x7E, "MachineType"}, {0x7F, "GetMemorySize"}, {0x80, "_GetGsDxDyOffset"},
    {0x82, "_InitTLB"}, {0x83, "FindAddress"}, {0x85, "SetMemoryMode"}, {0x86, "GetMemoryMode"},
    {0x87, "ExecPSX"}, {0xFC, "SetAlarm"}, {0xFE, "ReleaseAlarm"}, {-0xFD, "iSetAlarm"},
    {-0xFF, "iReleaseAlarm"},
};

std::uint32_t arg(Context* c, unsigned i) {
    // a0..a3 = r4..r7; os seguintes em t0..t3 = r8..r11 (EABI do ps2sdk)
    return c->r[4 + i].uw[0];
}

}  // namespace

std::string syscallName(std::int32_t number) {
    for (const auto& s : kSyscalls) {
        if (s.number == number) return s.name;
    }
    return "desconhecida";
}

Kernel::Kernel(Runtime& rt) : rt_(rt), ready_(128) {}

Kernel::~Kernel() {
    {
        std::lock_guard lk(batonMutex_);
        shutdown_ = true;
        for (auto& [id, t] : threads_) t->cv.notify_all();
    }
    for (auto& [id, t] : threads_) {
        if (t->host) t->host->join();
    }
}

void Kernel::notImplemented(std::int32_t number, std::uint32_t pc, const char* phase) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%s0x%X", number < 0 ? "-" : "",
                  static_cast<unsigned>(number < 0 ? -number : number));
    throw Unimplemented("syscall " + std::string(buf) + " (" + syscallName(number) +
                            ") ainda não implementada" + (phase ? std::string(" — ") + phase : "") +
                            "; chamada de " + rt_.describe(pc),
                        pc);
}

void Kernel::syscall(Context* c, std::uint32_t pc) {
    const auto number = static_cast<std::int32_t>(c->r[3].uw[0]);
    const std::uint64_t ret = dispatch(c, number, pc);
    if (rt_.options().traceSyscalls) {
        std::fprintf(stderr, "[syscall] t%d %s(0x%x, 0x%x, 0x%x, 0x%x) = 0x%llx  @ %s\n",
                     currentThreadId(), syscallName(number).c_str(), arg(c, 0), arg(c, 1), arg(c, 2),
                     arg(c, 3), static_cast<unsigned long long>(ret), rt_.describe(pc).c_str());
    }
    c->r[2].ud[0] = ret;
    // Fim da syscall é um safepoint: avança o relógio, entrega interrupções
    // pendentes e troca de thread se preciso (o v0 já está no contexto).
    rt_.safepoint(c, pc, kSyscallCycles);
}

std::int32_t Kernel::setupThread(Context* c, std::uint32_t pc) {
    const std::uint32_t gp = arg(c, 0);
    std::uint32_t stack = arg(c, 1);
    const std::uint32_t stackSize = arg(c, 2);
    const std::uint32_t args = arg(c, 3);
    c->r[28].sd[0] = static_cast<std::int32_t>(gp);
    if (stack == 0xFFFFFFFFu) stack = Memory::kRamSize - stackSize;
    stackBottom_ = stack;
    mainStack_ = stack;
    mainStackSize_ = stackSize;
    if (current_) {
        current_->gp = gp;
        current_->stack = stack;
        current_->stackSize = stackSize;
    }
    // Argumentos do programa: struct { int argc; char* argv[16]; char payload[256]; }
    if (args != 0) {
        Memory& m = rt_.memory();
        const std::uint32_t argvBase = args + 4;
        const std::uint32_t payload = args + 4 + 16 * 4;
        std::uint32_t used = 0;
        std::uint32_t argc = 0;
        for (const auto& a : bootArgs_) {
            if (argc == 16 || used + a.size() + 1 > 256) break;
            m.copyToGuest(payload + used, a.c_str(), static_cast<std::uint32_t>(a.size() + 1), pc);
            m.write<std::uint32_t>(argvBase + 4 * argc, payload + used, pc);
            used += static_cast<std::uint32_t>(a.size() + 1);
            ++argc;
        }
        m.write<std::uint32_t>(args, argc, pc);
    }
    // O kernel reserva uma área no topo da pilha para o contexto da thread.
    return static_cast<std::int32_t>(stack + stackSize - 0x2A0);
}

std::int32_t Kernel::addHandler(std::vector<Handler>& list, unsigned cause, std::uint32_t fn,
                                std::int32_t next, std::uint32_t argument, std::uint32_t gp) {
    Handler h{nextHandlerId_++, cause, fn, argument, gp, true};
    // next < 0: insere no fim; caso contrário no início (comportamento do kernel)
    if (next < 0) list.push_back(h);
    else list.insert(list.begin(), h);
    return h.id;
}

void Kernel::runHandlers(std::vector<Handler>& list, std::uint32_t enabledMask, unsigned cause,
                         std::uint32_t pc, bool ignoreEie) {
    if (!(enabledMask & (1u << cause))) return;
    if (!ignoreEie && !(rt_.context().cop0[cop0::Status] & ops::kStatusEIE)) return;  // DI ativo
    ++interruptDepth_;
    try {
        // Copia: o handler pode registrar/remover outros handlers.
        const std::vector<Handler> handlers = list;
        for (const auto& h : handlers) {
            if (h.cause != cause || !h.enabled) continue;
            // Handlers do ps2sdk: int handler(int cause, void* arg, void* addr).
            // Retorno < 0 interrompe a cadeia.
            Context& c = rt_.context();
            const std::uint32_t savedGp = c.r[28].uw[0];
            c.r[28].sd[0] = static_cast<std::int32_t>(h.gp);
            const auto ret = static_cast<std::int32_t>(rt_.invokeGuest(h.function, {cause, h.arg, 0}, pc));
            c.r[28].sd[0] = static_cast<std::int32_t>(savedGp);
            if (ret < 0) break;
        }
    } catch (...) {
        --interruptDepth_;
        throw;
    }
    --interruptDepth_;
}

void Kernel::raiseDmacInterrupt(unsigned channel, std::uint32_t pc) {
    runHandlers(dmacHandlers_, dmacMask_, channel, pc);
}

bool Kernel::canDeliverDmac(unsigned channel) const {
    if (!(dmacMask_ & (1u << channel))) return false;
    if (!(rt_.context().cop0[cop0::Status] & ops::kStatusEIE)) return false;
    for (const auto& h : dmacHandlers_) {
        if (h.cause == channel && h.enabled) return true;
    }
    return false;
}

void Kernel::queueAlarm(const Timing::Alarm& a) {
    pendingAlarms_.push_back({a.id, a.lines, a.handler, a.arg, a.gp});
}

void Kernel::serviceInterrupts(std::uint32_t pc, bool allowReschedule, bool idle) {
    if (interruptDepth_ == 0 && (idle || (rt_.context().cop0[cop0::Status] & ops::kStatusEIE))) {
        // Um handler pode gerar novas causas; limita as voltas por segurança.
        for (int round = 0; round < 8; ++round) {
            const std::uint32_t pending = intcStat_ & intcMask_;
            if (!pending && pendingAlarms_.empty()) break;
            for (unsigned cause = 0; cause < 32; ++cause) {
                const std::uint32_t bit = 1u << cause;
                if (!(pending & bit)) continue;
                intcStat_ &= ~bit;  // o kernel limpa a causa após os handlers
                runHandlers(intcHandlers_, intcMask_, cause, pc, idle);
            }
            // Alarmes: void handler(s32 id, u16 time, void* arg), em contexto de interrupção.
            std::vector<PendingAlarm> alarms;
            alarms.swap(pendingAlarms_);
            for (const auto& a : alarms) {
                ++interruptDepth_;
                Context& c = rt_.context();
                const std::uint32_t savedGp = c.r[28].uw[0];
                c.r[28].sd[0] = static_cast<std::int32_t>(a.gp);
                try {
                    rt_.invokeGuest(a.handler, {static_cast<std::uint32_t>(a.id), a.lines, a.arg}, pc);
                } catch (...) {
                    --interruptDepth_;
                    throw;
                }
                c.r[28].sd[0] = static_cast<std::int32_t>(savedGp);
                --interruptDepth_;
            }
        }
        rt_.iop().deliverPending(pc);
    }
    if (allowReschedule && reschedulePending_ && interruptDepth_ == 0) reschedule(pc);
}

std::uint64_t Kernel::dispatch(Context* c, std::int32_t number, std::uint32_t pc) {
    Memory& m = rt_.memory();
    auto ret32 = [](std::int32_t v) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(v)); };
    auto sarg = [c](unsigned i) { return static_cast<std::int32_t>(arg(c, i)); };
    switch (number) {
        // ---- Sistema -----------------------------------------------------
        case 0x01: return 0;  // ResetEE
        case 0x02: rt_.hardware().setGsCrt(arg(c, 0), arg(c, 1), arg(c, 2)); return 0;
        case 0x04: throw ProgramExit{sarg(0)};  // Exit
        case 0x3C: return ret32(setupThread(c, pc));
        case 0x3D: {  // SetupHeap(start, size)
            heapStart_ = arg(c, 0);
            const auto size = sarg(1);
            heapEnd_ = size < 0 ? stackBottom_ : heapStart_ + static_cast<std::uint32_t>(size);
            return ret32(static_cast<std::int32_t>(heapStart_));
        }
        case 0x3E: return ret32(static_cast<std::int32_t>(heapEnd_));  // EndOfHeap
        case 0x7F: return Memory::kRamSize;                            // GetMemorySize
        case 0x7D: return 0;                                           // PSMode
        case 0x7E: return 0;                                           // MachineType
        case 0x61: case 0x62: return 0;                                // Enable/DisableCache
        case 0x64: case -0x68: return 0;                               // FlushCache: não há cache a simular
        case 0x82: return 0;                                           // _InitTLB
        case 0x56: case -0x56: return 0;                               // _SetTLBEntry
        case 0x63: case -0x67: return ops::readCop0(c, arg(c, 0) & 31, pc);  // GetCop0
        case 0x5A: {  // Copy(dst, src, size): memcpy em modo kernel
            std::vector<std::uint8_t> tmp(arg(c, 2));
            m.copyFromGuest(tmp.data(), arg(c, 1), arg(c, 2), pc);
            m.copyToGuest(arg(c, 0), tmp.data(), arg(c, 2), pc);
            return 0;
        }
        case 0x74:  // SetSyscall(número, endereço): o HLE continua tratando a syscall
            userSyscalls_[sarg(0)] = arg(c, 1);
            return 0;
        case 0x5B:  // GetEntryAddress(syscall): não há kernel real para apontar
            return 0;
        case 0x75:  // _print(fmt, ...): imprime só a string de formato
            std::fputs(m.readCString(arg(c, 0), 4096, pc).c_str(), stdout);
            return 0;

        // ---- Configuração do OSD -------------------------------------------
        case 0x4A: osdConfig_ = m.read<std::uint32_t>(arg(c, 0), pc); return 0;
        case 0x4B: m.write<std::uint32_t>(arg(c, 0), osdConfig_, pc); return 0;
        case 0x6E: case 0x6F: {  // Set/GetOsdConfigParam2(ptr, size, offset)
            const std::uint32_t size = arg(c, 1), offset = arg(c, 2);
            if (offset + size > sizeof(osdConfig2_)) return 0;
            auto* bytes = reinterpret_cast<std::uint8_t*>(osdConfig2_) + offset;
            if (number == 0x6E) m.copyFromGuest(bytes, arg(c, 0), size, pc);
            else m.copyToGuest(arg(c, 0), bytes, size, pc);
            return 0;
        }

        // ---- GS ----------------------------------------------------------
        case 0x70: case -0x70: return gsImr_;                     // GsGetIMR
        case 0x71: case -0x71: gsImr_ = c->r[4].ud[0]; return 0;  // GsPutIMR

        // ---- Threads ----------------------------------------------------------
        case 0x20: return ret32(createThread(arg(c, 0), pc));  // CreateThread
        case 0x21: {                                           // DeleteThread
            Thread* t = thread(sarg(0), pc, false);
            if (!t || t->status != THS_DORMANT) return ret32(-1);
            t->deleted = true;
            return ret32(t->id);
        }
        case 0x22: return ret32(startThread(sarg(0), arg(c, 1), pc));  // StartThread
        case 0x23: throw ThreadExitSignal{};                            // ExitThread
        case 0x24: current_->deleted = true; throw ThreadExitSignal{};  // ExitDeleteThread
        case 0x25: case -0x26: {                                         // TerminateThread
            Thread* t = thread(sarg(0), pc, false);
            if (!t || t == current_) return ret32(-1);
            removeReady(t);
            if (t->status & THS_WAIT) {
                for (auto& [id, s] : semas_) std::erase(s.waiters, t->id);
                t->unwind = true;  // a thread do host está parada dentro da syscall
            }
            t->status = THS_DORMANT;
            t->suspended = false;
            return ret32(t->id);
        }
        case 0x29: case -0x2A: {  // ChangeThreadPriority(tid, prio)
            Thread* t = thread(sarg(0), pc);
            const std::int32_t prio = sarg(1);
            if (!t || prio < 0 || prio > 127) return ret32(-1);
            const std::int32_t old = t->priority;
            if (t->status == THS_READY) {
                removeReady(t);
                t->priority = prio;
                makeReady(t);
            } else {
                t->priority = prio;
                if (t == current_) reschedulePending_ = true;
            }
            return ret32(old);
        }
        case 0x2B: case -0x2C: {  // RotateThreadReadyQueue(prio)
            const std::int32_t prio = sarg(0);
            if (prio < 0 || prio > 127) return ret32(-1);
            auto& q = ready_[static_cast<std::size_t>(prio)];
            if (current_->priority == prio && current_->status == THS_RUN) {
                if (!q.empty() && number == 0x2B) {
                    current_->status = THS_READY;
                    q.push_back(current_);
                    Thread* next = q.front();
                    switchTo(next, pc);
                }
            } else if (!q.empty()) {
                q.push_back(q.front());
                q.pop_front();
            }
            return ret32(prio);
        }
        case 0x2D: case -0x2E: {  // ReleaseWaitThread
            Thread* t = thread(sarg(0), pc, false);
            if (!t || !(t->status & THS_WAIT)) return ret32(-1);
            for (auto& [id, s] : semas_) std::erase(s.waiters, t->id);
            t->waitResult = -1;
            t->status &= ~THS_WAIT;
            if (!t->suspended) makeReady(t);
            else t->status = THS_SUSPEND;
            return ret32(t->id);
        }
        case 0x2F: case -0x2F: return ret32(currentThreadId());  // GetThreadId
        case 0x30: case -0x31: {                                 // ReferThreadStatus(tid, status*)
            Thread* t = thread(sarg(0), pc);
            if (!t) return ret32(-1);
            if (const std::uint32_t st = arg(c, 1)) {
                const std::uint32_t fields[12] = {
                    t->status, t->entry, t->stack, t->stackSize, t->gp,
                    static_cast<std::uint32_t>(t->initPriority), static_cast<std::uint32_t>(t->priority),
                    t->attr, t->option, t->status & THS_WAIT ? t->waitType : 0,
                    static_cast<std::uint32_t>(t->waitId), static_cast<std::uint32_t>(t->wakeupCount)};
                m.copyToGuest(st, fields, sizeof(fields), pc);
            }
            return t->status;
        }
        case 0x32: {  // SleepThread
            if (current_->wakeupCount > 0) {
                current_->wakeupCount--;
                return 0;
            }
            current_->waitResult = 0;
            blockCurrent(TSW_SLEEP, 0, pc);
            return ret32(static_cast<std::int32_t>(current_->waitResult));
        }
        case 0x33: case -0x34: {  // WakeupThread
            Thread* t = thread(sarg(0), pc, false);
            if (!t || t->status == THS_DORMANT) return ret32(-1);
            if ((t->status & THS_WAIT) && t->waitType == TSW_SLEEP) {
                t->status &= ~THS_WAIT;
                if (t->suspended) t->status = THS_SUSPEND;
                else makeReady(t);
            } else {
                t->wakeupCount++;
            }
            return ret32(t->id);
        }
        case 0x35: case -0x36: {  // CancelWakeupThread
            Thread* t = thread(sarg(0), pc);
            if (!t) return ret32(-1);
            const std::int32_t n = t->wakeupCount;
            t->wakeupCount = 0;
            return ret32(n);
        }
        case 0x37: case -0x38: {  // SuspendThread
            Thread* t = thread(sarg(0), pc, false);
            if (!t || t == current_ || t->status == THS_DORMANT || t->suspended) return ret32(-1);
            t->suspended = true;
            if (t->status == THS_READY) {
                removeReady(t);
                t->status = THS_SUSPEND;
            } else if (t->status & THS_WAIT) {
                t->status = THS_WAIT | THS_SUSPEND;
            }
            return ret32(t->id);
        }
        case 0x39: case -0x3A: {  // ResumeThread
            Thread* t = thread(sarg(0), pc, false);
            if (!t || !t->suspended) return ret32(-1);
            t->suspended = false;
            if (t->status == THS_SUSPEND) makeReady(t);
            else t->status &= ~THS_SUSPEND;
            return ret32(t->id);
        }
        case 0x27: case 0x28: return 0;  // Disable/EnableDispatchThread (não suportado pelo kernel real)

        // ---- Semáforos -----------------------------------------------------
        case 0x40: {  // CreateSema(ee_sema_t*)
            const std::uint32_t p = arg(c, 0);
            Semaphore s;
            s.maxCount = static_cast<std::int32_t>(m.read<std::uint32_t>(p + 4, pc));
            s.initCount = static_cast<std::int32_t>(m.read<std::uint32_t>(p + 8, pc));
            s.attr = m.read<std::uint32_t>(p + 16, pc);
            s.option = m.read<std::uint32_t>(p + 20, pc);
            s.count = s.initCount;
            const std::int32_t id = nextSema_++;
            semas_[id] = s;
            return ret32(id);
        }
        case 0x41: case -0x49: {  // DeleteSema: libera quem espera com erro
            auto it = semas_.find(sarg(0));
            if (it == semas_.end()) return ret32(-1);
            for (std::int32_t tid : it->second.waiters) {
                if (Thread* t = thread(tid, pc, false)) {
                    t->waitResult = -1;
                    t->status &= ~THS_WAIT;
                    if (t->suspended) t->status = THS_SUSPEND;
                    else makeReady(t);
                }
            }
            semas_.erase(it);
            return ret32(sarg(0));
        }
        case 0x42: case -0x43: {  // SignalSema
            auto it = semas_.find(sarg(0));
            if (it == semas_.end()) return ret32(-1);
            Semaphore& s = it->second;
            while (!s.waiters.empty()) {
                Thread* t = thread(s.waiters.front(), pc, false);
                s.waiters.pop_front();
                if (!t || !(t->status & THS_WAIT)) continue;
                t->waitResult = it->first;
                t->status &= ~THS_WAIT;
                if (t->suspended) t->status = THS_SUSPEND;
                else makeReady(t);
                return ret32(it->first);
            }
            if (s.count >= s.maxCount && s.maxCount > 0) return ret32(-1);
            s.count++;
            return ret32(it->first);
        }
        case 0x44: {  // WaitSema
            auto it = semas_.find(sarg(0));
            if (it == semas_.end()) return ret32(-1);
            if (it->second.count > 0) {
                it->second.count--;
                return ret32(it->first);
            }
            it->second.waiters.push_back(current_->id);
            blockCurrent(TSW_SEMA, it->first, pc);
            return ret32(static_cast<std::int32_t>(current_->waitResult));
        }
        case 0x45: case -0x46: {  // PollSema
            auto it = semas_.find(sarg(0));
            if (it == semas_.end() || it->second.count <= 0) return ret32(-1);
            it->second.count--;
            return ret32(it->first);
        }
        case 0x47: case -0x48: {  // ReferSemaStatus(id, ee_sema_t*)
            auto it = semas_.find(sarg(0));
            if (it == semas_.end()) return ret32(-1);
            const auto& s = it->second;
            const std::uint32_t fields[6] = {
                static_cast<std::uint32_t>(s.count), static_cast<std::uint32_t>(s.maxCount),
                static_cast<std::uint32_t>(s.initCount), static_cast<std::uint32_t>(s.waiters.size()),
                s.attr, s.option};
            m.copyToGuest(arg(c, 1), fields, sizeof(fields), pc);
            return ret32(it->first);
        }

        // ---- Interrupções ----------------------------------------------------
        case 0x10:  // AddIntcHandler(cause, handler, next, arg)
            return ret32(addHandler(intcHandlers_, arg(c, 0), arg(c, 1), sarg(2), arg(c, 3), c->r[28].uw[0]));
        case 0x12:  // AddDmacHandler(channel, handler, next, arg)
            return ret32(addHandler(dmacHandlers_, arg(c, 0), arg(c, 1), sarg(2), arg(c, 3), c->r[28].uw[0]));
        case 0x11: case 0x13: {  // Remove{Intc,Dmac}Handler(cause, id)
            auto& list = number == 0x11 ? intcHandlers_ : dmacHandlers_;
            const auto id = sarg(1);
            const auto before = list.size();
            std::erase_if(list, [id](const Handler& h) { return h.id == id; });
            return ret32(list.size() < before ? 0 : -1);
        }
        case 0x14: case -0x1A: case 0x15: case -0x1B: {  // _Enable/_DisableIntc
            const std::uint32_t bit = 1u << (arg(c, 0) & 31);
            const bool was = (intcMask_ & bit) != 0;
            if (number == 0x14 || number == -0x1A) intcMask_ |= bit;
            else intcMask_ &= ~bit;
            return was ? 0 : 1;
        }
        case 0x16: case -0x1C: case 0x17: case -0x1D: {  // _Enable/_DisableDmac
            const std::uint32_t bit = 1u << (arg(c, 0) & 31);
            const bool was = (dmacMask_ & bit) != 0;
            if (number == 0x16 || number == -0x1C) dmacMask_ |= bit;
            else dmacMask_ &= ~bit;
            return was ? 0 : 1;
        }
        case 0x5C: case -0x5C: case 0x5D: case -0x5D: case 0x5E: case -0x5E: case 0x5F: case -0x5F: {
            // Enable/Disable{Intc,Dmac}Handler(id)
            const bool intc = number == 0x5C || number == -0x5C || number == 0x5D || number == -0x5D;
            const bool enable = number == 0x5C || number == -0x5C || number == 0x5E || number == -0x5E;
            auto& list = intc ? intcHandlers_ : dmacHandlers_;
            for (auto& h : list) {
                if (h.id == sarg(0)) h.enabled = enable;
            }
            return 0;
        }

        // ---- SIF (comunicação EE <-> IOP) -------------------------------------
        case 0x77: case -0x77: return rt_.iop().sifSetDma(arg(c, 0), arg(c, 1), pc);
        case 0x76: case -0x76: return ret32(-1);  // sceSifDmaStat: transferências HLE terminam na hora
        case 0x78: case -0x78: return 0;          // sceSifSetDChain
        case 0x79: return rt_.iop().sifSetReg(arg(c, 0), arg(c, 1));
        case 0x7A: return rt_.iop().sifGetReg(arg(c, 0));
        case 0x6B: return 0;  // sceSifStopDma

        // ---- Alarmes (unidade: linhas HSYNC) e VSync -------------------------------
        case 0x18: case -0x1E: case 0xFC: case -0xFD:  // SetAlarm(time, handler, arg)
            return ret32(rt_.timing().setAlarm(static_cast<std::uint16_t>(arg(c, 0)), arg(c, 1), arg(c, 2),
                                               c->r[28].uw[0]));
        case 0x19: case -0x1F: case 0xFE: case -0xFF:  // ReleaseAlarm(id)
            return ret32(rt_.timing().releaseAlarm(sarg(0)) ? sarg(0) : -1);
        case 0x73:  // SetVSyncFlag(u32* flag, u64* csr)
            rt_.timing().setVSyncFlag(arg(c, 0), arg(c, 1));
            return 0;
        case 0x50: case 0x51: case 0x52: case 0x53:
            notImplemented(number, pc, "o kernel do EE não implementa event flags de forma utilizável");
        case 0x06: case 0x07: case 0x7B: case 0x87:
            notImplemented(number, pc, "carregar outro executável não é suportado");
        default:
            notImplemented(number, pc, nullptr);
    }
}

}  // namespace anyps2::rt
