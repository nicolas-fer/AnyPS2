// Testes do IPU: registradores, FIFO de entrada e ponteiro de bits (BP/FP/
// IFC) e os comandos sem decodificação (BCLR, FDEC, SETIQ, SETVQ, SETTH).
// Os valores esperados saem da definição do fluxo de bits (MSB primeiro, na
// ordem dos bytes na memória), calculada à parte bit a bit.

#include <cstring>
#include <vector>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/ipu.h"
#include "anyps2/runtime/ipu_mpeg.h"
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

// ---- Etapa 3: VLC, IDCT, VDEC, BDEC e saída -----------------------------------

namespace {

// Monta um fluxo de bits (MSB primeiro) a partir de códigos da norma.
struct BitWriter {
    std::vector<std::uint8_t> bytes;
    unsigned bits = 0;
    void put(std::uint32_t v, unsigned n) {
        for (unsigned i = n; i-- > 0;) {
            if (bits % 8 == 0) bytes.push_back(0);
            if ((v >> i) & 1) bytes.back() |= static_cast<std::uint8_t>(0x80u >> (bits % 8));
            ++bits;
        }
    }
    void code(const char* s) {
        for (; *s; ++s) {
            if (*s != ' ') put(*s == '1' ? 1u : 0u, 1);
        }
    }
    void alignByte() {
        while (bits % 8) put(0, 1);
    }
};

std::uint32_t msb(const char* s) {
    BitWriter w;
    w.code(s);
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v = (v << 8) | (i < w.bytes.size() ? w.bytes[i] : 0u);
    return v;
}

// Põe o fluxo na RAM (completado até quadword) e liga o toIPU em modo normal.
void feed(Fixture& f, const BitWriter& w, std::uint32_t addr = 0x00100000) {
    std::vector<std::uint8_t> b = w.bytes;
    b.resize((b.size() + 15) & ~std::size_t{15}, 0);
    f.m.copyToGuest(addr, b.data(), static_cast<std::uint32_t>(b.size()), 0);
    f.m.write<std::uint32_t>(kD4Madr, addr, 0);
    f.m.write<std::uint32_t>(kD4Qwc, static_cast<std::uint32_t>(b.size() / 16), 0);
    f.m.write<std::uint32_t>(kD4Chcr, 0x101, 0);
}

// Liga o fromIPU para `qwc` quadwords em `addr`.
void drainTo(Fixture& f, std::uint32_t addr, std::uint32_t qwc) {
    f.m.write<std::uint32_t>(0x1000B010, addr, 0);
    f.m.write<std::uint32_t>(0x1000B020, qwc, 0);
    f.m.write<std::uint32_t>(0x1000B000, 0x100, 0);
}

std::int16_t raw16(Memory& m, std::uint32_t base, unsigned index) {
    return static_cast<std::int16_t>(m.read<std::uint16_t>(base + 2 * index, 0));
}

}  // namespace

// Algumas entradas de cada tabela, com o código escrito como na norma (a
// conferência completa das tabelas contra outra implementação foi feita à
// parte; aqui o que se testa é a consulta).
TEST_CASE(ipu, mpeg_vlc_tables) {
    using namespace anyps2::rt::mpeg;
    auto check = [](Table t, const char* code, int value, unsigned len) {
        const Vlc v = decode(t, msb(code));
        CHECK_EQ(v.value, value);
        CHECK_EQ(v.len, len);
    };
    check(Table::Mbai, "1", 1, 1);
    check(Table::Mbai, "0000 0101 11", 16, 10);
    check(Table::Mbai, "0000 0011 000", 33, 11);
    check(Table::Mbai, "0000 0001 111", kMbaiStuffing, 11);
    check(Table::Mbai, "0000 0001 000", kMbaiEscape, 11);
    CHECK_EQ(decode(Table::Mbai, msb("0000 0000 0001")).len, 0u);
    check(Table::MbTypeI, "01", kMbIntra | kMbQuant, 2);
    check(Table::MbTypeP, "001", kMbForward, 3);
    check(Table::MbTypeP, "0000 01", kMbIntra | kMbQuant, 6);
    check(Table::MbTypeB, "11", kMbForward | kMbBackward | kMbPattern, 2);
    check(Table::MbTypeB, "0000 10", kMbQuant | kMbBackward | kMbPattern, 6);
    check(Table::Cbp, "111", 60, 3);
    check(Table::Cbp, "0001 0011", 15, 8);
    check(Table::Cbp, "0000 0000 1", 0, 9);
    check(Table::MotionCode, "1", 0, 1);
    check(Table::MotionCode, "0000 0011 00", 16, 10);
    check(Table::DmVector, "11", -1, 2);
    check(Table::DcSizeLuma, "100", 0, 3);
    check(Table::DcSizeLuma, "1111 1111 1", 11, 9);
    check(Table::DcSizeChroma, "00", 0, 2);
    check(Table::DcSizeChroma, "1111 1111 11", 11, 10);
    auto dct = [](bool one, bool first, const char* code) { return decodeDct(one, first, msb(code)); };
    CHECK_EQ(dct(false, false, "10").kind, Dct::Eob);
    CHECK_EQ(dct(false, true, "10").kind, Dct::Coef);  // primeiro não intra: "1s" = (0,1)
    CHECK_EQ(dct(false, true, "10").len, 1u);
    CHECK_EQ(dct(false, false, "11").level, 1u);
    CHECK_EQ(dct(false, false, "11").len, 2u);
    CHECK_EQ(dct(false, false, "0000 01").kind, Dct::Escape);
    const Dct last = dct(false, false, "0000 0000 0001 1011");
    CHECK_EQ(last.run, 31u);
    CHECK_EQ(last.level, 1u);
    CHECK_EQ(last.len, 16u);
    CHECK_EQ(dct(true, false, "0110").kind, Dct::Eob);
    CHECK_EQ(dct(true, false, "1111 1111").level, 15u);
    CHECK_EQ(dct(true, false, "10").len, 2u);
    CHECK_EQ(dct(false, false, "0000 0000 0000 1").kind, Dct::Invalid);
}

