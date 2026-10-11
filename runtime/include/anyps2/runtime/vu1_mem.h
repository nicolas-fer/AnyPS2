#pragma once

#include <cstdint>
#include <cstring>

#include "anyps2/runtime/host_profile.h"
#include "anyps2/runtime/memory.h"
#include "anyps2/runtime/vif.h"

namespace anyps2::rt {

// Memória do VU1 (micro 0x1100_8000 e dados 0x1100_C000, 16 KB cada) vista pelo EE como um
// dispositivo, não como RAM direta nas páginas do Memory. Os bytes são os mesmos buffers do
// VuMemory (o VU, o VIF e o XGKICK continuam usando-os direto); só o caminho do EE passa
// por aqui, e todo acesso do EE chama antes o gancho de fence. Hoje o gancho é um no-op:
// ele marca o ponto em que, com o VU1 em outra thread, o EE esperaria o VU1 chegar ao
// estado do EE (plano do VU1 em outra thread, fatia 2). Também conta os acessos
// (ANYPS2_VU1_MEMCOUNT=1), que é a medição "vu1 sync" da fatia 0.
//
// O acesso do EE entra por Memory (mapVirtualDevice): leitura/escrita escalar e de 128
// bits, copyFromGuest/copyToGuest e readCString. O endereço chega sem o espelho kseg1
// (0xA000_0000), já em 0x1100_8000–0x1100_FFFF. Cada metade é registrada como uma faixa
// própria, então um acesso em bloco nunca cruza do micro para os dados (como antes, quando
// as duas memórias não eram contíguas no host).
class Vu1Memory : public MmioDevice {
public:
    static constexpr std::uint32_t kMicroBase = 0x11008000u;
    static constexpr std::uint32_t kDataBase = 0x1100C000u;
    static constexpr std::uint32_t kHalfSize = VuMemory::kVu1Size;
    using Fence = void (*)(void* ctx);

    Vu1Memory(std::uint8_t* micro, std::uint8_t* data) : micro_(micro), data_(data) {}

    // Registra as duas metades (e o espelho kseg1) no Memory.
    void attach(Memory& mem) {
        mem.mapVirtualDevice(kMicroBase, kHalfSize, this);
        mem.mapVirtualDevice(kDataBase, kHalfSize, this);
    }
    // Gancho chamado antes de cada acesso do EE; nulo = nada.
    void setFence(Fence fence, void* ctx) {
        fence_ = fence;
        fenceCtx_ = ctx;
    }
    // Conta leituras e escritas do EE no perfil (SyncStats::vu1MemReads/Writes).
    void setCounting(bool on) { counting_ = on; }

    // `size` é 1, 2, 4, 8 ou 16 (acesso escalar) ou qualquer tamanho que caiba numa das
    // metades (cópia em bloco do HLE).
    void read(std::uint32_t addr, void* out, unsigned size, std::uint32_t) override {
        if (fence_) fence_(fenceCtx_);
        if (counting_) HostProfile::count(&SyncStats::vu1MemReads);
        std::memcpy(out, host(addr), size);
    }
    void write(std::uint32_t addr, const void* in, unsigned size, std::uint32_t) override {
        if (fence_) fence_(fenceCtx_);
        if (counting_) HostProfile::count(&SyncStats::vu1MemWrites);
        std::memcpy(host(addr), in, size);
    }

private:
    // Memory só entrega acessos que cabem inteiros em uma das metades.
    std::uint8_t* host(std::uint32_t addr) const {
        return addr >= kDataBase ? data_ + (addr - kDataBase) : micro_ + (addr - kMicroBase);
    }
    std::uint8_t* micro_;
    std::uint8_t* data_;
    Fence fence_ = nullptr;
    void* fenceCtx_ = nullptr;
    bool counting_ = false;
};

}  // namespace anyps2::rt
