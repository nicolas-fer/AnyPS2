// IOP em HLE (Fase 6): cabeçalho de IRX, roteiro dos controles, leitura de
// ISO 9660, decodificação ADPCM e envelope/mixagem do SPU2; o leitor de
// vídeos (Program Stream) do MPG1; o driver de som da Polyphony (PDISPU2).

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "anyps2/common/error.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/input.h"
#include "anyps2/runtime/iop/cdvd.h"
#include "anyps2/runtime/iop/iop.h"
#include "anyps2/runtime/iop/movie.h"
#include "anyps2/runtime/iop/pdispu2.h"
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

namespace {

// Pacote PES: start code 00 00 01 id, tamanho (16 bits) e os dados.
void pes(std::vector<std::uint8_t>& s, std::uint8_t id, const std::vector<std::uint8_t>& data) {
    const auto n = static_cast<unsigned>(data.size());
    s.insert(s.end(), {0, 0, 1, id, static_cast<std::uint8_t>(n >> 8), static_cast<std::uint8_t>(n)});
    s.insert(s.end(), data.begin(), data.end());
}
// Pack header do MPEG-2 (marcador '01' no primeiro byte) com `stuffing` bytes.
void pack(std::vector<std::uint8_t>& s, unsigned stuffing, std::uint8_t marker = 0x44) {
    s.insert(s.end(), {0, 0, 1, 0xBA, marker, 0, 4, 0, 4, 1, 1, 0x9A, 0xEF, static_cast<std::uint8_t>(0xF8 | stuffing)});
    for (unsigned i = 0; i < stuffing; ++i) s.push_back(0xFF);
}

movie::ProgramStream streamOf(const std::vector<std::uint8_t>& s, bool loop = false) {
    return movie::ProgramStream(
        [&s](std::uint64_t pos, std::uint32_t n, std::uint8_t* dst) {
            if (pos + n > s.size()) return false;
            std::memcpy(dst, s.data() + pos, n);
            return true;
        },
        s.size(), loop);
}

}  // namespace

// Só os pacotes de vídeo (0xE0) saem; pack headers (com enchimento), system
// header, áudio privado e padding são pulados; 0x1B9 encerra.
TEST_CASE(iop, movie_program_stream_video_packets) {
    std::vector<std::uint8_t> s;
    const std::vector<std::uint8_t> v1 = {0x81, 0x80, 5, 1, 2, 3, 4, 5, 0, 0, 1, 0xB3};
    std::vector<std::uint8_t> v2(300);
    for (unsigned i = 0; i < v2.size(); ++i) v2[i] = static_cast<std::uint8_t>(i * 7);
    pack(s, 2);
    pes(s, 0xBB, {0x80, 1, 2, 3, 4, 5});  // system header
    pes(s, 0xE0, v1);
    pack(s, 0);
    pes(s, 0xBD, {0xA0, 9, 9});  // áudio
    pes(s, 0xBE, std::vector<std::uint8_t>(17, 0xFF));  // padding
    pes(s, 0xE0, v2);
    s.insert(s.end(), {0, 0, 1, 0xB9});
    pes(s, 0xE0, v1);  // depois do fim: não é lido
    auto ps = streamOf(s);
    std::vector<std::uint8_t> got;
    REQUIRE(ps.nextVideo(got, 0));
    CHECK(got == v1);
    REQUIRE(ps.nextVideo(got, 0));
    CHECK(got == v2);
    CHECK(!ps.nextVideo(got, 0));
    CHECK(ps.ended());
    CHECK(!ps.nextVideo(got, 0));
    // Dados que acabam no meio de um pacote também encerram.
    std::vector<std::uint8_t> cut;
    pes(cut, 0xE0, v2);
    cut.resize(100);
    auto ps2 = streamOf(cut);
    CHECK(!ps2.nextVideo(got, 0));
    // Pack header de MPEG-1 é recusado com erro claro.
    std::vector<std::uint8_t> mpeg1;
    pack(mpeg1, 0, 0x21);
    auto ps3 = streamOf(mpeg1);
    CHECK_THROWS_WITH(ps3.nextVideo(got, 0), "MPEG-1");
}

