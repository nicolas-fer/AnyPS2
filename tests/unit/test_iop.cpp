// IOP em HLE (Fase 6): cabeçalho de IRX, roteiro dos controles, leitura de
// ISO 9660, decodificação ADPCM e envelope/mixagem do SPU2.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "anyps2/common/error.h"
#include "anyps2/runtime/input.h"
#include "anyps2/runtime/iop/cdvd.h"
#include "anyps2/runtime/iop/iop.h"
#include "anyps2/runtime/iop/spu2.h"
#include "irx_builder.h"
#include "minitest.h"

using namespace anyps2::rt;

namespace {

using testutil::makeIrx;
using testutil::put32;

std::string tempPath(const std::string& name) {
    return (std::filesystem::temp_directory_path() / ("anyps2_test_" + name)).string();
}

}  // namespace

TEST_CASE(iop, irx_name_in_iopmod) {
    const auto d = makeIrx("cdvd_driver", "", 0x0101);
    const auto info = parseIrx(d.data(), d.size());
    REQUIRE(info.has_value());
    CHECK_EQ(info->name, std::string("cdvd_driver"));
    CHECK_EQ(info->version, 0x0101);
}

TEST_CASE(iop, irx_name_in_moduleinfo) {
    const auto d = makeIrx("", "audsrv", 0x0104);
    const auto info = parseIrx(d.data(), d.size());
    REQUIRE(info.has_value());
    CHECK_EQ(info->name, std::string("audsrv"));
    CHECK_EQ(info->version, 0x0104);
}

TEST_CASE(iop, irx_rejects_non_irx) {
    auto d = makeIrx("x", "", 1);
    put32(d, 52, 1);  // sem PT_SCE_IOPMOD
    CHECK(!parseIrx(d.data(), d.size()).has_value());
    const std::uint8_t junk[64] = {1, 2, 3};
    CHECK(!parseIrx(junk, sizeof(junk)).has_value());
    CHECK(!parseIrx(d.data(), 10).has_value());
}

TEST_CASE(iop, pad_buttons_parse) {
    CHECK_EQ(parsePadButtons("-"), 0);
    CHECK_EQ(parsePadButtons("CROSS"), padbtn::CROSS);
    CHECK_EQ(parsePadButtons("START+UP+R1"), padbtn::START | padbtn::UP | padbtn::R1);
    CHECK_THROWS_WITH(parsePadButtons("CROSS+X"), "desconhecido");
}

TEST_CASE(iop, pad_script) {
    const std::string path = tempPath("pad_script.txt");
    {
        std::ofstream f(path);
        f << "# comentário\n"
             "10 0 START\n"
             "\n"
             "5 1 CIRCLE 0 255 10 20\n"
             "20 0 -   # solta\n"
             "30 1 unplug\n";
    }
    Input in;
    in.loadScript(path);
    CHECK(in.scripted());
    CHECK_EQ(in.sample(0, 9).buttons, 0);
    CHECK_EQ(in.sample(0, 10).buttons, padbtn::START);
    CHECK_EQ(in.sample(0, 19).buttons, padbtn::START);
    CHECK_EQ(in.sample(0, 20).buttons, 0);
    const PadInput p1 = in.sample(1, 6);
    CHECK_EQ(p1.buttons, padbtn::CIRCLE);
    CHECK_EQ(p1.lx, 0);
    CHECK_EQ(p1.ly, 255);
    CHECK_EQ(p1.rx, 10);
    CHECK_EQ(p1.ry, 20);
    CHECK(p1.connected);
    CHECK(!in.sample(1, 30).connected);
    CHECK_EQ(in.sample(0, 5).lx, 0x80);
    {
        std::ofstream f(path);
        f << "10 0 START 1 2\n";
    }
    Input bad;
    CHECK_THROWS_WITH(bad.loadScript(path), ":1:");
    std::filesystem::remove(path);
}

TEST_CASE(iop, iso9660_lookup_and_read) {
    IsoImage iso;
    iso.open(ANYPS2_TEST_HOMEBREW_DIR "/cdvd/disc.iso");
    CHECK_EQ(iso.sectorCount(), 28u);
    const auto hello = iso.lookup("\\DATA\\HELLO.TXT;1");
    REQUIRE(hello.has_value());
    CHECK_EQ(hello->name, std::string("HELLO.TXT;1"));
    CHECK_EQ(hello->size, 23u);
    CHECK(!hello->isDir);
    CHECK_EQ(hello->date[0], 124);  // 2024
    char text[24] = {};
    REQUIRE(iso.readBytes(hello->lsn, 0, 23, reinterpret_cast<std::uint8_t*>(text)));
    CHECK_EQ(std::string(text), std::string("Ola do setor do disco!\n"));
    // Sem ";1", '/' como separador e minúsculas também acham.
    const auto big = iso.lookup("/data/big.bin");
    REQUIRE(big.has_value());
    CHECK_EQ(big->size, 5000u);
    std::uint8_t b[3];
    REQUIRE(iso.readBytes(big->lsn, 4095, 3, b));  // cruza o limite de setor
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned pos = 4095 + i;
        CHECK_EQ(b[i], static_cast<std::uint8_t>((pos * 7 + (pos >> 8)) & 0xFF));
    }
    const auto data = iso.lookup("\\DATA");
    REQUIRE(data.has_value());
    CHECK(data->isDir);
    const auto entries = iso.list(*data);
    REQUIRE(entries.size() == 2);
    CHECK_EQ(entries[0].name, std::string("BIG.BIN;1"));
    CHECK(!iso.lookup("\\NADA").has_value());
    CHECK(!iso.lookup("\\README.TXT;1\\X").has_value());
    CHECK(iso.lookup("\\")->isDir);
    std::uint8_t sector[2048];
    CHECK(!iso.readSectors(28, 1, sector));  // além do fim
}

