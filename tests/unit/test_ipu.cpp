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

namespace {

constexpr std::uint32_t kD4Chcr = 0x1000B400, kD4Madr = 0x1000B410, kD4Qwc = 0x1000B420, kD4Tadr = 0x1000B430;
constexpr std::uint32_t kDStat = 0x1000E010;

// Quadwords com bytes que dizem de onde vieram: q[i] = (tag << 4) | i.
void fillQw(Memory& m, std::uint32_t addr, unsigned count, std::uint8_t tag) {
    for (unsigned q = 0; q < count; ++q) {
        std::uint8_t b[16];
        for (unsigned i = 0; i < 16; ++i) b[i] = static_cast<std::uint8_t>((unsigned{tag} << 4) | (q & 0xF));
        m.copyToGuest(addr + q * 16, b, 16, 0);
    }
}
void dmaTag(Memory& m, std::uint32_t at, std::uint64_t qwc, std::uint64_t id, std::uint64_t addr) {
    const std::uint64_t t[2] = {qwc | (id << 28) | (addr << 32), 0};
    m.copyToGuest(at, t, 16, 0);
}
// Consome 32 bits por FDEC e devolve os primeiros 4 bytes de cada quadword
// consumido (o FDEC devolve o que vem depois do pulo).
std::uint32_t nextWord(Fixture& f) {
    f.cmd(0x40000020);
    return static_cast<std::uint32_t>(f.cmd64());
}

}  // namespace

// Modo normal: o canal enche o FIFO (8 quadwords) e pausa com STR ligado;
// cada quadword que o IPU consome puxa mais um; no fim, STR desliga e o
// D_STAT.CIS4 é marcado.
TEST_CASE(ipu, dma_to_ipu_normal_on_demand) {
    Fixture f;
    Memory& m = f.m;
    std::uint8_t data[20 * 16];
    for (unsigned i = 0; i < sizeof data; ++i) data[i] = static_cast<std::uint8_t>(i);
    m.copyToGuest(0x00100000, data, sizeof data, 0);
    f.cmd(0x00000000);  // BCLR
    m.write<std::uint32_t>(kD4Madr, 0x00100000, 0);
    m.write<std::uint32_t>(kD4Qwc, 20, 0);
    m.write<std::uint32_t>(kD4Chcr, 0x101, 0);
    CHECK_EQ(m.read<std::uint32_t>(kD4Chcr, 0) & 0x100, 0x100u);
    CHECK_EQ(m.read<std::uint32_t>(kD4Qwc, 0), 12u);
    CHECK_EQ(m.read<std::uint32_t>(kD4Madr, 0), 0x00100080u);
    CHECK_EQ(f.ctrl() & 0xF, 8u);
    // FDEC: o primeiro quadword vai para o buffer interno e o DMA repõe.
    f.cmd(0x40000000);
    CHECK_EQ(f.cmd64(), 0x00010203ull);
    CHECK_EQ(f.ctrl() & 0xF, 8u);
    CHECK_EQ(m.read<std::uint32_t>(kD4Qwc, 0), 11u);
    // SETIQ: 64 bytes (quadwords 0–3); o 4 já entra no buffer interno.
    f.cmd(0x50000000);
    for (unsigned i = 0; i < 64; ++i) CHECK_EQ(f.rt.ipu().intraQuant()[i], data[i]);
    CHECK_EQ(m.read<std::uint32_t>(kD4Qwc, 0), 7u);
    CHECK_EQ(m.read<std::uint32_t>(kD4Madr, 0), 0x00100000u + 13 * 16);
    CHECK_EQ(f.bp(), (8u << 8) | (1u << 16));
    // Consome o resto, 32 bits por vez, conferindo a ordem.
    for (unsigned w = 17; w < 80; ++w) {
        std::uint32_t expect = 0;
        for (unsigned i = 0; i < 4; ++i) expect = (expect << 8) | ((w * 4 + i) & 0xFF);
        CHECK_EQ(nextWord(f), expect);
        if (w == 24) CHECK_EQ(m.read<std::uint32_t>(kD4Chcr, 0) & 0x100, 0x100u);
    }
    CHECK_EQ(m.read<std::uint32_t>(kD4Chcr, 0) & 0x100, 0u);
    CHECK_EQ(m.read<std::uint32_t>(kD4Qwc, 0), 0u);
    CHECK_EQ(m.read<std::uint32_t>(kDStat, 0) & (1u << 4), 1u << 4);
    // Dados acabaram: o próximo FDEC espera.
    f.cmd(0x40000020);
    CHECK_EQ(f.ctrl() >> 31, 1u);
}

