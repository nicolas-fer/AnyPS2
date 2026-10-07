#pragma once

#include <cstdint>
#include <map>
#include <string>

#include "anyps2/runtime/memory.h"

namespace anyps2::rt {

class Runtime;

// Registradores de hardware do EE (0x1000_0000–0x1000_FFFF) e registradores
// privilegiados do GS (0x1200_0000–0x1200_1FFF).
//
// Fase 2: registradores conhecidos são armazenados (timers com contagem
// derivada do relógio do host, INTC, DMAC, GS, SIO para saída de debug);
// iniciar uma transferência de DMA ou acessar um registrador desconhecido
// lança erro com o endereço e o nome. GIF/VIF/DMA de verdade: Fase 4.
class Hardware : public MmioDevice {
public:
    explicit Hardware(Runtime& rt);

    void read(std::uint32_t addr, void* out, unsigned size, std::uint32_t pc) override;
    void write(std::uint32_t addr, const void* in, unsigned size, std::uint32_t pc) override;

    void setGsCrt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field);
    void setIntcMask(std::uint32_t mask) { intcMask_ = mask; }

    // Nome do registrador (para mensagens), ou "" se desconhecido.
    static std::string registerName(std::uint32_t addr);

private:
    std::uint64_t read64(std::uint32_t addr, unsigned size, std::uint32_t pc);
    void write64(std::uint32_t addr, std::uint64_t value, unsigned size, std::uint32_t pc);
    std::uint32_t timerCount(unsigned timer) const;

    Runtime& rt_;
    std::map<std::uint32_t, std::uint64_t> regs_;
    std::uint32_t intcStat_ = 0;
    std::uint32_t intcMask_ = 0;
    std::string sioLine_;
};

}  // namespace anyps2::rt
