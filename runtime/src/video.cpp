#include "anyps2/runtime/video.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <vector>

#include "anyps2/common/error.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

namespace {

class HeadlessVideo final : public Video {
public:
    void present(const gs::Frame&) override {}
    bool pollEvents() override { return true; }
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t n, std::uint32_t crc = 0) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

void put32be(std::vector<std::uint8_t>& v, std::uint32_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 24));
    v.push_back(static_cast<std::uint8_t>(x >> 16));
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}

void chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
    put32be(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put32be(out, crc32(body.data(), body.size()));
}

}  // namespace

namespace {

// Compressor deflate mínimo e determinístico: LZ77 guloso com cadeias de
// hash (janela de 32 KB) e códigos de Huffman fixos (RFC 1951, seção 3.2.6).
class Deflater {
public:
    std::vector<std::uint8_t> compress(const std::vector<std::uint8_t>& in) {
        out_.clear();
        bitBuf_ = 0;
        bitCount_ = 0;
        putBits(1, 1);  // BFINAL
        putBits(1, 2);  // BTYPE = 01 (Huffman fixo)
        constexpr std::size_t kWindow = 32768, kHashSize = 1 << 15, kMaxChain = 48;
        std::vector<std::int32_t> head(kHashSize, -1), prev(in.size(), -1);
        auto hash = [&](std::size_t i) {
            return static_cast<std::size_t>((in[i] << 10) ^ (in[i + 1] << 5) ^ in[i + 2]) & (kHashSize - 1);
        };
        std::size_t i = 0;
        while (i < in.size()) {
            std::size_t bestLen = 0, bestDist = 0;
            if (i + 3 <= in.size()) {
                const std::size_t h = hash(i);
                std::int32_t cand = head[h];
                for (std::size_t chain = 0; cand >= 0 && chain < kMaxChain; ++chain) {
                    const auto c = static_cast<std::size_t>(cand);
                    if (i - c > kWindow) break;
                    std::size_t len = 0;
                    const std::size_t maxLen = std::min<std::size_t>(258, in.size() - i);
                    while (len < maxLen && in[c + len] == in[i + len]) ++len;
                    if (len > bestLen) {
                        bestLen = len;
                        bestDist = i - c;
                        if (len == maxLen) break;
                    }
                    cand = prev[c];
                }
            }
            const std::size_t step = bestLen >= 3 ? bestLen : 1;
            if (bestLen >= 3) {
                putLength(bestLen);
                putDistance(bestDist);
            } else {
                putLiteral(in[i]);
            }
            for (std::size_t k = 0; k < step; ++k, ++i) {
                if (i + 3 <= in.size()) {
                    const std::size_t h = hash(i);
                    prev[i] = head[h];
                    head[h] = static_cast<std::int32_t>(i);
                }
            }
        }
        putSymbol(256);  // fim do bloco
        if (bitCount_ > 0) out_.push_back(static_cast<std::uint8_t>(bitBuf_));
        return out_;
    }

private:
    void putBits(std::uint32_t value, unsigned n) {  // LSB primeiro
        bitBuf_ |= value << bitCount_;
        bitCount_ += n;
        while (bitCount_ >= 8) {
            out_.push_back(static_cast<std::uint8_t>(bitBuf_));
            bitBuf_ >>= 8;
            bitCount_ -= 8;
        }
    }
    void putCode(std::uint32_t code, unsigned n) {  // códigos de Huffman: MSB primeiro
        std::uint32_t rev = 0;
        for (unsigned k = 0; k < n; ++k) rev |= ((code >> k) & 1u) << (n - 1 - k);
        putBits(rev, n);
    }
    void putSymbol(unsigned sym) {
        if (sym < 144) putCode(0x30 + sym, 8);
        else if (sym < 256) putCode(0x190 + (sym - 144), 9);
        else if (sym < 280) putCode(sym - 256, 7);
        else putCode(0xC0 + (sym - 280), 8);
    }
    void putLiteral(std::uint8_t b) { putSymbol(b); }
    void putLength(std::size_t len) {
        static constexpr std::uint16_t kBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr std::uint8_t kExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        unsigned c = 28;
        while (kBase[c] > len) --c;
        putSymbol(257 + c);
        putBits(static_cast<std::uint32_t>(len - kBase[c]), kExtra[c]);
    }
    void putDistance(std::size_t dist) {
        static constexpr std::uint16_t kBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                    33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr std::uint8_t kExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        unsigned c = 29;
        while (kBase[c] > dist) --c;
        putCode(c, 5);
        putBits(static_cast<std::uint32_t>(dist - kBase[c]), kExtra[c]);
    }

    std::vector<std::uint8_t> out_;
    std::uint32_t bitBuf_ = 0;
    unsigned bitCount_ = 0;
};

}  // namespace

void writePng(const std::string& path, const gs::Frame& f) {
    std::vector<std::uint8_t> raw;
    raw.reserve((std::size_t{f.width} * 3 + 1) * f.height);
    for (std::uint32_t y = 0; y < f.height; ++y) {
        raw.push_back(1);  // filtro "Sub": diferença para o pixel à esquerda
        std::uint32_t left = 0;
        for (std::uint32_t x = 0; x < f.width; ++x) {
            const std::uint32_t p = f.pixels[std::size_t{y} * f.width + x];
            for (unsigned s = 0; s < 24; s += 8) {
                raw.push_back(static_cast<std::uint8_t>(((p >> s) - (left >> s)) & 0xFF));
            }
            left = p;
        }
    }
    std::vector<std::uint8_t> z = {0x78, 0x01};
    const std::vector<std::uint8_t> deflated = Deflater().compress(raw);
    z.insert(z.end(), deflated.begin(), deflated.end());
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32be(z, (b << 16) | a);

    std::vector<std::uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> ihdr;
    put32be(ihdr, f.width);
    put32be(ihdr, f.height);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8 bits, RGB
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    std::ofstream out(path, std::ios::binary);
    if (!out) throw anyps2::Error("não foi possível gravar '" + path + "'");
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
}

std::unique_ptr<Video> createVideo(const RuntimeOptions& options, const std::string& title, Input* input) {
    if (options.video == "none") return std::make_unique<HeadlessVideo>();
    if (!options.video.empty() && options.video != "sdl") {
        throw anyps2::Error("ANYPS2_VIDEO='" + options.video + "' inválido (use sdl ou none)");
    }
    std::string error;
    if (auto v = createSdlVideo(title, options.video.empty(), input, error)) return v;
    if (options.video == "sdl") throw anyps2::Error("não foi possível abrir a janela: " + error);
    return std::make_unique<HeadlessVideo>();
}

}  // namespace anyps2::rt