// A fatia de 5120 bytes que o EE recebe: registro {bytes, 0, bytes, 0}, os
// dados completados até múltiplo de 16 e um registro zerado.
TEST_CASE(iop, movie_slot_layout) {
    std::uint8_t slot[movie::kSlotSize];
    std::memset(slot, 0xCC, sizeof slot);
    std::vector<std::uint8_t> v(4090);
    for (unsigned i = 0; i < v.size(); ++i) v[i] = static_cast<std::uint8_t>(i + 1);
    CHECK_EQ(movie::buildSlot(&v, slot, 0), 32u + 4096u);
    std::uint32_t w[4];
    std::memcpy(w, slot, 16);
    CHECK_EQ(w[0], 4090u);
    CHECK_EQ(w[1], 0u);
    CHECK_EQ(w[2], 4090u);
    CHECK_EQ(w[3], 0u);
    CHECK(std::memcmp(slot + 16, v.data(), v.size()) == 0);
    CHECK_EQ(slot[16 + 4090], 0u);  // enchimento
    CHECK_EQ(slot[16 + 4095], 0u);
    for (unsigned i = 0; i < 16; ++i) CHECK_EQ(slot[16 + 4096 + i], 0u);  // registro de fim
    CHECK_EQ(slot[16 + 4096 + 16], 0xCCu);                                // o resto fica
    // Fim do stream: só o registro zerado.
    std::memset(slot, 0xCC, sizeof slot);
    CHECK_EQ(movie::buildSlot(nullptr, slot, 0), 16u);
    CHECK_EQ(slot[0], 0u);
    CHECK_EQ(slot[15], 0u);
    CHECK_EQ(slot[16], 0xCCu);
    // Pacote grande demais para a fatia.
    std::vector<std::uint8_t> big(5100);
    CHECK_THROWS_WITH(movie::buildSlot(&big, slot, 0), "não cabe");
}

// Vídeo em repetição (flag 0x10 do MPG1): no fim volta ao começo; sem nenhum
// pacote de vídeo numa passada, acaba em vez de girar para sempre.
TEST_CASE(iop, movie_program_stream_loop) {
    std::vector<std::uint8_t> s;
    pack(s, 0);
    pes(s, 0xE0, {1, 2, 3});
    pes(s, 0xE0, {4, 5});
    s.insert(s.end(), {0, 0, 1, 0xB9});
    auto ps = streamOf(s, true);
    std::vector<std::uint8_t> got;
    for (unsigned pass = 0; pass < 3; ++pass) {
        REQUIRE(ps.nextVideo(got, 0));
        CHECK_EQ(got.size(), 3u);
        REQUIRE(ps.nextVideo(got, 0));
        CHECK_EQ(got.size(), 2u);
    }
    CHECK(!ps.ended());
    std::vector<std::uint8_t> silent;
    pack(silent, 0);
    pes(silent, 0xBD, {9, 9});
    auto ps2 = streamOf(silent, true);
    CHECK(!ps2.nextVideo(got, 0));
    CHECK(ps2.ended());
}

// --- PDISPU2: o bloco de registradores de 960 bytes do driver de som ---

