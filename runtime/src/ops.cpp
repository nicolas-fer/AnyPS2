// Partes não-inline da semântica das instruções (exceções, COP0).

#include <chrono>

#include "anyps2/runtime/iop.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/ops.h"

#include "anyps2/runtime/dmac.h"
#include "anyps2/runtime/vu/vu.h"
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
    using namespace vucore::reg;
    if (id == 0) return;  // vi0 é sempre zero
    if (id < 16) {
        c->vi[id] = value & 0xFFFF;
        return;
    }
    switch (id) {
        case Status: c->vi[Status] = (c->vi[Status] & 0x3Fu) | (value & 0xFC0u); return;  // só os sticky
        case Mac: case TPC: case VPUSTAT: return;                                        // somente leitura
        case Clip: c->vi[Clip] = value & 0xFFFFFFu; return;
        case R: c->vi[R] = (value & 0x007FFFFFu) | 0x3F800000u; return;
        case I: case Q: c->vi[id] = value; return;
        case CMSAR0: c->vi[CMSAR0] = value & 0xFFFF; return;
        case FBRST:
            if (value & 0x002) c->rt->vu0().reset();
            if (value & 0x200) c->rt->vu1().reset();
            return;
        case CMSAR1:  // escrever CMSAR1 inicia o microprograma do VU1
            c->vi[CMSAR1] = value & 0xFFFF;
            c->rt->vu1().start((value & 0xFFFF) * 8, pc);
            return;
        default:
            c->vi[id] = value;
            return;
    }
}

u32 cfc2(Context* c, unsigned id) {
    using namespace vucore::reg;
    if (id < 16) return c->vi[id] & 0xFFFF;
    switch (id) {
        case FBRST: case VPUSTAT: return 0;  // VUs nunca ocupados
        case Status: return c->vi[Status] & 0xFFF;
        case Mac: return c->vi[Mac] & 0xFFFF;
        case Clip: return c->vi[Clip] & 0xFFFFFF;
        default: return c->vi[id];
    }
}

void vu0Macro(Context* c, u32 lowerWord, u32 upperWord, u32 pc) {
    c->rt->vu0().macro(vu::decode(lowerWord, upperWord), pc);
}

void vu0Call(Context* c, u32 addr, u32 pc) {
    c->rt->vu0().start(addr, pc);
}

}  // namespace anyps2::rt::ops
