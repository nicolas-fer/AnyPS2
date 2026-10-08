#include "anyps2/vu/scan.h"

#include <algorithm>
#include <cstring>

#include "anyps2/vu/isa.h"

namespace anyps2::vu {

namespace {

constexpr std::uint32_t kMicroMax = 0x4000;  // micro memória do VU1 (16 KB)
constexpr std::uint32_t kEBit = 1u << 30;

std::uint32_t read32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

bool isZeroPair(const std::uint8_t* p) {
    return read32(p) == 0 && read32(p + 4) == 0;
}

// Contém um bit E com o delay slot dentro do bloco: um microprograma termina.
bool hasEnd(const std::vector<std::uint32_t>& w) {
    for (std::size_t i = 1; i + 2 < w.size(); i += 2) {
        if (w[i] & kEBit) return true;
    }
    return false;
}

// Nos blocos crus (sem MPG) exige-se também que o par faça algo: uma escrita
// com máscara xyzw vazia é legal mas nenhum montador gera, e é o padrão
// típico de constantes double/float lidas como instruções.
bool meaningful(std::uint32_t lowerWord, std::uint32_t upperWord) {
    if (!plausiblePair(lowerWord, upperWord)) return false;
    const Instr in = decode(lowerWord, upperWord);
    if (in.upper.op != U::NOP && in.upper.op != U::CLIP && in.upper.dest == 0) return false;
    if (in.i) return true;
    switch (in.lower.op) {
        case L::LQ: case L::SQ: case L::LQI: case L::SQI: case L::LQD: case L::SQD: case L::MOVE: case L::MR32:
        case L::MFIR: case L::ILW: case L::ISW: case L::ILWR: case L::ISWR: case L::RGET: case L::RNEXT:
        case L::MFP:
            return in.lower.dest != 0;
        default:
            return true;
    }
}

// Bit E no delay slot de outro bit E: não é um microprograma.
bool doubleEnd(const std::vector<std::uint32_t>& w) {
    for (std::size_t i = 1; i + 2 < w.size(); i += 2) {
        if ((w[i] & kEBit) && (w[i + 2] & kEBit)) return true;
    }
    return false;
}

struct Mpg {
    std::size_t vifcode;   // posição do VIFcode
    std::size_t data;      // primeiro par
    std::uint32_t pairs;
    std::uint32_t load;    // bytes
};

}  // namespace

bool plausiblePair(std::uint32_t lowerWord, std::uint32_t upperWord) {
    const Instr in = decode(lowerWord, upperWord);
    if (in.d || in.t) return false;
    if (in.upper.op == U::INVALID || !in.upper.canonical) return false;
    if (in.i) return true;  // lower é o imediato do LOI
    return in.lower.op != L::INVALID && in.lower.canonical;
}

std::vector<MicroBlob> findMicrocode(const std::uint8_t* data, std::size_t size, std::uint32_t address) {
    std::vector<MicroBlob> out;
    if (size < 16) return out;
    auto pairOk = [&](std::size_t at) { return plausiblePair(read32(data + at), read32(data + at + 4)); };
    auto rawOk = [&](std::size_t at) { return meaningful(read32(data + at), read32(data + at + 4)); };

    // ---- Pacotes MPG ---------------------------------------------------------
    // O VIFcode fica numa palavra ímpar (o código precisa começar alinhado a
    // 64 bits) e é seguido por NUM pares (NUM = 0 → 256).
    std::vector<Mpg> mpgs;
    const std::size_t first = ((address + 4) & 7u) == 0 ? 0 : 4 - (address & 3u);
    for (std::size_t p = first; p + 4 <= size; p += 4) {
        if (((address + p + 4) & 7u) != 0) continue;
        const std::uint32_t w = read32(data + p);
        if (((w >> 24) & 0x7F) != 0x4A) continue;
        std::uint32_t num = (w >> 16) & 0xFF;
        if (num == 0) num = 256;
        const std::uint32_t load = (w & 0xFFFF) * 8;
        const std::size_t start = p + 4;
        if (start + std::size_t{num} * 8 > size || load + num * 8 > kMicroMax) continue;
        bool ok = true;
        bool nonZero = false;
        for (std::uint32_t i = 0; i < num && ok; ++i) {
            ok = pairOk(start + i * 8);
            nonZero = nonZero || !isZeroPair(data + start + i * 8);
        }
        if (!ok || !nonZero) continue;
        mpgs.push_back({p, start, num, load});
        p = start + std::size_t{num} * 8 - 4;  // continua depois do código
    }
    // Programas maiores que 256 pares vêm em vários MPG seguidos (separados no
    // máximo por alguns VIFcodes como NOP/FLUSHE) com endereços contíguos.
    std::vector<std::pair<std::size_t, std::size_t>> mpgRanges;
    for (std::size_t i = 0; i < mpgs.size();) {
        MicroBlob b;
        b.address = static_cast<std::uint32_t>(address + mpgs[i].data);
        b.loadAddress = mpgs[i].load;
        b.fromMpg = true;
        std::size_t j = i;
        for (;;) {
            const Mpg& m = mpgs[j];
            for (std::uint32_t k = 0; k < m.pairs * 2; ++k) b.words.push_back(read32(data + m.data + k * 4));
            mpgRanges.emplace_back(m.vifcode, m.data + std::size_t{m.pairs} * 8);
            const std::size_t end = m.data + std::size_t{m.pairs} * 8;
            if (j + 1 < mpgs.size() && mpgs[j + 1].vifcode - end <= 12 &&
                mpgs[j + 1].load == m.load + m.pairs * 8) {
                ++j;
                continue;
            }
            break;
        }
        i = j + 1;
        appendUnique(out, {std::move(b)});
    }

    // ---- Blocos crus -----------------------------------------------------------
    auto insideMpg = [&](std::size_t at) {
        for (const auto& r : mpgRanges) {
            if (at + 8 > r.first && at < r.second) return true;
        }
        return false;
    };
    const std::size_t firstPair = (8 - (address & 7u)) & 7u;
    std::size_t p = firstPair;
    while (p + 8 <= size) {
        if (insideMpg(p) || !rawOk(p)) {
            p += 8;
            continue;
        }
        std::size_t end = p;
        while (end + 8 <= size && !insideMpg(end) && rawOk(end)) end += 8;
        std::size_t s = p, e = end;
        while (s < e && isZeroPair(data + s)) s += 8;
        while (e > s && isZeroPair(data + e - 8)) e -= 8;
        if (e - s >= 16) {
            MicroBlob b;
            b.address = static_cast<std::uint32_t>(address + s);
            for (std::size_t k = s; k < e; k += 4) b.words.push_back(read32(data + k));
            if (hasEnd(b.words) && !doubleEnd(b.words)) appendUnique(out, {std::move(b)});
        }
        p = end;
    }
    return out;
}

void appendUnique(std::vector<MicroBlob>& out, std::vector<MicroBlob> more) {
    for (auto& b : more) {
        auto same = std::find_if(out.begin(), out.end(), [&](const MicroBlob& o) { return o.words == b.words; });
        if (same == out.end()) {
            out.push_back(std::move(b));
        } else if (same->loadAddress == kUnknownLoad && b.loadAddress != kUnknownLoad) {
            *same = std::move(b);
        }
    }
}

}  // namespace anyps2::vu