// IDCT: só o DC dá um bloco constante (F/8); um coeficiente horizontal (u=1)
// varia em x e não em y.
TEST_CASE(ipu, mpeg_idct) {
    using anyps2::rt::mpeg::idct;
    std::int32_t in[64] = {}, out[64];
    in[0] = 80;
    idct(in, out);
    for (int v : out) CHECK_EQ(v, 10);
    in[0] = 0;
    in[1] = 100;
    idct(in, out);
    // 100 · cos((2x+1)π/16) / (4√2), arredondado.
    const int row[8] = {17, 15, 10, 3, -3, -10, -15, -17};
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; ++x) CHECK_EQ(out[y * 8 + x], row[x]);
    }
}

TEST_CASE(ipu, vdec_tables_and_top) {
    Fixture f;
    BitWriter w;
    w.code("0000 0101 11");            // MBAI 16
    w.code("0011");                    // tipo B: forward + pattern
    w.code("0000 0011 01");            // motion_code 15...
    w.code("1");                       // ...negativo
    w.code("11");                      // dmvector -1
    w.code("1");                       // motion_code 0
    w.code("0000 0000 0001");          // MBAI inválido
    w.put(0, 4);
    w.put(0xCAFEBABEu, 32);
    w.put(0x12345678u, 32);
    f.cmd(0x00000000);
    feed(f, w);
    f.cmd(0x30000000);  // VDEC tabela 0
    CHECK_EQ(f.ctrl() >> 31, 0u);
    CHECK_EQ(f.cmd64(), 0x000A0010ull);
    CHECK(f.ipuIrq());
    f.m.write<std::uint32_t>(kCtrl, 0x03000000, 0);  // PCT = B
    f.cmd(0x34000000);
    CHECK_EQ(f.cmd64(), 0x0004008Aull);  // 8|2 + quadro (2<<6) + 4 bits
    f.cmd(0x38000000);
    CHECK_EQ(f.cmd64(), 0xFFFFFFF1ull);  // -15 (o comprimento se perde no sinal)
    f.cmd(0x3C000000);
    CHECK_EQ(f.cmd64(), 0xFFFFFFFFull);
    f.cmd(0x38000000);
    CHECK_EQ(f.cmd64(), 0x00010000ull);
    CHECK_EQ(f.ctrl() & (1u << 14), 0u);
    f.cmd(0x30000000);
    CHECK_EQ(f.cmd64(), 0ull);
    CHECK_EQ(f.ctrl() & (1u << 14), 1u << 14);  // ECD
    // O código inválido não foi consumido: pulando os 16 bits dele, o resto.
    f.cmd(0x40000010);
    CHECK_EQ(f.cmd64(), 0xCAFEBABEull);
}

