// Testes do IPU: registradores, FIFO de entrada e ponteiro de bits (BP/FP/
// IFC) e os comandos sem decodificação (BCLR, FDEC, SETIQ, SETVQ, SETTH).
// Os valores esperados saem da definição do fluxo de bits (MSB primeiro, na
// ordem dos bytes na memória), calculada à parte bit a bit.

#include <cstring>
#include <vector>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/ipu.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/runtime.h"
#include "minitest.h"

using namespace anyps2::rt;

namespace {

constexpr std::uint32_t kCmd = 0x10002000, kCtrl = 0x10002010, kBp = 0x10002020, kTop = 0x10002030;
constexpr std::uint32_t kInFifo = 0x10007010;

struct Fixture {
    ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    Runtime rt{info, RuntimeOptions{}};
    Memory& m = rt.memory();

    void push(const std::uint8_t* bytes) {
        Reg128 q{};
        std::memcpy(&q, bytes, 16);
        m.write128(kInFifo, q, 0);
    }
    void cmd(std::uint32_t c) { m.write<std::uint32_t>(kCmd, c, 0); }
    std::uint64_t cmd64() { return m.read<std::uint64_t>(kCmd, 0); }
    std::uint32_t ctrl() { return m.read<std::uint32_t>(kCtrl, 0); }
    std::uint32_t bp() { return m.read<std::uint32_t>(kBp, 0); }
    bool ipuIrq() {
        const bool raised = (rt.kernel().intcStat() & (1u << 8)) != 0;
        rt.kernel().clearIntcStat(1u << 8);
        return raised;
    }
};

// 32 bits a partir do bit `pos` (MSB primeiro), um bit por vez.
std::uint32_t bitsAt(const std::vector<std::uint8_t>& s, unsigned pos) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 32; ++i) {
        const unsigned b = pos + i;
        v = (v << 1) | ((s[b / 8] >> (7 - b % 8)) & 1);
    }
    return v;
}

}  // namespace

TEST_CASE(ipu, fdec_and_bit_pointer) {
    Fixture f;
    std::vector<std::uint8_t> s(48);
    for (unsigned i = 0; i < s.size(); ++i) s[i] = static_cast<std::uint8_t>(0x12 + i * 0x22);
    f.cmd(0x00000000);  // BCLR
    CHECK(f.ipuIrq());
    for (unsigned q = 0; q < 3; ++q) f.push(s.data() + q * 16);
    CHECK_EQ(f.ctrl() & 0xF, 3u);      // IFC
    CHECK_EQ(f.bp(), 3u << 8);         // BP=0, IFC=3, FP=0
    // FDEC sem pulo: os 32 primeiros bits; o primeiro quadword vai para o
    // buffer interno (FP=1, IFC=2) e BP não anda.
    f.cmd(0x40000000);
    CHECK_EQ(f.ctrl() >> 31, 0u);
    CHECK(f.ipuIrq());
    CHECK_EQ(static_cast<std::uint32_t>(f.cmd64()), bitsAt(s, 0));
    CHECK_EQ(f.cmd64() >> 63, 0ull);
    CHECK_EQ(f.bp(), (1u << 16) | (2u << 8));
    // FB=4: pula meio byte.
    f.cmd(0x40000004);
    CHECK_EQ(static_cast<std::uint32_t>(f.cmd64()), bitsAt(s, 4));
    CHECK_EQ(f.bp() & 0x7F, 4u);
    // Atravessa o fim do primeiro quadword: 4+59 = 63, depois +40 = 103 e a
    // leitura de 32 bits precisa do segundo (FP=2).
    f.cmd(0x4000003B);
    CHECK_EQ(static_cast<std::uint32_t>(f.cmd64()), bitsAt(s, 63));
    f.cmd(0x40000028);
    CHECK_EQ(static_cast<std::uint32_t>(f.cmd64()), bitsAt(s, 103));
    CHECK_EQ(f.bp(), 103u | (1u << 8) | (2u << 16));
    // IPU_TOP mostra os mesmos 32 bits, sem BUSY.
    CHECK_EQ(f.m.read<std::uint64_t>(kTop, 0), std::uint64_t{bitsAt(s, 103)});
    // +25 = 128: o primeiro quadword sai, o segundo passa à frente (FP=1).
    f.cmd(0x40000019);
    CHECK_EQ(static_cast<std::uint32_t>(f.cmd64()), bitsAt(s, 128));
    CHECK_EQ(f.bp(), (1u << 8) | (1u << 16));
}

