// Reprodutor de GS dumps do PCSX2 (tools/gsplay): um dump sintético montado em
// memória (cabeçalho, estado com FRAME/SCISSOR e uma palavra na VRAM, registradores
// privilegiados, pacotes Transfer pelo PATH2 e pelo PATH1 antigo, um ReadFIFO2 e
// um VSync) tem de restaurar o estado, desenhar os sprites e chamar o VSync.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "anyps2/common/error.h"
#include "anyps2/runtime/gs/gs.h"
#include "gsplay.h"
#include "minitest.h"

using namespace anyps2;
using namespace anyps2::gsplay;
using namespace anyps2::rt::gs;

namespace {

void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}
void put64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    put32(v, static_cast<std::uint32_t>(x));
    put32(v, static_cast<std::uint32_t>(x >> 32));
}

constexpr std::uint32_t kSentinelWord = 0xCAFEF00Du;
constexpr std::size_t kSentinelAddr = 0x10000;  // byte da VRAM fora da área desenhada

// Estado (PCSX2 GSState::Freeze, versão 9): ambiente, contextos 1 e 2, vértice,
// GIFReg obsoleto, tr.x/y e a VRAM.
std::vector<std::uint8_t> makeState() {
    std::vector<std::uint8_t> s;
    put32(s, 9);
    std::uint64_t env[16] = {};
    env[2] = 1;  // PRMODECONT: o atributo vem do PRIM
    env[9] = 1;  // COLCLAMP
    for (std::uint64_t e : env) put64(s, e);
    for (int c = 0; c < 2; ++c) {
        std::uint64_t ctx[12] = {};
        ctx[6] = 0 | (63ull << 16) | (0ull << 32) | (63ull << 48);      // SCISSOR 0..63
        ctx[8] = (1ull << 16) | (1ull << 17);                           // TEST: ZTE, ZTST=ALWAYS
        ctx[10] = 0 | (1ull << 16) | (0ull << 24);                      // FRAME: FBP 0, FBW 1, PSMCT32
        ctx[11] = 1ull << 32;                                           // ZBUF: ZMSK
        for (std::uint64_t r : ctx) put64(s, r);
    }
    for (int i = 0; i < 5; ++i) put64(s, 0);  // vértice corrente
    put64(s, 0);                              // GIFReg obsoleto
    put32(s, 0);
    put32(s, 0);                              // tr.x, tr.y
    s.resize(s.size() + rt::gs::Vram::kSize);
    std::memcpy(s.data() + kStateVramOffset + kSentinelAddr, &kSentinelWord, 4);
    return s;
}

// Pacote Transfer com um GIFtag PACKED A+D e os pares (valor, registrador).
std::vector<std::uint8_t> adPacket(std::uint8_t path, const std::vector<std::pair<std::uint64_t, std::uint8_t>>& ad) {
    std::vector<std::uint8_t> data;
    put64(data, ad.size() | (1ull << 15) | (0ull << 58) | (1ull << 60));  // NLOOP, EOP, PACKED, NREG=1
    put64(data, 0xE);                                                     // REGS = A+D
    for (const auto& [value, reg] : ad) {
        put64(data, value);
        put64(data, reg);
    }
    std::vector<std::uint8_t> pkt;
    pkt.push_back(0);
    pkt.push_back(path);
    put32(pkt, static_cast<std::uint32_t>(data.size()));
    pkt.insert(pkt.end(), data.begin(), data.end());
    return pkt;
}

std::uint64_t rgbaq(unsigned r, unsigned g, unsigned b, unsigned a) {
    return r | (g << 8) | (b << 16) | (std::uint64_t{a} << 24) | (std::uint64_t{0x3F800000u} << 32);
}
std::uint64_t xy(unsigned x, unsigned y) { return (std::uint64_t{x} * 16) | ((std::uint64_t{y} * 16) << 16); }

std::vector<std::uint8_t> sprite(std::uint8_t path, unsigned x0, unsigned y0, unsigned x1, unsigned y1,
                                 std::uint64_t color) {
    return adPacket(path, {{6, PRIM}, {color, RGBAQ}, {xy(x0, y0), XYZ2}, {xy(x1, y1), XYZ2}});
}

