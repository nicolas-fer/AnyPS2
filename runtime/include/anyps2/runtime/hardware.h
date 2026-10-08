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
// Encaminha cada faixa para o seu dispositivo: timers/VBlank (Timing), INTC
// (Kernel), DMAC, GIF, VIF0/VIF1 e seus FIFOs, SIF (Iop), GS. Registradores
// conhecidos sem efeito emulado são apenas armazenados; acessar um
// registrador desconhecido lança erro com o endereço e o PC.
class Hardware : public MmioDevice {
public:
    explicit Hardware(Runtime& rt);

    void read(std::uint32_t addr, void* out, unsigned size, std::uint32_t pc) override;
    void write(std::uint32_t addr, const void* in, unsigned size, std::uint32_t pc) override;

    // Nome do registrador (para mensagens), ou "" se desconhecido.
    static std::string registerName(std::uint32_t addr);

private:
    std::uint64_t read64(std::uint32_t addr, unsigned size, std::uint32_t pc);
    void write64(std::uint32_t addr, std::uint64_t value, unsigned size, std::uint32_t pc);

    Runtime& rt_;
    std::map<std::uint32_t, std::uint64_t> regs_;
    std::string sioLine_;
};

}  // namespace anyps2::rt
