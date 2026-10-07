#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace anyps2::rt {

class Runtime;

// Relógio do EE (em ciclos de 294,912 MHz) e os eventos que dependem dele:
// timers T0–T3, VBlank (início/fim, GS_CSR.VSINT/FIELD, SetVSyncFlag) e os
// alarmes do kernel.
//
// Modos:
//  * Real    — o tempo é o do host. Quando todas as threads dormem, o
//              runtime espera de verdade até o próximo evento.
//  * Virtual — determinístico: o tempo avança com as instruções executadas
//              (contadas nos safepoints, ~1 ciclo por instrução) e pula
//              direto para o próximo evento quando todas as threads dormem.
//              Usado nos testes (ANYPS2_CLOCK=virtual).
class Timing {
public:
    enum class Mode { Real, Virtual };

    static constexpr std::uint64_t kEeHz = 294912000ull;
    static constexpr std::uint64_t kCyclesPerLine = 18743;   // NTSC: ~15,734 kHz
    static constexpr std::uint64_t kLinesPerField = 262;
    static constexpr std::uint64_t kCyclesPerField = kCyclesPerLine * kLinesPerField;  // ~59,94 Hz
    static constexpr std::uint64_t kVblankLines = 22;  // duração do VBlank

    Timing(Runtime& rt, Mode mode);

    Mode mode() const { return mode_; }
    std::uint64_t now() const;
    // Modo virtual: soma ciclos executados.
    void consume(std::int64_t cycles);
    // Dispara os eventos vencidos até agora.
    void process(std::uint32_t pc);
    // Ciclos até o próximo evento (para o orçamento dos safepoints).
    std::uint64_t cyclesUntilNextEvent() const;
    // Todas as threads bloqueadas: avança (virtual) ou espera (real) até o
    // próximo evento e o processa. false se não há evento nenhum agendado.
    bool advanceToNextEvent(std::uint32_t pc);

    // ---- Timers (registradores 0x1000_0000 + n*0x800) -------------------
    std::uint32_t readTimer(unsigned timer, unsigned reg);
    void writeTimer(unsigned timer, unsigned reg, std::uint32_t value);

    // ---- GS ----------------------------------------------------------------
    std::uint64_t readGsCsr();
    void writeGsCsr(std::uint64_t value);
    void setVSyncFlag(std::uint32_t flagAddr, std::uint32_t csrAddr) {
        vsyncFlag_ = flagAddr;
        vsyncCsr_ = csrAddr;
    }
    std::uint64_t vblankCount() const { return vblanks_; }

    // ---- Alarmes do kernel (unidade: linhas HSYNC) --------------------------
    std::int32_t setAlarm(std::uint16_t lines, std::uint32_t handler, std::uint32_t arg, std::uint32_t gp);
    bool releaseAlarm(std::int32_t id);
    struct Alarm {
        std::int32_t id;
        std::uint64_t due;
        std::uint16_t lines;
        std::uint32_t handler;
        std::uint32_t arg;
        std::uint32_t gp;
    };

private:
    struct Timer {
        std::uint32_t mode = 0;
        std::uint32_t comp = 0;
        std::uint32_t hold = 0;
        std::uint32_t count = 0;
        std::uint64_t base = 0;  // ciclo em que `count` era válido
    };
    std::uint64_t divider(const Timer& t) const;
    void advanceTimer(unsigned n, std::uint64_t now);
    std::uint64_t timerNextEvent(unsigned n, std::uint64_t now) const;
    std::uint64_t nextEventTime() const;
    void raise(unsigned cause);

    Runtime& rt_;
    Mode mode_;
    std::chrono::steady_clock::time_point start_;
    std::uint64_t virtualNow_ = 0;
    Timer timers_[4];
    std::uint64_t nextVblankStart_ = kCyclesPerField;
    std::uint64_t nextVblankEnd_ = kCyclesPerField + kVblankLines * kCyclesPerLine;
    std::uint64_t vblanks_ = 0;
    std::uint64_t csr_ = 0;
    std::uint32_t vsyncFlag_ = 0, vsyncCsr_ = 0;
    std::vector<Alarm> alarms_;
    std::int32_t nextAlarmId_ = 1;
};

}  // namespace anyps2::rt