// Macrobloco intra: DC de cada bloco (com predição), um coeficiente AC por
// escape no Y0; depois dele, um start code alinhado liga SCD.
TEST_CASE(ipu, bdec_intra_macroblock) {
    Fixture f;
    BitWriter w;
    for (unsigned i = 0; i < 64; ++i) w.put(16, 8);    // matriz intra (SETIQ)
    w.code("100");                                      // Y0: DC +0 → 128
    w.code("0000 01");                                  // escape: run 0,
    w.put(0, 6);
    w.put(100, 12);                                     // level 100 em (u=1, v=0)
    w.code("10");                                       // EOB
    w.code("01");                                       // Y1: tamanho 2, "11" = +3 → 131
    w.code("11");
    w.code("10");
    w.code("00");                                       // Y2: tamanho 1, "0" = -1 → 130
    w.code("0");
    w.code("10");
    w.code("100");                                      // Y3: +0 → 130
    w.code("10");
    w.code("00");                                       // Cb: +0 → 128
    w.code("10");
    w.code("110");                                      // Cr: tamanho 3, "000" = -7 → 121
    w.code("000");
    w.code("10");
    w.alignByte();
    w.put(0x000001B3u, 32);                             // sequence header
    f.cmd(0x00000000);
    feed(f, w);
    f.cmd(0x50000000);  // SETIQ
    drainTo(f, 0x00300000, 48);
    f.cmd(0x2C010000);  // BDEC: MBI, DCR, QSC=1 (escala 2)
    CHECK_EQ(f.ctrl() >> 31, 0u);
    CHECK(f.ipuIrq());
    CHECK_EQ(f.m.read<std::uint32_t>(0x1000B000, 0) & 0x100, 0u);  // fromIPU terminou
    CHECK_EQ(f.m.read<std::uint32_t>(0x1000B010, 0), 0x00300000u + 768);
    Memory& m = f.m;
    constexpr std::uint32_t kOut = 0x00300000;
    // Y0: 128 + 200·cos((2x+1)π/16)/(4√2) — o escape dá (100·2·16)>>4 = 200.
    const int y0[8] = {163, 157, 148, 135, 121, 108, 99, 93};
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; ++x) CHECK_EQ(raw16(m, kOut, y * 16 + x), y0[x]);
    }
    CHECK_EQ(raw16(m, kOut, 0 * 16 + 8), 131);
    CHECK_EQ(raw16(m, kOut, 7 * 16 + 15), 131);
    CHECK_EQ(raw16(m, kOut, 8 * 16 + 0), 130);
    CHECK_EQ(raw16(m, kOut, 15 * 16 + 15), 130);
    CHECK_EQ(raw16(m, kOut, 256), 128);
    CHECK_EQ(raw16(m, kOut, 256 + 63), 128);
    CHECK_EQ(raw16(m, kOut, 320), 121);
    CHECK_EQ(raw16(m, kOut, 383), 121);
    // CBP = 0x3F, SCD ligado, TOP no start code.
    CHECK_EQ((f.ctrl() >> 8) & 0x3F, 0x3Fu);
    CHECK_EQ(f.ctrl() & (3u << 14), 1u << 15);
    CHECK_EQ(f.m.read<std::uint64_t>(kTop, 0), 0x000001B3ull);
}

// Não intra: o CBP vem do fluxo; só o Y0 tem coeficiente. Sem fromIPU, o
// comando fica ocupado até a saída caber no FIFO (8 quadwords).
TEST_CASE(ipu, bdec_non_intra_and_output_fifo) {
    Fixture f;
    BitWriter w;
    for (unsigned i = 0; i < 64; ++i) w.put(16, 8);  // matriz não intra
    w.code("1010");                                  // CBP 32: só Y0
    w.code("0000 01");                               // escape no primeiro: run 0,
    w.put(0, 6);
    w.put(20, 12);                                   // level 20 → ((2·20+1)·2·16)>>5 = 41
    w.code("10");                                    // EOB
    w.put(0xFFFFFFFFu, 32);                          // dados quaisquer (sem start code)
    f.cmd(0x00000000);
    feed(f, w);
    f.cmd(0x58000000);  // SETIQ não intra
    f.cmd(0x20010000);  // BDEC não intra, QSC=1
    CHECK_EQ(f.ctrl() >> 31, 1u);
    CHECK_EQ((f.ctrl() >> 4) & 0xF, 8u);  // OFC
    CHECK(f.ipuIrq());                    // o do SETIQ
    CHECK(!f.ipuIrq());
    std::vector<std::int16_t> mb;
    for (unsigned q = 0; q < 48; ++q) {
        Reg128 r{};
        f.m.read128(0x10007000, r, 0);
        std::uint8_t b[16];
        std::memcpy(b, &r, 16);
        for (unsigned i = 0; i < 8; ++i) {
            mb.push_back(static_cast<std::int16_t>(b[2 * i] | (unsigned{b[2 * i + 1]} << 8)));
        }
        if (q == 38) CHECK_EQ(f.ctrl() >> 31, 1u);
        if (q == 39) {
            CHECK_EQ(f.ctrl() >> 31, 0u);  // sobraram 8: terminou
            CHECK(f.ipuIrq());
        }
    }
    for (unsigned y = 0; y < 16; ++y) {
        for (unsigned x = 0; x < 16; ++x) CHECK_EQ(mb[y * 16 + x], (y < 8 && x < 8) ? 5 : 0);  // 41/8
    }
    CHECK_EQ(mb[300], 0);
    CHECK_EQ((f.ctrl() >> 8) & 0x3F, 32u);
    CHECK_EQ(f.ctrl() & (3u << 14), 0u);
}