// Chain (ref/refe) maior que o FIFO; e o padrão da libmpeg: com o canal
// pausado no último tag (refe), o programa acrescenta um tag e reescreve o
// CHCR com ID=ref — a cadeia continua em vez de terminar.
TEST_CASE(ipu, dma_to_ipu_chain_and_append) {
    Fixture f;
    Memory& m = f.m;
    fillQw(m, 0x00200000, 8, 0xA);
    fillQw(m, 0x00210000, 4, 0xB);
    fillQw(m, 0x00220000, 1, 0xC);
    dmaTag(m, 0x00100000, 8, 3, 0x00200000);  // ref: 8 de A
    dmaTag(m, 0x00100010, 4, 0, 0x00210000);  // refe: 4 de B
    f.cmd(0x00000000);
    m.write<std::uint32_t>(kD4Tadr, 0x00100000, 0);
    m.write<std::uint32_t>(kD4Qwc, 0, 0);
    m.write<std::uint32_t>(kD4Chcr, 0x105, 0);  // chain, STR
    // A encheu o FIFO; o canal pausou já no tag de B (QWC=4).
    CHECK_EQ(f.ctrl() & 0xF, 8u);
    const std::uint32_t chcr = m.read<std::uint32_t>(kD4Chcr, 0);
    CHECK_EQ(chcr & 0x100, 0x100u);
    CHECK_EQ(chcr >> 28, 0u);  // tag corrente: refe
    CHECK_EQ(m.read<std::uint32_t>(kD4Qwc, 0), 4u);
    CHECK_EQ(m.read<std::uint32_t>(kD4Tadr, 0), 0x00100020u);
    // Acrescenta C (refe) depois de B e troca o tag corrente para ref.
    dmaTag(m, 0x00100020, 1, 0, 0x00220000);
    m.write<std::uint32_t>(0x1000F590, 0x1201 | 0x10000, 0);  // D_ENABLEW: suspende
    m.write<std::uint32_t>(kD4Chcr, (chcr & 0x0FFFFFFFu) | 0x30000000u, 0);
    m.write<std::uint32_t>(0x1000F590, 0x1201, 0);
    // Lê tudo: 8 quadwords de A, 4 de B e 1 de C, nessa ordem.
    for (unsigned q = 0; q < 13; ++q) {
        const unsigned tag = q < 8 ? 0xA : q < 12 ? 0xB : 0xC;
        const unsigned idx = q < 8 ? q : q < 12 ? q - 8 : 0;
        const std::uint32_t b = (tag << 4) | idx;
        const std::uint32_t expect = b * 0x01010101u;
        for (unsigned w = 0; w < 4; ++w) {
            if (q == 0 && w == 0) {
                f.cmd(0x40000000);
                CHECK_EQ(static_cast<std::uint32_t>(f.cmd64()), expect);
            } else {
                CHECK_EQ(nextWord(f), expect);
            }
        }
    }
    CHECK_EQ(m.read<std::uint32_t>(kD4Chcr, 0) & 0x100, 0u);
    CHECK_EQ(m.read<std::uint32_t>(kD4Tadr, 0), 0x00100030u);
    CHECK_EQ(m.read<std::uint32_t>(kDStat, 0) & (1u << 4), 1u << 4);
}