TEST_CASE(ipu, command_waits_for_data) {
    Fixture f;
    std::uint8_t q[16];
    for (unsigned i = 0; i < 16; ++i) q[i] = static_cast<std::uint8_t>(0xA0 + i);
    f.cmd(0x00000000);
    f.ipuIrq();
    // Sem dados: TOP e o FDEC ficam ocupados.
    CHECK_EQ(f.m.read<std::uint64_t>(kTop, 0) >> 63, 1ull);
    f.cmd(0x40000008);
    CHECK_EQ(f.ctrl() >> 31, 1u);
    CHECK_EQ(f.cmd64() >> 63, 1ull);
    CHECK(!f.ipuIrq());
    // Outro comando com o IPU ocupado é erro do programa.
    CHECK_THROWS_WITH(f.cmd(0x40000000), "ainda em andamento");
    // O quadword chega pelo FIFO e o FDEC termina (pulando 8 bits).
    f.push(q);
    CHECK_EQ(f.ctrl() >> 31, 0u);
    CHECK(f.ipuIrq());
    CHECK_EQ(f.cmd64(), 0xA1A2A3A4ull);
    // BCLR com BP: esvazia o FIFO e posiciona o ponteiro.
    f.push(q);
    f.cmd(0x00000010);
    CHECK_EQ(f.bp(), 16u);
    f.push(q);
    f.cmd(0x40000000);
    CHECK_EQ(f.cmd64(), 0xA2A3A4A5ull);
}

TEST_CASE(ipu, setiq_setvq_setth) {
    Fixture f;
    std::uint8_t data[64];
    for (unsigned i = 0; i < 64; ++i) data[i] = static_cast<std::uint8_t>(i * 3 + 1);
    // SETIQ intra com os dados já no FIFO (como o sceMpegInit faz).
    f.cmd(0x00000000);
    for (unsigned q = 0; q < 4; ++q) f.push(data + q * 16);
    f.cmd(0x50000000);
    CHECK_EQ(f.ctrl() >> 31, 0u);
    CHECK(f.ipuIrq());
    for (unsigned i = 0; i < 64; ++i) CHECK_EQ(f.rt.ipu().intraQuant()[i], data[i]);
    CHECK_EQ(f.ctrl() & 0xF, 0u);
    // SETIQ não intra antes dos dados: espera os 4 quadwords.
    f.cmd(0x58000000);
    for (unsigned q = 0; q < 3; ++q) f.push(data + q * 16);
    CHECK_EQ(f.ctrl() >> 31, 1u);
    f.push(data + 48);
    CHECK_EQ(f.ctrl() >> 31, 0u);
    for (unsigned i = 0; i < 64; ++i) CHECK_EQ(f.rt.ipu().nonIntraQuant()[i], data[i]);
    // SETVQ: 16 cores de 16 bits (little-endian).
    f.push(data);
    f.push(data + 16);
    f.cmd(0x60000000);
    CHECK_EQ(f.ctrl() >> 31, 0u);
    CHECK_EQ(f.rt.ipu().vqClut()[0], static_cast<std::uint16_t>(data[0] | (data[1] << 8)));
    CHECK_EQ(f.rt.ipu().vqClut()[15], static_cast<std::uint16_t>(data[30] | (data[31] << 8)));
    // SETTH: TH0 nos bits 8..0, TH1 nos bits 24..16.
    f.ipuIrq();
    f.cmd(0x90450123);
    CHECK(f.ipuIrq());
    CHECK_EQ(f.rt.ipu().threshold(0), 0x123u);
    CHECK_EQ(f.rt.ipu().threshold(1), 0x045u);
}

TEST_CASE(ipu, ctrl_reset_and_unsupported) {
    Fixture f;
    std::uint8_t q[16] = {};
    f.push(q);
    f.push(q);
    // Modos graváveis (MP1, PCT=I, IVF, AS, IDP) ficam; RST esvazia o FIFO.
    f.m.write<std::uint32_t>(kCtrl, 0x01B10000, 0);
    CHECK_EQ(f.ctrl(), 0x01B10002u);
    f.ipuIrq();
    f.m.write<std::uint32_t>(kCtrl, 0x41B10000, 0);
    CHECK_EQ(f.ctrl(), 0x01B10000u);
    CHECK(f.ipuIrq());
    CHECK_EQ(f.bp(), 0u);
    CHECK_THROWS_WITH(f.cmd(0x10000000), "IDEC");
}