// Os dados chegam aos poucos: a primeira tentativa fica sem dado e volta
// atrás; com o resto, o resultado é o mesmo de ter tudo de uma vez.
TEST_CASE(ipu, bdec_restarts_when_data_arrives) {
    BitWriter w;
    for (unsigned i = 0; i < 64; ++i) w.put(16, 8);
    for (unsigned b = 0; b < 6; ++b) {
        w.code(b < 4 ? "01" : "10");  // tamanho do DC 2
        w.code("10");                 // +2
        w.code("0000 01");            // escape: run 3, level -7
        w.put(3, 6);
        w.put(0xFF9, 12);
        w.code("10");                 // EOB
    }
    w.put(0xFFFFFFFFu, 32);
    std::vector<std::uint8_t> all = w.bytes;
    all.resize((all.size() + 15) & ~std::size_t{15}, 0);
    const auto total = static_cast<std::uint32_t>(all.size() / 16);
    REQUIRE(total > 5);
    std::vector<std::int16_t> expected;
    {
        Fixture g;
        g.cmd(0x00000000);
        feed(g, w);
        g.cmd(0x50000000);
        drainTo(g, 0x00300000, 48);
        g.cmd(0x2C010000);
        CHECK_EQ(g.ctrl() >> 31, 0u);
        for (unsigned i = 0; i < 384; ++i) expected.push_back(raw16(g.m, 0x00300000, i));
    }
    CHECK(expected[0] != 0);
    // Só a matriz (4 quadwords) e mais um; depois o resto.
    Fixture f;
    f.cmd(0x00000000);
    f.m.copyToGuest(0x00100000, all.data(), static_cast<std::uint32_t>(all.size()), 0);
    f.m.write<std::uint32_t>(kD4Madr, 0x00100000, 0);
    f.m.write<std::uint32_t>(kD4Qwc, 5, 0);
    f.m.write<std::uint32_t>(kD4Chcr, 0x101, 0);
    f.cmd(0x50000000);
    drainTo(f, 0x00300000, 48);
    f.cmd(0x2C010000);
    CHECK_EQ(f.ctrl() >> 31, 1u);
    CHECK_EQ(f.m.read<std::uint32_t>(0x1000B020, 0), 48u);  // nada saiu
    // Esperando dados, o FIFO aparece vazio (como no hardware, que já teria
    // consumido tudo): é o que faz a libmpeg chamar o callback de "sem dados".
    CHECK_EQ(f.ctrl() & 0xF, 0u);
    CHECK_EQ((f.bp() >> 8) & 0xF, 0u);
    f.m.write<std::uint32_t>(kD4Qwc, total - 5, 0);
    f.m.write<std::uint32_t>(kD4Chcr, 0x101, 0);
    CHECK_EQ(f.ctrl() >> 31, 0u);
    for (unsigned i = 0; i < 384; ++i) CHECK_EQ(raw16(f.m, 0x00300000, i), expected[i]);
}

// ---- Etapa 4: CSC -------------------------------------------------------------

namespace {

// Macrobloco RAW8: Y nas linhas 0–7 = 235; nas 8–15, x < 8 = 16 e x ≥ 8 = 126.
// Cb = 128 (o canto de baixo à esquerda = 90); Cr = 128, e 240 no canto de
// baixo à direita.
void cscMacroblock(BitWriter& w) {
    for (unsigned y = 0; y < 16; ++y) {
        for (unsigned x = 0; x < 16; ++x) w.put(y < 8 ? 235 : x < 8 ? 16 : 126, 8);
    }
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; ++x) w.put(y >= 4 && x < 4 ? 90 : 128, 8);
    }
    for (unsigned y = 0; y < 8; ++y) {
        for (unsigned x = 0; x < 8; ++x) w.put(y >= 4 && x >= 4 ? 240 : 128, 8);
    }
}