TEST_CASE(iop, iso9660_rejects_non_iso) {
    const std::string path = tempPath("not.iso");
    {
        std::ofstream f(path, std::ios::binary);
        std::vector<char> zeros(40 * 2048, 0);
        f.write(zeros.data(), static_cast<std::streamsize>(zeros.size()));
    }
    IsoImage iso;
    CHECK_THROWS_WITH(iso.open(path), "não é uma imagem ISO 9660");
    CHECK_THROWS_WITH(iso.open(path + ".nao_existe"), "não foi possível abrir");
    std::filesystem::remove(path);
}

TEST_CASE(iop, adpcm_decode) {
    // Filtro 0, shift 0: nibble n -> n << 12 (com sinal).
    std::uint8_t block[16] = {0x00, 0x00, 0x71, 0x8F};
    std::int16_t out[28];
    std::int32_t s1 = 0, s2 = 0;
    Spu2::decodeBlock(block, out, s1, s2);
    CHECK_EQ(out[0], 0x1000);   // 1 (nibble baixo primeiro)
    CHECK_EQ(out[1], 0x7000);   // 7
    CHECK_EQ(out[2], -0x1000);  // 0xF = -1
    CHECK_EQ(out[3], -0x8000);  // 0x8 = -8
    CHECK_EQ(out[4], 0);
    // Filtro 1 (60/64 da amostra anterior), shift 12: só a predição.
    std::uint8_t pred[16] = {0x1C, 0x00};
    s1 = 6400;
    s2 = 0;
    Spu2::decodeBlock(pred, out, s1, s2);
    CHECK_EQ(out[0], (6400 * 60 + 32) >> 6);  // 6000
    CHECK_EQ(out[1], (6000 * 60 + 32) >> 6);  // 5625
    // Filtro 2 (115, -52) com saturação.
    std::uint8_t sat[16] = {0x20, 0x00, 0x77};
    s1 = 30000;
    s2 = 0;
    Spu2::decodeBlock(sat, out, s1, s2);
    CHECK_EQ(out[0], 0x7FFF);
}

TEST_CASE(iop, spu2_voice_plays_and_stops) {
    Spu2 spu;
    // Dois blocos de amostras constantes (+0x4000), o segundo com "fim".
    std::uint8_t data[32] = {};
    for (int b = 0; b < 2; ++b) {
        data[b * 16] = 0x01;  // shift 1: nibble 4 -> (4 << 12) >> 1 = 0x2000
        for (int i = 2; i < 16; ++i) data[b * 16 + i] = 0x44;
    }
    data[17] = 0x01;  // fim sem repetir
    REQUIRE(spu.writeRam(0x1000, data, sizeof(data)));
    CHECK(!spu.writeRam(Spu2::kRamSize - 4, data, 8));
    Spu2::VoiceSetup v;
    v.start = 0x1000;
    v.pitch = 0x1000;          // 1 amostra da origem por amostra de saída
    v.volL = 0x3FFF;           // máximo
    v.volR = 0;                // mudo à direita
    v.adsr1 = 0x000F;          // ataque instantâneo, sustain no máximo
    v.adsr2 = 0x0000;
    spu.keyOn(24 + 3, v);
    CHECK(spu.active(27));
    std::vector<std::int32_t> mix(2 * 100, 0);
    spu.render(mix.data(), 100);
    // 56 amostras (2 blocos) e a voz para; a interpolação atrasa 2 amostras.
    CHECK(!spu.active(27));
    std::int32_t peak = 0;
    for (int i = 0; i < 100; ++i) {
        peak = std::max(peak, mix[static_cast<std::size_t>(2 * i)]);
        CHECK_EQ(mix[static_cast<std::size_t>(2 * i + 1)], 0);
    }
    // 0x2000 * envelope (~0x7FFF) * volume (0x7FFE) ~ 0x1FFF
    CHECK(peak > 0x1F00 && peak <= 0x2000);
    CHECK_EQ(mix[2 * 99], 0);
}

TEST_CASE(iop, spu2_key_off_releases) {
    Spu2 spu;
    // Bloco 0: início de loop; bloco 1: fim + repetir -> volta ao bloco 0.
    std::uint8_t loop[32] = {};
    loop[0] = 0x01;
    loop[1] = 0x04;
    for (int i = 2; i < 16; ++i) loop[i] = 0x44;
    loop[16] = 0x01;
    loop[17] = 0x03;
    for (int i = 18; i < 32; ++i) loop[i] = 0x44;
    REQUIRE(spu.writeRam(0x2000, loop, sizeof(loop)));
    Spu2::VoiceSetup v;
    v.start = 0x2000;
    v.adsr1 = 0x000F;
    v.adsr2 = 0x0005;  // release linear rápido
    spu.keyOn(0, v);
    std::vector<std::int32_t> mix(2 * 2000, 0);
    spu.render(mix.data(), 2000);
    CHECK(spu.active(0));  // loop: continua tocando
    CHECK(mix[2 * 1999] > 0x1000);
    spu.keyOff(0);
    std::fill(mix.begin(), mix.end(), 0);
    spu.render(mix.data(), 2000);
    CHECK(!spu.active(0));
    CHECK_EQ(mix[2 * 1999], 0);
}
