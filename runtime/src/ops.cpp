// Partes não-inline da semântica das instruções (exceções, COP0).

#include <chrono>

#include "anyps2/runtime/iop.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/ops.h"

#include "anyps2/runtime/dmac.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt::ops {

void overflowException(u32 pc) {
    throw GuestError("exceção de overflow aritmético (ADD/ADDI/SUB/DADD...)", pc);
}

void trapException(u32 pc) {
    throw GuestError("exceção de trap (instrução T*)", pc);
}

void breakException(Context* c, u32 pc) {
    throw GuestError("instrução BREAK executada em " + c->rt->describe(pc), pc);
}

void unsupported(Context* c, u32 pc, const char* text) {
    throw Unimplemented(std::string("instrução não suportada: ") + text + " em " + c->rt->describe(pc), pc);
}

void unimplementedCop0(Context* c, u32 pc, const char* what) {
    throw Unimplemented(std::string("COP0: ") + what + " em " + c->rt->describe(pc), pc);
}

namespace {
// O EE roda a 294,912 MHz; COUNT incrementa a cada ciclo.
u32 cycleCount() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    const auto ns = duration_cast<nanoseconds>(steady_clock::now() - t0).count();
    return static_cast<u32>(static_cast<u64>(ns) * 294912ull / 1000000ull);
}
}  // namespace

std::uint32_t readCop0(Context* c, unsigned reg, u32 pc) {
    switch (reg) {
        case cop0::Count: return cycleCount();
        case cop0::Index: case cop0::Random: case cop0::EntryLo0: case cop0::EntryLo1:
        case cop0::Context: case cop0::PageMask: case cop0::Wired: case cop0::BadVAddr:
        case cop0::EntryHi: case cop0::Compare: case cop0::Status: case cop0::Cause:
        case cop0::EPC: case cop0::PRId: case cop0::Config: case cop0::BadPAddr:
        case cop0::TagLo: case cop0::TagHi: case cop0::ErrorEPC:
            return c->cop0[reg];
        default:
            throw Unimplemented("leitura do registrador " + std::to_string(reg) + " do COP0", pc);
    }
}

void writeCop0(Context* c, unsigned reg, u32 value, u32 pc) {
    switch (reg) {
        case cop0::Count: case cop0::PRId: case cop0::BadVAddr: case cop0::BadPAddr:
            return;  // somente leitura (ou ignorado)
        case cop0::Index: case cop0::Random: case cop0::EntryLo0: case cop0::EntryLo1:
        case cop0::Context: case cop0::PageMask: case cop0::Wired: case cop0::EntryHi:
        case cop0::Compare: case cop0::Status: case cop0::Cause: case cop0::EPC:
        case cop0::Config: case cop0::TagLo: case cop0::TagHi: case cop0::ErrorEPC:
            c->cop0[reg] = value;
            return;
        default:
            throw Unimplemented("escrita no registrador " + std::to_string(reg) + " do COP0", pc);
    }
}

bool cop0Condition(Context* c) {
    return c->rt->dmac().cpcond0();
}

std::uint32_t readPerfCounter(Context*, unsigned reg, bool counter) {
    if (!counter) return 0;           // PCCR
    return reg == 0 ? cycleCount() : 0;  // PCR0 ~ ciclos
}

void onInterruptsEnabled(Context* c) {
    c->rt->kernel().serviceInterrupts(c->pc);
}

void ctc2(Context* c, unsigned id, u32 value, u32 pc) {
    if (id == 0) return;  // vi0 é sempre zero
    if (id < 16) {
        c->vi[id] = value & 0xFFFF;
        return;
    }
    // Registradores de controle do VU0 (status, MAC, clip, R, I, Q, TPC,
    // CMSAR0, FBRST, VPU-STAT, CMSAR1...). Guardamos o valor; os efeitos
    // (iniciar microprogramas, reset) chegam na Fase 5.
    if (id == 31) {  // CMSAR1: inicia o VU1
        throw Unimplemented("CTC2 em CMSAR1 (iniciar microprograma do VU1) — Fase 5", pc);
    }
    c->vi[id] = value;
}

}  // namespace anyps2::rt::ops
