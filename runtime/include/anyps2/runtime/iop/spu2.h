#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace anyps2::rt {

// SPU2 em software: 2 MB de RAM de som e 48 vozes (2 núcleos × 24) que
// tocam ADPCM do PS2 (blocos de 16 bytes: shift/filtro, flags de loop e 28
// amostras de 4 bits), com envelope ADSR, pitch e volume por voz, mixadas em
// estéreo a 48 kHz.
//
// O que difere do hardware (documentado no README):
//  - a interpolação entre amostras é cúbica (Catmull-Rom); o chip usa uma
//    tabela gaussiana própria, que não copiamos;
//  - volume só no modo fixo (sem "sweep"), sem reverb, sem entrada de
//    disco/memória (CORE0/1 input) e sem efeitos de ruído/FM.
// Quem pede algo disso pelo HLE recebe erro claro.
class Spu2 {
public:
    static constexpr std::uint32_t kRamSize = 2u * 1024 * 1024;
    static constexpr unsigned kVoices = 48;
    static constexpr unsigned kRate = 48000;

    struct VoiceSetup {
        std::uint32_t start = 0;   // endereço em bytes na RAM do SPU2
        std::uint16_t pitch = 0x1000;  // 0x1000 = 48 kHz
        std::uint16_t volL = 0x3FFF, volR = 0x3FFF;  // registradores VOLL/VOLR (modo fixo)
        std::uint16_t adsr1 = 0, adsr2 = 0;
    };

    Spu2();
    void reset();

    std::uint8_t* ram() { return ram_.data(); }
    // false se [addr, addr+size) sai dos 2 MB.
    bool writeRam(std::uint32_t addr, const std::uint8_t* src, std::uint32_t size);

    void keyOn(unsigned voice, const VoiceSetup& setup);
    void keyOff(unsigned voice);  // entra em release
    void setVolume(unsigned voice, std::uint16_t volL, std::uint16_t volR);
    void setPitch(unsigned voice, std::uint16_t pitch);
    void setEnvelope(unsigned voice, std::uint16_t adsr1, std::uint16_t adsr2);  // vale já na voz tocando
    // A voz ainda produz som (não chegou ao fim do sample nem do release).
    bool active(unsigned voice) const { return voices_[voice].phase != Phase::Off; }
    std::uint32_t startAddress(unsigned voice) const { return voices_[voice].start; }
    // Nível do envelope (registrador ENVX, 0..0x7FFF) e o "fim do sample"
    // (bit do ENDX: a voz leu um bloco com a flag de fim desde o último keyOn).
    std::uint16_t envelopeLevel(unsigned voice) const { return static_cast<std::uint16_t>(voices_[voice].level); }
    bool reachedEnd(unsigned voice) const { return voices_[voice].ended; }

    // Soma a saída das vozes em `mix` (frames × 2, L/R intercalados).
    void render(std::int32_t* mix, std::size_t frames);

    // Decodifica um bloco ADPCM (16 bytes) em 28 amostras; s1/s2 = histórico
    // do filtro (atualizado). Exposto para os testes.
    static void decodeBlock(const std::uint8_t* block, std::int16_t out[28], std::int32_t& s1, std::int32_t& s2);

private:
    enum class Phase : std::uint8_t { Off, Attack, Decay, Sustain, Release };
    struct Voice {
        Phase phase = Phase::Off;
        std::uint32_t start = 0, addr = 0, loop = 0;
        std::uint16_t pitch = 0x1000;
        std::int32_t volL = 0, volR = 0;  // efetivos, -0x8000..0x7FFE
        std::uint16_t adsr1 = 0, adsr2 = 0;
        std::int32_t level = 0;      // envelope 0..0x7FFF
        std::int32_t envWait = 0;    // amostras até o próximo passo do envelope
        std::uint32_t counter = 0;   // fração de amostra (12 bits)
        std::int16_t block[28] = {};
        unsigned index = 28;          // próxima amostra do bloco (28 = decodificar)
        std::uint8_t blockFlags = 0;
        std::int32_t s1 = 0, s2 = 0;  // histórico do filtro ADPCM
        std::array<std::int32_t, 4> hist{};  // últimas amostras (interpolação)
        bool stopAtBlockEnd = false;
        bool ended = false;           // leu um bloco com a flag de fim
    };
    void nextSample(Voice& v);
    void envelope(Voice& v);

    std::vector<std::uint8_t> ram_;
    std::array<Voice, kVoices> voices_{};
};

}  // namespace anyps2::rt
