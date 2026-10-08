#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "anyps2/runtime/context.h"

namespace anyps2::rt {

// Tratador de uma faixa de I/O mapeada em memória (registradores de hardware).
class MmioDevice {
public:
    virtual ~MmioDevice() = default;
    // size ∈ {1, 2, 4, 8, 16}. Leituras/escritas não tratadas devem lançar
    // Unimplemented com o endereço e o nome do registrador.
    virtual void read(std::uint32_t addr, void* out, unsigned size, std::uint32_t pc) = 0;
    virtual void write(std::uint32_t addr, const void* in, unsigned size, std::uint32_t pc) = 0;
};

// Mapa de memória do EE:
//   0x0000_0000–0x01FF_FFFF  RDRAM (32 MB), espelhada em 0x2000_0000
//                            (uncached), 0x3000_0000 (uncached acelerado),
//                            0x8000_0000 (kseg0) e 0xA000_0000 (kseg1)
//   0x7000_0000–0x7000_3FFF  scratchpad (16 KB)
//   0x1000_0000–0x1000_FFFF  registradores do EE (DMAC, INTC, timers, VIF, GIF, SIF...)
//   0x1200_0000–0x1200_1FFF  registradores privilegiados do GS
// O resto não é mapeado: qualquer acesso lança erro com endereço e PC.
class Memory {
public:
    static constexpr std::uint32_t kRamSize = 32u * 1024 * 1024;
    static constexpr std::uint32_t kScratchpadBase = 0x70000000u;
    static constexpr std::uint32_t kScratchpadSize = 16u * 1024;
    static constexpr unsigned kPageBits = 12;
    static constexpr std::uint32_t kPageMask = (1u << kPageBits) - 1;

    Memory();
    ~Memory();
    Memory(const Memory&) = delete;
    Memory& operator=(const Memory&) = delete;

    // Registra um dispositivo para [base, base+size). Faixas não podem se sobrepor.
    void mapDevice(std::uint32_t base, std::uint32_t size, MmioDevice* device);
    // Mapeia memória comum do host (ex.: memórias dos VUs) em [vbase, vbase+size)
    // e no espelho uncached (vbase | 0xA000_0000 quando aplicável).
    void mapRam(std::uint32_t vbase, std::uint8_t* host, std::uint32_t size);

    std::uint8_t* ram() { return ram_.get(); }
    std::uint8_t* scratchpad() { return spr_.get(); }

    // Ponteiro do host para [addr, addr+size) se estiver inteiramente em RAM
    // ou scratchpad (mesma página não é exigida para RAM contígua).
    std::uint8_t* hostPointer(std::uint32_t addr, std::uint32_t size);

    // ---- Acesso rápido usado pelo código gerado ----------------------------
    template <typename T>
    T read(std::uint32_t addr, std::uint32_t pc) {
        std::uint8_t* page = pages_[addr >> kPageBits];
        if (page && (addr & (sizeof(T) - 1)) == 0) [[likely]] {
            T v;
            std::memcpy(&v, page + (addr & kPageMask), sizeof(T));
            return v;
        }
        T v;
        slowRead(addr, &v, sizeof(T), pc);
        return v;
    }

    template <typename T>
    void write(std::uint32_t addr, T value, std::uint32_t pc) {
        std::uint8_t* page = pages_[addr >> kPageBits];
        if (page && (addr & (sizeof(T) - 1)) == 0) [[likely]] {
            std::memcpy(page + (addr & kPageMask), &value, sizeof(T));
            return;
        }
        slowWrite(addr, &value, sizeof(T), pc);
    }

    // 128 bits (LQ/SQ/LQC2/SQC2): o endereço já vem alinhado a 16.
    void read128(std::uint32_t addr, Reg128& out, std::uint32_t pc) {
        std::uint8_t* page = pages_[addr >> kPageBits];
        if (page) [[likely]] {
            std::memcpy(&out, page + (addr & kPageMask), 16);
            return;
        }
        slowRead(addr, &out, 16, pc);
    }
    void write128(std::uint32_t addr, const Reg128& in, std::uint32_t pc) {
        std::uint8_t* page = pages_[addr >> kPageBits];
        if (page) [[likely]] {
            std::memcpy(page + (addr & kPageMask), &in, 16);
            return;
        }
        slowWrite(addr, &in, 16, pc);
    }

    // Utilitários para o runtime (HLE): leem/escrevem sem exigir alinhamento.
    void copyFromGuest(void* dst, std::uint32_t addr, std::uint32_t size, std::uint32_t pc);
    void copyToGuest(std::uint32_t addr, const void* src, std::uint32_t size, std::uint32_t pc);
    std::string readCString(std::uint32_t addr, std::uint32_t maxLen, std::uint32_t pc);

    // Converte endereço virtual em físico (remove espelhos de kseg/uncached).
    static std::uint32_t physical(std::uint32_t addr);

private:
    void slowRead(std::uint32_t addr, void* out, unsigned size, std::uint32_t pc);
    void slowWrite(std::uint32_t addr, const void* in, unsigned size, std::uint32_t pc);
    MmioDevice* deviceFor(std::uint32_t addr, unsigned size) const;
    void mapPages(std::uint32_t vbase, std::uint8_t* host, std::uint32_t size);

    struct DeviceRange {
        std::uint32_t base;
        std::uint32_t size;
        MmioDevice* device;
    };

    std::unique_ptr<std::uint8_t[]> ram_;
    std::unique_ptr<std::uint8_t[]> spr_;
    std::unique_ptr<std::uint8_t*[]> pages_;  // 2^20 entradas (páginas de 4 KB)
    std::vector<DeviceRange> devices_;
};

}  // namespace anyps2::rt
