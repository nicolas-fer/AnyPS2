#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "anyps2/runtime/iop/spu2.h"

namespace anyps2::rt {

// Estado do driver de som da Polyphony (PDISPU2.IRX, "PDI_SPU2_Manager") em
// HLE: o EE mantém uma cópia dos registradores do SPU2 (960 bytes) e manda só
// o que mudou; este objeto traduz isso para o Spu2 do runtime. O protocolo
// completo está no topo de pdispu2.cpp. Separado do RPC para ser testável sem
// o IOP nem o disco do jogo.
class PdiSpu2 {
public:
    static constexpr unsigned kCores = 2;
    static constexpr unsigned kVoicesPerCore = 24;
    static constexpr std::uint32_t kCoreSize = 464;  // bytes de cada núcleo no bloco
    static constexpr std::uint32_t kBlockSize = 960;  // tamanho enviado pelo EE
    static constexpr std::uint32_t kStatusSize = kCores * (4 + 2 * kVoicesPerCore);  // 104

    explicit PdiSpu2(Spu2& spu) : spu_(spu) {}
    void setTrace(bool on) { trace_ = on; }  // key-on/key-off no stderr

    // Aplica um bloco de registradores (>= 928 bytes). Devolve o estado
    // (status()) logo após a aplicação, como o driver original.
    std::vector<std::uint8_t> apply(const std::uint8_t* block, std::size_t size);

    // Por núcleo: ENDX (u32, um bit por voz) e ENVX das 24 vozes (u16).
    std::vector<std::uint8_t> status() const;

private:
    struct VoiceRegs {
        std::uint32_t ssa = 0;
        std::uint16_t pitch = 0, volL = 0, volR = 0, adsr1 = 0, adsr2 = 0;
    };
    void warnOnce(unsigned bit, const char* text);
    void pushLive(unsigned voice);

    Spu2& spu_;
    std::array<VoiceRegs, kCores * kVoicesPerCore> regs_{};
    std::uint32_t warned_ = 0;
    bool trace_ = false;
};

}  // namespace anyps2::rt
