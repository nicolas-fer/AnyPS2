#include "gsplay.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include "anyps2/common/error.h"
#include "anyps2/runtime/gs/vram.h"
#include "anyps2/runtime/video.h"

namespace anyps2::gsplay {

using namespace rt;
using namespace rt::gs;

namespace {

constexpr std::size_t kPrivBytes = 8192;
constexpr std::size_t kMaxMessages = 8;

std::uint32_t rd32(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::uint32_t v;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

std::uint64_t rd64(const std::uint8_t* p) {
    std::uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

}  // namespace

Dump parseDump(std::vector<std::uint8_t> bytes) {
    if (bytes.size() < 8 + 36) throw Error("GS dump: arquivo pequeno demais");
    Dump d;
    d.bytes = std::move(bytes);
    const auto& b = d.bytes;
    if (rd32(b, 0) != 0xFFFFFFFFu) {
        throw Error("GS dump: assinatura 0xFFFFFFFF ausente (formato antigo ou arquivo compactado em .zst?)");
    }
    const std::size_t hsize = rd32(b, 4);
    if (hsize < 36 || 8 + hsize > b.size()) throw Error("GS dump: cabeçalho inválido");
    d.stateVersion = rd32(b, 8);
    d.stateSize = rd32(b, 12);
    const std::size_t serialOff = rd32(b, 16), serialSize = rd32(b, 20);
    d.crc = rd32(b, 24);
    if (d.stateVersion != 9) {
        throw Error("GS dump: state_version " + std::to_string(d.stateVersion) + " não suportada (só a 9)");
    }
    if (serialSize && serialOff + serialSize <= hsize) {
        d.serial.assign(reinterpret_cast<const char*>(b.data()) + 8 + serialOff, serialSize);
        while (!d.serial.empty() && d.serial.back() == '\0') d.serial.pop_back();
    }
    d.stateOffset = 8 + hsize;
    if (d.stateSize < kStateVramOffset + Vram::kSize) {
        throw Error("GS dump: estado de " + std::to_string(d.stateSize) + " bytes não cobre a VRAM de 4 MB");
    }
    d.privOffset = d.stateOffset + d.stateSize;
    d.packetsOffset = d.privOffset + kPrivBytes;
    if (d.packetsOffset > b.size()) throw Error("GS dump: arquivo truncado antes dos registradores privilegiados");
    return d;
}

Dump loadDump(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("não foi possível abrir '" + path + "'");
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return parseDump(std::move(bytes));
}

Player::Player(const Dump& dump)
    : dump_(dump), gs_(std::make_unique<Gs>(nullptr)), gif_(std::make_unique<Gif>(nullptr, *gs_)) {
    restoreState();
}

void Player::restoreState() {
    const std::uint8_t* s = dump_.bytes.data() + dump_.stateOffset;
    std::size_t at = 4;  // a versão do próprio estado
    auto next = [&]() {
        const std::uint64_t v = rd64(s + at);
        at += 8;
        return v;
    };
    Gs& g = *gs_;
    auto w = [&](std::uint8_t reg, std::uint64_t v) { g.writeRegister(reg, v, 0); };

    // VRAM direto (Gs::vram() espera as faixas).
    std::memcpy(g.vram().data(), s + kStateVramOffset, Vram::kSize);

    // Ambiente. PRIM primeiro (zera a fila de vértices). TRXDIR não é escrito:
    // dispararia uma transferência; o segundo TRXREG (obsoleto) é descartado.
    constexpr std::uint8_t kSkip = 0xFF;
    const std::uint8_t env[16] = {PRIM,  PRMODE, PRMODECONT, TEXCLUT,   SCANMSK, TEXA, FOGCOL, DIMX,
                                  DTHE,  COLCLAMP, PABE,     BITBLTBUF, kSkip,   TRXPOS, TRXREG, kSkip};
    for (std::uint8_t reg : env) {
        const std::uint64_t v = next();
        if (reg == kSkip) continue;
        w(reg, reg == PRIM ? (v & 0x7FF) : v);
    }
    // Contextos 1 e 2. TEX0 sem CLD: o buffer da CLUT não está no estado, e uma
    // carga agora leria uma CLUT de um TEX0 que talvez nem seja a da CLUT atual.
    for (unsigned c = 0; c < 2; ++c) {
        const auto r = [c](std::uint8_t base) { return static_cast<std::uint8_t>(base + c); };
        const std::uint8_t regs[12] = {r(XYOFFSET_1), r(TEX0_1),    r(TEX1_1), r(CLAMP_1),  r(MIPTBP1_1), r(MIPTBP2_1),
                                       r(SCISSOR_1),  r(ALPHA_1),   r(TEST_1), r(FBA_1),    r(FRAME_1),   r(ZBUF_1)};
        for (std::uint8_t reg : regs) {
            std::uint64_t v = next();
            if (reg == TEX0_1 || reg == TEX0_2) v &= ~(std::uint64_t{7} << 61);
            w(reg, v);
        }
    }
    // Vértice corrente: RGBAQ, ST, UV, FOG. O XYZ não é escrito (iria para a fila).
    const std::uint64_t rgbaq = next(), st = next(), uv = next(), fog = next();
    w(RGBAQ, rgbaq);
    w(ST, st);
    w(UV, uv);
    w(FOG, fog);

    // Registradores privilegiados (PMODE..BGCOLOR); CSR/IMR/BUSDIR/SIGLBLID ficam
    // como estão (escrever no CSR limparia eventos).
    const std::uint8_t* p = dump_.bytes.data() + dump_.privOffset;
    for (std::uint32_t off = 0; off < 0xF0; off += 0x10) {
        g.writePrivileged(0x12000000u + off, rd64(p + off), 0);
    }
    g.waitIdle();
}

Stats Player::run(const VsyncFn& onVsync) {
    Stats st;
    const auto& b = dump_.bytes;
    std::size_t at = dump_.packetsOffset;
    auto need = [&](std::size_t n) {
        if (n > b.size() - at) throw Error("GS dump: pacote truncado em " + std::to_string(at));
    };
    bool stop = false;
    while (at < b.size() && !stop) {
        const std::uint8_t id = b[at++];
        ++st.packets;
        switch (id) {
            case 0: {  // Transfer
                need(5);
                const unsigned path = b[at];
                const std::size_t size = rd32(b, at + 1);
                at += 5;
                need(size);
                const std::uint8_t* data = b.data() + at;
                at += size;
                if (path > 3) break;  // 4 = falso: não vai ao GIF
                // 0 = PATH1 antigo (o final de um buffer de 16 KB, mas os dados já
                // vêm do começo do pacote), 1 = PATH2, 2 = PATH3, 3 = PATH1 novo.
                const unsigned gifPath = path == 0 || path == 3 ? 1u : path + 1;
                ++st.transfers;
                try {
                    gif_->transfer(gifPath, data, size / 16, 0);
                } catch (const Error& e) {
                    ++st.errors;
                    if (st.messages.size() < kMaxMessages) {
                        st.messages.push_back("pacote " + std::to_string(st.packets) + " (PATH" +
                                              std::to_string(gifPath) + "): " + e.what());
                    }
                    gif_->reset();
                }
                break;
            }
            case 1:  // VSync
                need(1);
                ++at;
                ++st.vsyncs;
                gs_->vblankStart();
                if (onVsync && !onVsync(st.vsyncs, *gs_)) stop = true;
                break;
            case 2:  // ReadFIFO2: download LOCAL->HOST, descartado
                need(4);
                at += 4;
                ++st.readFifos;
                break;
            case 3:  // Registers
                need(kPrivBytes);
                for (std::uint32_t off = 0; off < 0xF0; off += 0x10) {
                    gs_->writePrivileged(0x12000000u + off, rd64(b.data() + at + off), 0);
                }
                at += kPrivBytes;
                ++st.registers;
                break;
            default:
                throw Error("GS dump: pacote de tipo " + std::to_string(id) + " desconhecido em " +
                            std::to_string(at - 1));
        }
    }
    gs_->waitIdle();
    st.draws = gs_->drawCount();
    return st;
}

Frame readBuffer(Gs& gs, std::uint32_t fbp, std::uint32_t fbw, std::uint32_t psm, std::uint32_t width,
                 std::uint32_t height) {
    if (!isValidPsm(psm)) throw Error("PSM inválido " + hex(psm, 2));
    Frame f;
    f.width = width;
    f.height = height;
    f.pixels.resize(std::size_t{width} * height);
    Vram& v = gs.vram();
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t raw = v.readPixel(psm, fbp * 32, fbw, x, y);
            std::uint32_t out;
            switch (psm) {
                case PSMCT32: case PSMCT24: out = raw & 0xFFFFFF; break;
                case PSMCT16: case PSMCT16S:
                    out = ((raw & 0x1F) << 3) | (((raw >> 5) & 0x1F) << 11) | (((raw >> 10) & 0x1F) << 19);
                    break;
                case PSMZ32: case PSMZ24: case PSMZ16: case PSMZ16S: {
                    const std::uint32_t top = psm == PSMZ32 ? raw >> 24 : psm == PSMZ24 ? raw >> 16 : raw >> 8;
                    out = (top & 0xFF) * 0x010101u;
                    break;
                }
                default: out = (raw & 0xFF) * 0x010101u; break;  // índices: tons de cinza
            }
            f.pixels[std::size_t{y} * width + x] = out | 0xFF000000u;
        }
    }
    return f;
}

namespace {

void usage() {
    std::fputs(
        "uso: anyps2_gsplay dump.gs [--out prefixo] [--frames N|all] [--vsync K] [--fb FBP,FBW,PSM,W,H]\n"
        "  Reproduz um GS dump do PCSX2 no GS em software e grava prefixo_K.png a cada VSync\n"
        "  (K=1 é o primeiro; --vsync 0 grava o estado restaurado, antes dos pacotes).\n"
        "  --out     prefixo dos arquivos (padrão: o caminho do dump sem a extensão)\n"
        "  --frames  só os N primeiros VSyncs (padrão: all)\n"
        "  --vsync   só o K-ésimo VSync\n"
        "  --fb      no fim grava prefixo_fb.png com o buffer da VRAM (FBP em páginas, PSM numérico,\n"
        "            ex.: 0xF0,10,0,640,512 para PSMCT32)\n"
        "  O rastreador do GS (ANYPS2_GS_PROBE, _DRAWLOG, _TEXDUMP, _VRAMLOG) continua valendo.\n",
        stderr);
}

bool parseUnsigned(const std::string& s, std::uint32_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 0);
    if (*end != '\0' || v > 0xFFFFFFFFul) return false;
    out = static_cast<std::uint32_t>(v);
    return true;
}

}  // namespace