std::vector<std::uint8_t> makeDump(const std::vector<std::vector<std::uint8_t>>& packets) {
    std::vector<std::uint8_t> d;
    put32(d, 0xFFFFFFFFu);
    const std::vector<std::uint8_t> state = makeState();
    const char serial[] = "SYNT-00000";
    put32(d, 36 + sizeof(serial));  // cabeçalho: 9 u32 + serial
    put32(d, 9);
    put32(d, static_cast<std::uint32_t>(state.size()));
    put32(d, 36);                    // serial_offset
    put32(d, sizeof(serial));
    put32(d, 0x1234ABCD);            // crc
    put32(d, 640);
    put32(d, 480);
    put32(d, 0);                     // screenshot_offset
    put32(d, 0);                     // screenshot_size
    d.insert(d.end(), serial, serial + sizeof(serial));
    d.insert(d.end(), state.begin(), state.end());
    d.resize(d.size() + 8192);       // registradores privilegiados
    for (const auto& p : packets) d.insert(d.end(), p.begin(), p.end());
    return d;
}

}  // namespace

TEST_CASE(gsplay, replays_sprites_vsync_and_restores_state) {
    // PATH2: sprite verde em 8..16; PATH1 antigo: sprite azul em 32..40.
    std::vector<std::uint8_t> readFifo = {2};
    put32(readFifo, 64);
    const std::vector<std::uint8_t> vsync = {1, 0};
    const Dump dump = parseDump(makeDump({sprite(1, 8, 8, 16, 16, rgbaq(0x11, 0x22, 0x33, 0x80)), readFifo,
                                          sprite(0, 32, 32, 40, 40, rgbaq(0, 0, 0xFF, 0x80)), vsync}));
    CHECK_EQ(dump.serial, std::string("SYNT-00000"));
    CHECK_EQ(dump.crc, 0x1234ABCDu);

    Player player(dump);
    std::uint64_t vsyncs = 0;
    std::uint32_t atVsync = 0;
    const Stats st = player.run([&](std::uint64_t n, Gs& gs) {
        vsyncs = n;
        atVsync = gs.vram().readPixel(PSMCT32, 0, 1, 10, 10);
        return true;
    });
    CHECK_EQ(st.packets, 4u);
    CHECK_EQ(st.transfers, 2u);
    CHECK_EQ(st.readFifos, 1u);
    CHECK_EQ(st.vsyncs, 1u);
    CHECK_EQ(st.errors, 0u);
    CHECK_EQ(st.draws, 2u);
    CHECK_EQ(vsyncs, 1u);
    CHECK_EQ(atVsync, 0x80332211u);

    Gs& gs = player.gs();
    CHECK_EQ(gs.vram().readPixel(PSMCT32, 0, 1, 8, 8), 0x80332211u);
    CHECK_EQ(gs.vram().readPixel(PSMCT32, 0, 1, 15, 15), 0x80332211u);
    CHECK_EQ(gs.vram().readPixel(PSMCT32, 0, 1, 16, 16), 0u);  // fora do sprite
    CHECK_EQ(gs.vram().readPixel(PSMCT32, 0, 1, 32, 32), 0x80FF0000u);
    CHECK_EQ(gs.vram().read32(kSentinelAddr), kSentinelWord);  // a VRAM do estado foi restaurada

    const Frame f = readBuffer(gs, 0, 1, PSMCT32, 64, 64);
    CHECK_EQ(f.width, 64u);
    CHECK_EQ(f.pixels[10 * 64 + 10], 0xFF332211u);
}

TEST_CASE(gsplay, stops_when_the_vsync_callback_says_so) {
    const std::vector<std::uint8_t> vsync = {1, 0};
    const Dump dump = parseDump(makeDump({vsync, vsync, vsync}));
    Player player(dump);
    const Stats st = player.run([](std::uint64_t n, Gs&) { return n < 2; });
    CHECK_EQ(st.vsyncs, 2u);
}

TEST_CASE(gsplay, rejects_bad_dumps) {
    auto good = makeDump({});
    auto bad = good;
    bad[0] = 0;
    CHECK_THROWS_WITH(parseDump(bad), "assinatura");
    bad = good;
    bad[8] = 8;
    CHECK_THROWS_WITH(parseDump(bad), "state_version");
    bad = good;
    bad.resize(bad.size() - 4);
    CHECK_THROWS_WITH(parseDump(bad), "truncado");
    auto truncated = makeDump({{77, 0}});  // tipo de pacote desconhecido
    const Dump d = parseDump(truncated);
    Player p(d);
    CHECK_THROWS_WITH(p.run(), "desconhecido");
}