namespace {

// Monta o bloco que o EE manda (fn 4 do SPUP): dois núcleos de 464 bytes.
struct SpuBlock {
    std::vector<std::uint8_t> b = std::vector<std::uint8_t>(PdiSpu2::kBlockSize, 0);
    void w16(std::size_t off, std::uint32_t x) {
        b[off] = static_cast<std::uint8_t>(x);
        b[off + 1] = static_cast<std::uint8_t>(x >> 8);
    }
    void w32(std::size_t off, std::uint32_t x) {
        w16(off, x & 0xFFFF);
        w16(off + 2, x >> 16);
    }
    std::uint32_t r32(std::size_t off) const {
        return std::uint32_t{b[off]} | std::uint32_t{b[off + 1]} << 8 | std::uint32_t{b[off + 2]} << 16 |
               std::uint32_t{b[off + 3]} << 24;
    }
    static std::size_t voiceOff(unsigned core, unsigned i) { return core * PdiSpu2::kCoreSize + 8 + 16 * i; }
    // Configura a voz como o jogo faz antes de um key-on.
    void voice(unsigned core, unsigned i, std::uint32_t flags, std::uint32_t pitch, std::uint32_t volL,
               std::uint32_t volR, std::uint32_t ssa, std::uint32_t adsr1, std::uint32_t adsr2) {
        const std::size_t o = voiceOff(core, i);
        w16(o, flags);
        w16(o + 2, pitch);
        w16(o + 4, volL);
        w16(o + 6, volR);
        w32(o + 8, ssa);
        w16(o + 12, adsr1);
        w16(o + 14, adsr2);
        const std::size_t m = core * PdiSpu2::kCoreSize + 4;
        w32(m, r32(m) | 1u << i);
    }
    void keyOn(unsigned core, std::uint32_t mask) {
        const std::size_t c = core * PdiSpu2::kCoreSize;
        w32(c, r32(c) | 0x800);
        w32(c + 436, mask);
    }
    void keyOff(unsigned core, std::uint32_t mask) {
        const std::size_t c = core * PdiSpu2::kCoreSize;
        w32(c, r32(c) | 0x1000);
        w32(c + 440, mask);
    }
};

// Dois blocos ADPCM de amostra constante (+0x2000); o segundo é o fim com
// repetição ("loop" do começo ao fim), para a voz seguir tocando.
void writeLoopSample(Spu2& spu, std::uint32_t addr) {
    std::uint8_t d[32] = {};
    d[0] = 0x01;
    d[1] = 0x04;  // início do loop
    for (int i = 2; i < 16; ++i) d[i] = 0x44;
    d[16] = 0x01;
    d[17] = 0x03;  // fim + repetir
    for (int i = 18; i < 32; ++i) d[i] = 0x44;
    REQUIRE(spu.writeRam(addr, d, sizeof(d)));
}

std::int32_t peakLeft(Spu2& spu, std::size_t frames) {
    std::vector<std::int32_t> mix(2 * frames, 0);
    spu.render(mix.data(), frames);
    std::int32_t peak = 0;
    for (std::size_t i = 0; i < frames; ++i) peak = std::max(peak, mix[2 * i]);
    return peak;
}

}  // namespace

TEST_CASE(iop, pdispu2_key_on_plays_with_block_registers) {
    Spu2 spu;
    PdiSpu2 drv(spu);
    writeLoopSample(spu, 0x6000);
    SpuBlock blk;
    // Núcleo 1, voz 3 (= voz 27 do Spu2): endereço inicial exige a marca 0x40+0x8.
    blk.voice(1, 3, 0x40 | 0x8 | 0x1 | 0x2 | 0x4 | 0x10 | 0x20, 0x1000, 0x3FFF, 0x3FFF, 0x6000, 0x000F, 0x0000);
    blk.keyOn(1, 1u << 3);
    drv.apply(blk.b.data(), blk.b.size());
    CHECK(spu.active(27));
    CHECK(!spu.active(3));
    CHECK_EQ(spu.startAddress(27), 0x6000u);
    // 0x2000 * envelope (~0x7FFF) * volume (0x7FFE) ~ 0x1FFF
    const std::int32_t peak = peakLeft(spu, 400);
    CHECK(peak > 0x1F00 && peak <= 0x2000);
}

TEST_CASE(iop, pdispu2_mute_flag_restores_volume_and_mask_filters_voices) {
    Spu2 spu;
    PdiSpu2 drv(spu);
    writeLoopSample(spu, 0x6000);
    SpuBlock blk;
    // Voz 0: volume máximo só à esquerda, pela marca 0x40 (zera durante a
    // atualização e devolve o VOLL/VOLR do bloco no fim).
    blk.voice(0, 0, 0x40 | 0x8 | 0x1 | 0x10 | 0x20, 0x1000, 0x3FFF, 0, 0x6000, 0x000F, 0);
    // Voz 1: os campos estão no bloco, mas a máscara de vozes alteradas (+4)
    // não a inclui; o key-on usa os registradores padrão (tudo zero).
    const std::size_t v1 = SpuBlock::voiceOff(0, 1);
    blk.w16(v1, 0x40 | 0x8 | 0x1 | 0x2 | 0x4);
    blk.w16(v1 + 2, 0x1000);
    blk.w16(v1 + 4, 0x3FFF);
    blk.w16(v1 + 6, 0x3FFF);
    blk.w32(v1 + 8, 0x6000);
    blk.keyOn(0, 0x3);
    drv.apply(blk.b.data(), blk.b.size());
    CHECK(spu.active(0));
    std::vector<std::int32_t> mix(2 * 400, 0);
    spu.render(mix.data(), 400);
    std::int32_t l = 0, r = 0;
    for (std::size_t i = 0; i < 400; ++i) {
        l = std::max(l, mix[2 * i]);
        r = std::max(r, mix[2 * i + 1]);
    }
    CHECK(l > 0x1F00);  // só a voz 0 soa (a voz 1 ficou com pitch e volume 0)
    CHECK_EQ(r, 0);
}