int runCli(int argc, char** argv) {
    std::string dumpPath, prefix;
    std::uint64_t maxFrames = ~std::uint64_t{0};
    bool onlyOne = false;
    std::uint32_t onlyVsync = 0;
    bool wantFb = false;
    std::uint32_t fb[5] = {};
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw Error("falta o valor de " + a);
                return argv[++i];
            };
            if (a == "--out") {
                prefix = value();
            } else if (a == "--frames") {
                const std::string v = value();
                std::uint32_t n;
                if (v == "all") maxFrames = ~std::uint64_t{0};
                else if (parseUnsigned(v, n)) maxFrames = n;
                else throw Error("--frames espera um número ou 'all'");
            } else if (a == "--vsync") {
                if (!parseUnsigned(value(), onlyVsync)) throw Error("--vsync espera um número");
                onlyOne = true;
            } else if (a == "--fb") {
                const std::string v = value();
                std::size_t from = 0;
                for (int k = 0; k < 5; ++k) {
                    const std::size_t comma = v.find(',', from);
                    const std::string part = v.substr(from, comma == std::string::npos ? comma : comma - from);
                    if (!parseUnsigned(part, fb[k]) || (k < 4 && comma == std::string::npos)) {
                        throw Error("--fb espera FBP,FBW,PSM,W,H");
                    }
                    from = comma == std::string::npos ? v.size() : comma + 1;
                }
                if (fb[3] == 0 || fb[4] == 0 || fb[3] > 4096 || fb[4] > 4096) {
                    throw Error("--fb: W e H têm de estar entre 1 e 4096");
                }
                wantFb = true;
            } else if (a == "-h" || a == "--help") {
                usage();
                return 0;
            } else if (!a.empty() && a[0] == '-') {
                throw Error("opção desconhecida " + a);
            } else if (dumpPath.empty()) {
                dumpPath = a;
            } else {
                throw Error("argumento extra '" + a + "'");
            }
        }
        if (dumpPath.empty()) {
            usage();
            return 2;
        }
        if (prefix.empty()) prefix = std::filesystem::path(dumpPath).replace_extension().string();
        const std::filesystem::path parent = std::filesystem::path(prefix).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);

        const Dump dump = loadDump(dumpPath);
        std::printf("dump: %s (serial '%s', crc %08X, estado v%u, %zu bytes de pacotes)\n", dumpPath.c_str(),
                    dump.serial.c_str(), dump.crc, dump.stateVersion, dump.bytes.size() - dump.packetsOffset);
        Player player(dump);

        auto save = [&](const std::string& suffix, const Frame& f) {
            if (f.width == 0 || f.height == 0) {
                std::fprintf(stderr, "aviso: %s sem imagem (vídeo desligado)\n", suffix.c_str());
                return;
            }
            const std::string path = prefix + suffix + ".png";
            writePng(path, f);
            std::printf("gravado %s (%ux%u)\n", path.c_str(), f.width, f.height);
        };
        const bool initialOnly = onlyOne && onlyVsync == 0;
        if (initialOnly) save("_0", player.gs().display());
        Stats st;
        if (!initialOnly) {
            st = player.run([&](std::uint64_t n, Gs& gs) {
                if (!onlyOne || n == onlyVsync) save("_" + std::to_string(n), gs.display());
                return onlyOne ? n < onlyVsync : n < maxFrames;
            });
        }
        if (!onlyOne && st.vsyncs == 0) save("_fim", player.gs().display());  // dump sem VSync
        if (wantFb) save("_fb", readBuffer(player.gs(), fb[0], fb[1], fb[2], fb[3], fb[4]));
        std::printf("pacotes %llu (transferências %llu, ReadFIFO %llu, registradores %llu), desenhos %llu, "
                    "VSyncs %llu, erros %llu\n",
                    static_cast<unsigned long long>(st.packets), static_cast<unsigned long long>(st.transfers),
                    static_cast<unsigned long long>(st.readFifos), static_cast<unsigned long long>(st.registers),
                    static_cast<unsigned long long>(st.draws), static_cast<unsigned long long>(st.vsyncs),
                    static_cast<unsigned long long>(st.errors));
        for (const auto& m : st.messages) std::fprintf(stderr, "erro: %s\n", m.c_str());
        return st.errors ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "anyps2_gsplay: %s\n", e.what());
        return 1;
    }
}

}  // namespace anyps2::gsplay
