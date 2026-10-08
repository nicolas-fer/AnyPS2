#include "anyps2/runtime/iop/spu2.h"

#include <algorithm>
#include <cstring>

namespace anyps2::rt {

namespace {

// Coeficientes dos filtros de predição do ADPCM (formato público, o mesmo
// do PS1), em 1/64.
constexpr std::int32_t kFilter0[5] = {0, 60, 115, 98, 122};
constexpr std::int32_t kFilter1[5] = {0, 0, -52, -55, -60};

std::int32_t clamp16(std::int32_t v) {
    return std::clamp<std::int32_t>(v, -0x8000, 0x7FFF);
}

// Registrador de volume no modo fixo: bits 14-0 = volume/2 (com sinal).
std::int32_t fixedVolume(std::uint16_t reg) {
    const auto v = static_cast<std::int16_t>(static_cast<std::uint16_t>(reg << 1));
    return v;
}

// Catmull-Rom entre p1 e p2, t em 1/4096.
std::int32_t cubic(const std::array<std::int32_t, 4>& p, std::uint32_t t12) {
    const std::int64_t t = t12;
    const std::int64_t a = -p[0] + 3 * p[1] - 3 * p[2] + p[3];
    const std::int64_t b = 2 * p[0] - 5 * p[1] + 4 * p[2] - p[3];
    const std::int64_t c = -p[0] + p[2];
    const std::int64_t d = 2 * p[1];
    // ((a t³ + b t²) / 4096² + c t) / 4096 + d, tudo / 2
    const std::int64_t v = ((((a * t >> 12) + b) * t >> 12) + c) * t >> 12;
    return clamp16(static_cast<std::int32_t>((v + d) / 2));
}

}  // namespace

Spu2::Spu2() : ram_(kRamSize, 0) {}

void Spu2::reset() {
    std::fill(ram_.begin(), ram_.end(), std::uint8_t{0});
    voices_ = {};
}

bool Spu2::writeRam(std::uint32_t addr, const std::uint8_t* src, std::uint32_t size) {
    if (std::uint64_t{addr} + size > kRamSize) return false;
    std::memcpy(ram_.data() + addr, src, size);
    return true;
}

void Spu2::decodeBlock(const std::uint8_t* b, std::int16_t out[28], std::int32_t& s1, std::int32_t& s2) {
    std::uint32_t shift = b[0] & 0x0F;
    if (shift > 12) shift = 9;  // valores reservados se comportam como 9
    const std::uint32_t filter = std::min<std::uint32_t>((b[0] >> 4) & 0x07, 4);
    for (unsigned i = 0; i < 28; ++i) {
        const std::uint8_t byte = b[2 + i / 2];
        const auto nibble = static_cast<std::uint16_t>((i & 1) ? (byte >> 4) : (byte & 0x0F));
        const std::int32_t raw = static_cast<std::int16_t>(static_cast<std::uint16_t>(nibble << 12)) >> shift;
        const std::int32_t s = clamp16(raw + ((s1 * kFilter0[filter] + s2 * kFilter1[filter] + 32) >> 6));
        s2 = s1;
        s1 = s;
        out[i] = static_cast<std::int16_t>(s);
    }
}

void Spu2::keyOn(unsigned voice, const VoiceSetup& setup) {
    Voice v;
    v.phase = Phase::Attack;
    v.start = v.addr = v.loop = setup.start & ~15u;
    v.pitch = setup.pitch;
    v.volL = fixedVolume(setup.volL);
    v.volR = fixedVolume(setup.volR);
    v.adsr1 = setup.adsr1;
    v.adsr2 = setup.adsr2;
    voices_[voice] = v;
}

void Spu2::keyOff(unsigned voice) {
    Voice& v = voices_[voice];
    if (v.phase != Phase::Off) {
        v.phase = Phase::Release;
        v.envWait = 0;
    }
}

void Spu2::setVolume(unsigned voice, std::uint16_t volL, std::uint16_t volR) {
    voices_[voice].volL = fixedVolume(volL);
    voices_[voice].volR = fixedVolume(volR);
}

void Spu2::setPitch(unsigned voice, std::uint16_t pitch) {
    voices_[voice].pitch = pitch;
}

void Spu2::nextSample(Voice& v) {
    if (v.index >= 28) {
        if (v.stopAtBlockEnd) {
            // Bloco com "fim" sem "repetir": a voz para e fica muda.
            v.phase = Phase::Off;
            v.level = 0;
            return;
        }
        const std::uint8_t* b = ram_.data() + (v.addr & (kRamSize - 1));
        v.blockFlags = b[1];
        if (v.blockFlags & 4) v.loop = v.addr;  // início do loop
        decodeBlock(b, v.block, v.s1, v.s2);
        v.index = 0;
        if (v.blockFlags & 1) {
            // Fim do sample: com "repetir" volta ao loop; sem, encerra
            // depois de tocar este bloco.
            v.addr = v.loop;
            v.stopAtBlockEnd = (v.blockFlags & 2) == 0;
        } else {
            v.addr = (v.addr + 16) & (kRamSize - 1);
        }
    }
    v.hist = {v.hist[1], v.hist[2], v.hist[3], v.block[v.index++]};
}

// Envelope ADSR (passos e esperas como no SPU do PS1/PS2).
void Spu2::envelope(Voice& v) {
    bool exponential = false, decrease = false;
    std::int32_t shift = 0, step = 0;
    switch (v.phase) {
        case Phase::Attack:
            exponential = (v.adsr1 & 0x8000) != 0;
            shift = (v.adsr1 >> 10) & 0x1F;
            step = 7 - ((v.adsr1 >> 8) & 3);
            break;
        case Phase::Decay:
            exponential = true;
            decrease = true;
            shift = (v.adsr1 >> 4) & 0x0F;
            step = -8;
            break;
        case Phase::Sustain:
            exponential = (v.adsr2 & 0x8000) != 0;
            decrease = (v.adsr2 & 0x4000) != 0;
            shift = (v.adsr2 >> 8) & 0x1F;
            step = decrease ? -8 + ((v.adsr2 >> 6) & 3) : 7 - ((v.adsr2 >> 6) & 3);
            break;
        case Phase::Release:
            exponential = (v.adsr2 & 0x0020) != 0;
            decrease = true;
            shift = v.adsr2 & 0x1F;
            step = -8;
            break;
        case Phase::Off:
            return;
    }
    if (v.envWait > 0) {
        --v.envWait;
    } else {
        std::int32_t cycles = 1 << std::max(0, shift - 11);
        std::int32_t delta = step << std::max(0, 11 - shift);
        if (exponential && !decrease && v.level > 0x6000) cycles *= 4;
        if (exponential && decrease) delta = delta * v.level >> 15;
        v.level = std::clamp<std::int32_t>(v.level + delta, 0, 0x7FFF);
        v.envWait = cycles - 1;
    }
    const std::int32_t sustainLevel = ((v.adsr1 & 0x0F) + 1) * 0x800;
    switch (v.phase) {
        case Phase::Attack:
            if (v.level >= 0x7FFF) {
                v.phase = Phase::Decay;
                v.envWait = 0;
            }
            break;
        case Phase::Decay:
            if (v.level <= sustainLevel) {
                v.phase = Phase::Sustain;
                v.envWait = 0;
            }
            break;
        case Phase::Release:
            if (v.level == 0) v.phase = Phase::Off;
            break;
        default:
            break;
    }
}

void Spu2::render(std::int32_t* mix, std::size_t frames) {
    for (Voice& v : voices_) {
        if (v.phase == Phase::Off) continue;
        for (std::size_t f = 0; f < frames && v.phase != Phase::Off; ++f) {
            const std::int32_t s = cubic(v.hist, v.counter & 0xFFF);
            envelope(v);
            const std::int32_t out = s * v.level >> 15;
            mix[2 * f] += out * v.volL >> 15;
            mix[2 * f + 1] += out * v.volR >> 15;
            v.counter = (v.counter & 0xFFF) + std::min<std::uint32_t>(v.pitch, 0x3FFF);
            while (v.counter >= 0x1000 && v.phase != Phase::Off) {
                v.counter -= 0x1000;
                nextSample(v);
            }
        }
    }
}

}  // namespace anyps2::rt