std::uint32_t pixel(Memory& m, std::uint32_t base, unsigned mb, unsigned y, unsigned x) {
    return m.read<std::uint32_t>(base + mb * 1024 + (y * 16 + x) * 4, 0);
}
constexpr std::uint32_t rgba(unsigned r, unsigned g, unsigned b, unsigned a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

}  // namespace

// YCbCr → RGB32 em ponto fixo, dois macroblocos; o segundo com os limiares
// do SETTH (TH0 zera o pixel escuro, TH1 dá alfa 0x40).
TEST_CASE(ipu, csc_rgb32_and_thresholds) {
    Fixture f;
    BitWriter w;
    cscMacroblock(w);
    cscMacroblock(w);
    f.cmd(0x00000000);
    feed(f, w);
    drainTo(f, 0x00300000, 64);
    f.cmd(0x70000001);  // CSC de 1 macrobloco, RGB32
    CHECK_EQ(f.ctrl() >> 31, 0u);
    CHECK(f.ipuIrq());
    Memory& m = f.m;
    CHECK_EQ(pixel(m, 0x00300000, 0, 0, 0), rgba(255, 255, 255, 0x80));
    CHECK_EQ(pixel(m, 0x00300000, 0, 7, 15), rgba(255, 255, 255, 0x80));
    CHECK_EQ(pixel(m, 0x00300000, 0, 8, 0), rgba(0, 15, 0, 0x80));  // Y=16, Cb=90
    CHECK_EQ(pixel(m, 0x00300000, 0, 8, 8), rgba(255, 37, 128, 0x80));
    CHECK_EQ(pixel(m, 0x00300000, 0, 15, 15), rgba(255, 37, 128, 0x80));
    CHECK_EQ(pixel(m, 0x00300000, 0, 12, 3), rgba(0, 15, 0, 0x80));
    f.cmd(0x90900020);  // SETTH: TH0 = 0x20, TH1 = 0x90
    drainTo(f, 0x00300400, 64);
    f.cmd(0x70000001);
    CHECK_EQ(pixel(m, 0x00300000, 1, 0, 0), rgba(255, 255, 255, 0x80));
    CHECK_EQ(pixel(m, 0x00300000, 1, 8, 0), 0u);                       // abaixo de TH0
    CHECK_EQ(pixel(m, 0x00300000, 1, 8, 8), rgba(255, 37, 128, 0x80)); // R acima de TH1
    CHECK_EQ(pixel(m, 0x00300000, 0, 8, 8), rgba(255, 37, 128, 0x80)); // o primeiro ficou
    // RGB16 (OFM) ainda não existe: erro claro.
    CHECK_THROWS_WITH(f.cmd(0x78000001), "RGB16");
}

// CSC de vários macroblocos com os dados chegando aos poucos: o que já saiu
// não se refaz; o comando espera e continua.
TEST_CASE(ipu, csc_waits_between_macroblocks) {
    Fixture f;
    BitWriter w;
    cscMacroblock(w);
    cscMacroblock(w);
    std::vector<std::uint8_t> all = w.bytes;  // 768 bytes = 48 quadwords
    f.m.copyToGuest(0x00100000, all.data(), static_cast<std::uint32_t>(all.size()), 0);
    f.cmd(0x00000000);
    f.m.write<std::uint32_t>(kD4Madr, 0x00100000, 0);
    f.m.write<std::uint32_t>(kD4Qwc, 30, 0);  // o primeiro inteiro e um pedaço do segundo
    f.m.write<std::uint32_t>(kD4Chcr, 0x101, 0);
    drainTo(f, 0x00300000, 128);
    f.cmd(0x70000002);
    CHECK_EQ(f.ctrl() >> 31, 1u);
    CHECK_EQ(f.ctrl() & 0xF, 0u);
    CHECK_EQ(f.m.read<std::uint32_t>(0x1000B020, 0), 64u);  // o primeiro já saiu
    CHECK_EQ(pixel(f.m, 0x00300000, 0, 8, 8), rgba(255, 37, 128, 0x80));
    f.m.write<std::uint32_t>(kD4Qwc, 18, 0);
    f.m.write<std::uint32_t>(kD4Chcr, 0x101, 0);
    CHECK_EQ(f.ctrl() >> 31, 0u);
    CHECK_EQ(f.m.read<std::uint32_t>(0x1000B000, 0) & 0x100, 0u);
    CHECK_EQ(pixel(f.m, 0x00300000, 1, 8, 8), rgba(255, 37, 128, 0x80));
    CHECK_EQ(pixel(f.m, 0x00300000, 1, 15, 0), rgba(0, 15, 0, 0x80));
}