TEST_CASE(iop, pdispu2_key_off_wins_only_without_key_on) {
    Spu2 spu;
    PdiSpu2 drv(spu);
    writeLoopSample(spu, 0x6000);
    SpuBlock on;
    on.voice(0, 0, 0x40 | 0x8 | 0x1 | 0x10 | 0x20, 0x1000, 0x3FFF, 0x3FFF, 0x6000, 0x000F, 0x0005);
    on.keyOn(0, 1);
    drv.apply(on.b.data(), on.b.size());
    CHECK(spu.active(0));
    // key-off sozinho: release (ADSR2 = 5, rápido) até parar
    SpuBlock off;
    off.keyOff(0, 1);
    drv.apply(off.b.data(), off.b.size());
    peakLeft(spu, 3000);
    CHECK(!spu.active(0));
    // key-on e key-off da mesma voz no mesmo bloco: vence o key-on
    SpuBlock both;
    both.keyOn(0, 1);
    both.keyOff(0, 1);
    drv.apply(both.b.data(), both.b.size());
    peakLeft(spu, 3000);
    CHECK(spu.active(0));
}

TEST_CASE(iop, pdispu2_live_voice_gets_new_volume_and_envelope) {
    Spu2 spu;
    PdiSpu2 drv(spu);
    writeLoopSample(spu, 0x6000);
    SpuBlock on;
    on.voice(0, 5, 0x40 | 0x8 | 0x1 | 0x10 | 0x20, 0x1000, 0x3FFF, 0x3FFF, 0x6000, 0x000F, 0x0000);
    on.keyOn(0, 1u << 5);
    drv.apply(on.b.data(), on.b.size());
    CHECK(peakLeft(spu, 400) > 0x1F00);
    // Muda só o VOLL, para a metade, sem novo key-on (a voz toca sem parar).
    SpuBlock chg;
    chg.voice(0, 5, 0x2, 0, 0x1FFF, 0, 0, 0, 0);
    drv.apply(chg.b.data(), chg.b.size());
    const std::int32_t half = peakLeft(spu, 400);
    CHECK(half > 0xF00 && half < 0x1100);
}

TEST_CASE(iop, pdispu2_status_reports_envelope_and_end) {
    Spu2 spu;
    PdiSpu2 drv(spu);
    // Um bloco de amostra com fim sem repetir.
    std::uint8_t d[16] = {};
    d[0] = 0x01;
    d[1] = 0x01;
    for (int i = 2; i < 16; ++i) d[i] = 0x44;
    REQUIRE(spu.writeRam(0x7000, d, sizeof(d)));
    SpuBlock blk;
    blk.voice(1, 0, 0x40 | 0x8 | 0x1 | 0x10 | 0x20, 0x1000, 0x3FFF, 0x3FFF, 0x7000, 0x000F, 0);
    blk.keyOn(1, 1);
    const auto st0 = drv.apply(blk.b.data(), blk.b.size());
    CHECK_EQ(st0.size(), static_cast<std::size_t>(PdiSpu2::kStatusSize));
    peakLeft(spu, 200);
    const auto st = drv.status();
    // núcleo 1 começa em 52; ENDX tem o bit 0; ENVX da voz 0 em 56
    const auto u32 = [&st](std::size_t o) {
        return std::uint32_t{st[o]} | std::uint32_t{st[o + 1]} << 8 | std::uint32_t{st[o + 2]} << 16 |
               std::uint32_t{st[o + 3]} << 24;
    };
    CHECK_EQ(u32(52), 1u);
    CHECK_EQ(u32(0), 0u);
    // enquanto a amostra tocava, o envelope esteve alto; depois de parar, ENVX = 0
    CHECK_EQ(st[56] | st[57] << 8, 0);
}

TEST_CASE(iop, pdispu2_rejects_short_block) {
    Spu2 spu;
    PdiSpu2 drv(spu);
    std::vector<std::uint8_t> small(100, 0);
    CHECK_THROWS_WITH(drv.apply(small.data(), small.size()), "bloco de registradores");
}
