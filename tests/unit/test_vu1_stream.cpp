// Fila ordenada do VU1 (fatia 4 do plano do VU1 em outra thread): a ordem dos itens, os dados
// COPIADOS no enfileiramento (o EE reescrever a memória depois do push não muda o que o GS
// recebe), os eventos SIGNAL/FINISH/LABEL/VSINT/reset pela fila de respostas com o mesmo CSR,
// SIGLBLID e INTC de antes (o oráculo é o Gs isolado, que aplica no próprio espelho na hora), os
// erros do consumidor relançados no EE com a mesma classe, mensagem e pc, e a chave
// ANYPS2_VU1_THREAD.
//
// Dois modos do stream: o padrão (inline, o consumidor roda em cada push) e o adiado
// (setDeferred, só para testes: enfileira com cópia e só consome no fence), que expõe o que a
// thread de verdade vai precisar.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/timing.h"
#include "anyps2/runtime/vif.h"
#include "anyps2/runtime/vu/vu.h"
#include "anyps2/runtime/vu1_stream.h"
#include "minitest.h"

using namespace anyps2::rt;
using namespace anyps2::rt::gs;

namespace {

struct Pkt {
    std::vector<std::uint64_t> q;
    // GIFtag A+D com `n` registradores.
    explicit Pkt(unsigned n, bool eop = true) {
        q.push_back(n | (std::uint64_t{eop} << 15) | (std::uint64_t{1} << 60));
        q.push_back(0xE);
    }
    Pkt& ad(std::uint8_t reg, std::uint64_t value) {
        q.push_back(value);
        q.push_back(reg);
        return *this;
    }
    const std::uint8_t* data() const { return reinterpret_cast<const std::uint8_t*>(q.data()); }
    std::uint8_t* data() { return reinterpret_cast<std::uint8_t*>(q.data()); }
    std::size_t qwords() const { return q.size() / 2; }
};

struct Env {
    ProgramInfo info{"teste", 0, nullptr, 0, "nenhum.image"};
    RuntimeOptions options = [] {
        RuntimeOptions o;
        o.virtualClock = true;
        return o;
    }();
    Runtime rt{info, options};
};

constexpr std::uint32_t kLNop = 0x8000033Cu, kUNop = 0x000002FFu, kEnd = 1u << 30;
std::uint32_t lo(std::uint32_t op, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs, std::int32_t imm11) {
    return (op << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (static_cast<std::uint32_t>(imm11) & 0x7FF);
}

}  // namespace

// Ordem: PATH3, PATH2 (DIRECT) e PATH3 escrevem o mesmo registrador do GS; vale o último na ordem
// de emissão do EE. Adiado, nada vale antes do fence; e cada item carrega o seq da emissão.
TEST_CASE(vu1_stream, items_apply_in_emission_order) {
    Env e;
    Vu1Stream& s = e.rt.vu1Stream();
    CHECK(s.inlineMode());
    s.setDeferred(true);
    CHECK(!s.inlineMode());

    Pkt a(1), b(1), c(1);
    a.ad(TEST_1, 1);
    b.ad(TEST_1, 2);
    c.ad(TEST_1, 3);
    s.pushPath3(a.data(), a.qwords(), 0x100);
    // O DIRECT do VIF1 entrega um pacote de 16 bytes por vez ao PATH2.
    VifItem d1{VifItem::Kind::Direct};
    d1.pc = 0x200;
    d1.data = b.data();
    VifItem d2{VifItem::Kind::Direct};
    d2.pc = 0x200;
    d2.data = b.data() + 16;
    s.pushVif(d1);
    s.pushVif(d2);
    s.pushPath3(c.data(), c.qwords(), 0x300);

    CHECK_EQ(s.pushedSeq(), 4u);
    CHECK_EQ(s.processedSeq(), 0u);
    CHECK_EQ(s.queued(), 4u);
    CHECK_EQ(e.rt.gs().reg(TEST_1), 0u);  // nada aplicado antes do fence
    s.fence();
    CHECK_EQ(s.queued(), 0u);
    CHECK_EQ(s.processedSeq(), 4u);
    CHECK_EQ(e.rt.gs().reg(TEST_1), 3u);

    // Ao contrário (PATH2 por último), vale o do PATH2.
    s.pushPath3(a.data(), a.qwords(), 0x100);
    s.pushPath3(c.data(), c.qwords(), 0x100);
    s.pushVif(d1);
    s.pushVif(d2);
    s.fence();
    CHECK_EQ(e.rt.gs().reg(TEST_1), 2u);
}

// O VIF e o início do VU1 na mesma ordem: MPG grava o microprograma, UNPACK grava o contador e
// CTC2 CMSAR1 (Vu1Start) o executa; MSCAL depois do FBRST (Vu1Reset) vê os registradores zerados.
TEST_CASE(vu1_stream, vif_and_vu1_items_keep_the_order) {
    Env e;
    Vu1Stream& s = e.rt.vu1Stream();
    s.setDeferred(true);
    constexpr std::uint32_t X = 8;
    // Contador em vi5 lido de data1[1002].x, +1, gravado de volta; E-bit e o par de delay slot.
    std::uint32_t prog[] = {lo(0x04, X, 5, 0, 1002), kUNop, lo(0x08, 0, 5, 5, 1), kUNop,
                                  lo(0x05, X, 5, 0, 1002), kUNop, kLNop, kUNop | kEnd, kLNop, kUNop};
    VifItem mpg{VifItem::Kind::Mpg};
    mpg.words = prog;
    mpg.count = sizeof prog / 4;
    mpg.addr = 0;
    VifQwWrite w{1002, 0x1, {7, 0, 0, 0}};
    VifItem up{VifItem::Kind::Unpack};
    up.writes = &w;
    up.count = 1;

    s.pushVif(mpg);
    s.pushVif(up);
    s.pushVu1Start(0, 0x400);
    // Reescrever os dados do item depois do push não muda o que foi enfileirado.
    w.v[0] = 99;
    std::memset(prog, 0, sizeof prog);
    s.fence();
    std::uint32_t counter;
    std::memcpy(&counter, e.rt.vu().data1.get() + 1002 * 16, 4);
    CHECK_EQ(counter, 8u);
    CHECK_EQ(e.rt.vu1().regs().vi[5], 8u);

    // Vu1Reset: os registradores do VU1 voltam a zero, na ordem em que foram pedidos; o contador
    // (na memória de dados) continua, e a segunda execução o leva a 9.
    s.pushVu1Reset();
    s.fence();
    CHECK_EQ(e.rt.vu1().regs().vi[5], 0u);
    s.pushVu1Start(0, 0x400);
    s.fence();
    std::memcpy(&counter, e.rt.vu().data1.get() + 1002 * 16, 4);
    CHECK_EQ(counter, 9u);
}

// Path3 é copiado no enfileiramento: o EE reescreve (ou libera) a memória do DMA e o GS recebe o
// que estava lá no push. O mesmo vale para o DIRECT e o UNPACK, e o resultado adiado é idêntico
// ao inline.
TEST_CASE(vu1_stream, path3_is_copied_at_enqueue) {
    Env deferred, direct;
    deferred.rt.vu1Stream().setDeferred(true);
    for (Env* e : {&deferred, &direct}) {
        Pkt p(2);
        p.ad(TEST_1, 0x11).ad(ALPHA_1, 0x22);
        Vu1Stream& s = e->rt.vu1Stream();
        s.pushPath3(p.data(), p.qwords(), 0x100);
        // A "RAM do DMA" é reaproveitada pelo EE logo depois.
        std::memset(p.data(), 0xEE, p.q.size() * 8);
        s.fence();
        CHECK_EQ(e->rt.gs().reg(TEST_1), 0x11u);
        CHECK_EQ(e->rt.gs().reg(ALPHA_1), 0x22u);
    }
    // DIRECT: o buffer do decodificador é reescrito a cada quadword.
    Env d2;
    Vu1Stream& s = d2.rt.vu1Stream();
    s.setDeferred(true);
    Pkt p(1);
    p.ad(FOG, 0x33ull << 56);
    std::uint8_t buf[16];
    VifItem it{VifItem::Kind::Direct};
    it.data = buf;
    std::memcpy(buf, p.data(), 16);
    s.pushVif(it);
    std::memcpy(buf, p.data() + 16, 16);
    s.pushVif(it);
    std::memset(buf, 0, sizeof buf);
    s.fence();
    CHECK_EQ(d2.rt.gs().reg(FOG), 0x33ull << 56);
}

// SIGNAL, LABEL, FINISH, VSINT e o reset do GS: o consumidor só publica; o EE aplica no espelho e
// pede a interrupção. O oráculo é o Gs isolado (sem stream), que aplica na hora, como antes.
TEST_CASE(vu1_stream, events_use_the_response_queue_with_the_same_csr_and_irq) {
    for (const bool deferred : {false, true}) {
        Env e;
        Vu1Stream& s = e.rt.vu1Stream();
        s.setDeferred(deferred);
        Gs ref(nullptr);  // sem runtime: sem INTC, aplica no espelho próprio
        ref.setHsyncSource(Timing::kCyclesPerLine);
        ref.setRealTimeClock(false);
        Kernel& k = e.rt.kernel();

        s.setImr(0);  // o IMR de partida mascara tudo; aqui as interrupções do GS valem
        e.rt.timing().consume(12345);
        const std::uint64_t t = e.rt.timing().now();
        ref.setItemClock(t);

        // SIGNAL (id 0x1234, máscara 0xFFFF) + LABEL (0xBEEF0000 nos 32 bits altos) + FINISH, pelo PATH3.
        constexpr std::uint64_t kSignalV = 0x1234ull | (0xFFFFull << 32);
        constexpr std::uint64_t kLabelV = 0xBEEF0000ull | (0xFFFF0000ull << 32);
        Pkt p(3);
        p.ad(SIGNAL, kSignalV);
        p.ad(LABEL, kLabelV);
        p.ad(FINISH, 0);
        s.pushPath3(p.data(), p.qwords(), 0x100);
        ref.writeRegister(SIGNAL, kSignalV, 0);
        ref.writeRegister(LABEL, kLabelV, 0);
        ref.writeRegister(FINISH, 0, 0);

        if (deferred) {
            // Nada chegou ao EE: o CSR, o SIGLBLID e o INTC só mudam depois do fence.
            CHECK_EQ(k.intcStat() & 1u, 0u);
            CHECK_EQ(s.eeState().siglblid(), 0u);
            CHECK_EQ(s.eeState().nextEventTime(), ~std::uint64_t{0});
        }
        CHECK_EQ(s.csr(t) & 0xFull, ref.csr() & 0xFull);
        CHECK_EQ(s.csr(t) & 1u, 1u);                               // SIGNAL
        CHECK_EQ(k.intcStat() & 1u, 1u);                           // IMR deixa: INTC_GS
        CHECK_EQ(s.eeState().siglblid(), ref.eeState().siglblid());
        CHECK_EQ(s.eeState().siglblid(), (0xBEEF0000ull << 32) | 0x1234ull);
        CHECK_EQ(s.nextEventTime(), ref.nextEventTime());
        CHECK(s.nextEventTime() != ~std::uint64_t{0});
        // O FINISH vale no seu tempo, no relógio do carimbo (não o do EE no fence).
        e.rt.timing().consume(1000000);
        CHECK_EQ(s.csr(e.rt.timing().now()) & 2u, 0u);
        const std::uint64_t due = s.nextEventTime();
        s.processEvents(due);
        ref.processEvents(due);
        CHECK_EQ(s.csr(due) & 2u, 2u);
        CHECK_EQ(s.csr(due) & 3u, ref.csr() & 3u);
        CHECK_EQ(s.nextEventTime(), ~std::uint64_t{0});

        // Limpar os bits pelo CSR é do EE (direto no espelho); o IMR mascara a interrupção do próximo.
        k.clearIntcStat(1u);
        s.writeGsPrivileged(0x12001000u, 0x3, 0);
        CHECK_EQ(s.csr(due) & 3u, 0u);
        s.writeGsPrivileged(0x12001010u, 0x7F00, 0);  // IMR: tudo mascarado
        CHECK_EQ(s.imr(), 0x7F00u);
        Pkt q(1);
        q.ad(SIGNAL, 1ull | (1ull << 32));
        s.pushPath3(q.data(), q.qwords(), 0x100);
        CHECK_EQ(s.csr(due) & 1u, 1u);       // o bit liga
        CHECK_EQ(k.intcStat() & 1u, 0u);     // mas não há interrupção
        s.setImr(0);

        // VBlank: inverte o campo e liga VSINT com a interrupção.
        const std::uint64_t field = s.csr(due) & (1ull << 13);
        s.pushVblank(due);
        CHECK_EQ(s.csr(due) & (1ull << 13), field ^ (1ull << 13));
        CHECK_EQ(s.csr(due) & 8u, 8u);
        CHECK_EQ(k.intcStat() & 1u, 1u);

        // Reset do GS (CSR.RESET): zera os eventos e os FINISH pendentes, mantém o campo.
        Pkt f(1);
        f.ad(FINISH, 0);
        s.pushPath3(f.data(), f.qwords(), 0x100);
        CHECK(s.nextEventTime() != ~std::uint64_t{0});
        s.writeGsPrivileged(0x12001000u, 0x200, 0);
        CHECK_EQ(s.csr(due) & 0x1Full, 0u);
        CHECK_EQ(s.nextEventTime(), ~std::uint64_t{0});
        CHECK_EQ(s.csr(due) & (1ull << 13), field ^ (1ull << 13));
    }
}

// Registradores de vídeo e BUSDIR são do consumidor: o EE os escreve pela fila e os lê depois de
// um fence; SetGsCrt vira duas escritas.
TEST_CASE(vu1_stream, privileged_registers_go_through_the_queue) {
    Env e;
    Vu1Stream& s = e.rt.vu1Stream();
    s.setDeferred(true);
    s.writeGsPrivileged(0x12000000u, 0x1234, 0);  // PMODE
    s.setCrt(1, 2, 1);
    CHECK_EQ(s.queued(), 3u);
    CHECK_EQ(s.readGsPrivileged(0x12000000u, 0), 0x1234u);  // a leitura passa pelo fence
    CHECK_EQ(s.queued(), 0u);
    CHECK_EQ(s.readGsPrivileged(0x12000010u, 0), 2u << 8);
    CHECK_EQ(s.readGsPrivileged(0x12000020u, 0), 1u | (1u << 1));
    s.writeGsPrivileged(0x12001040u, 1, 0);  // BUSDIR
    s.fence();
    CHECK(e.rt.gs().busDirToHost());
    // Endereço inexistente: o mesmo erro de antes.
    bool threw = false;
    try {
        s.readGsPrivileged(0x12001FF0u, 0x777);
    } catch (const Unimplemented& ex) {
        threw = true;
        CHECK_EQ(ex.pc(), 0x777u);
    }
    CHECK(threw);
}

// Erros do consumidor: guardados e relançados no EE com a classe, a mensagem e o pc do item; no
// inline, no mesmo ponto de antes (dentro do push); adiado, no fence ou no próximo push.
TEST_CASE(vu1_stream, consumer_errors_are_rethrown_in_the_ee) {
    // O oráculo: o Gif isolado.
    std::string expected;
    std::uint32_t expectedPc = 0;
    {
        Gs g(nullptr);
        Gif gif(nullptr, g);
        std::uint64_t tag[2] = {1 | (std::uint64_t{1} << 15) | (std::uint64_t{1} << 60), 0xB};  // PACKED, REGS = 0xB
        std::uint64_t data[2] = {0, 0};
        std::uint8_t bytes[32];
        std::memcpy(bytes, tag, 16);
        std::memcpy(bytes + 16, data, 16);
        try {
            gif.transfer(3, bytes, 2, 0xABC);
        } catch (const Unimplemented& ex) {
            expected = ex.what();
            expectedPc = ex.pc();
        }
    }
    CHECK(!expected.empty());
    CHECK_EQ(expectedPc, 0xABCu);

    std::uint64_t pkt[4] = {1 | (std::uint64_t{1} << 15) | (std::uint64_t{1} << 60), 0xB, 0, 0};
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(pkt);

    // Inline: lançado dentro do push.
    {
        Env e;
        std::string got;
        std::uint32_t pc = 0;
        try {
            e.rt.vu1Stream().pushPath3(bytes, 2, 0xABC);
        } catch (const Unimplemented& ex) {
            got = ex.what();
            pc = ex.pc();
        }
        CHECK_EQ(got, expected);
        CHECK_EQ(pc, expectedPc);
        CHECK(!e.rt.vu1Stream().hasPendingError());
        // O stream continua utilizável depois do erro (o GIF parou no meio do pacote: reset).
        e.rt.vu1Stream().pushGifReset();
        Pkt ok(1);
        ok.ad(TEST_1, 5);
        e.rt.vu1Stream().pushPath3(ok.data(), ok.qwords(), 0);
        CHECK_EQ(e.rt.gs().reg(TEST_1), 5u);
    }
    // Adiado: o push não lança; o fence relança.
    {
        Env e;
        Vu1Stream& s = e.rt.vu1Stream();
        s.setDeferred(true);
        s.pushPath3(bytes, 2, 0xABC);
        Pkt later(1);
        later.ad(TEST_1, 9);
        s.pushPath3(later.data(), later.qwords(), 0x500);  // depois do item que falha: não roda
        std::string got;
        std::uint32_t pc = 0;
        try {
            s.fence();
        } catch (const Unimplemented& ex) {
            got = ex.what();
            pc = ex.pc();
        }
        CHECK_EQ(got, expected);
        CHECK_EQ(pc, expectedPc);
        CHECK_EQ(e.rt.gs().reg(TEST_1), 0u);
        CHECK(!s.hasPendingError());
    }
    // Adiado: sem fence, o próximo enfileiramento a relança (o item consumido por drain).
    {
        Env e;
        Vu1Stream& s = e.rt.vu1Stream();
        s.setDeferred(true);
        s.pushPath3(bytes, 2, 0xABC);
        s.drain();
        CHECK(s.hasPendingError());
        CHECK_EQ(s.pendingErrorSeq(), 1u);
        bool threw = false;
        try {
            s.pushGifReset();
        } catch (const Unimplemented& ex) {
            threw = true;
            CHECK_EQ(std::string(ex.what()), expected);
        }
        CHECK(threw);
    }
    // Escrita em registrador do GIF desconhecido (Unimplemented com o pc do EE), pela fila.
    {
        Env e;
        std::uint32_t pc = 0;
        try {
            e.rt.vu1Stream().pushGifWrite(0x10003030u, 1, 0x1234);
        } catch (const Unimplemented& ex) {
            pc = ex.pc();
        }
        CHECK_EQ(pc, 0x1234u);
    }
}

// Os registradores do GIF e o PATH3 mascarado são lidos depois do fence; a escrita e o reset do
// GIF passam pela fila.
TEST_CASE(vu1_stream, gif_registers_through_the_queue) {
    Env e;
    Vu1Stream& s = e.rt.vu1Stream();
    s.setDeferred(true);
    s.pushGifWrite(0x10003010u, 5, 0);  // GIF_MODE
    VifItem m{VifItem::Kind::Path3Mask};
    m.masked = true;
    s.pushVif(m);
    CHECK_EQ(s.queued(), 2u);
    CHECK(s.path3Masked());  // fence
    CHECK_EQ(s.queued(), 0u);
    CHECK_EQ(s.readGif(0x10003010u, 0), 5u);
    CHECK_EQ(s.readGif(0x10003020u, 0) & 2u, 2u);  // GIF_STAT.M3P
    s.pushGifReset();
    CHECK(!s.path3Masked());
}

// ANYPS2_VU1_THREAD: 0 (ou ausente) = inline; 1 ainda não existe: avisa e cai no inline.
TEST_CASE(vu1_stream, thread_key_is_read_and_falls_back_to_inline) {
    {
        Env e;
        CHECK_EQ(e.rt.vu1Stream().requestedThread(), 0u);
        CHECK(e.rt.vu1Stream().inlineMode());
    }
#ifdef _WIN32
    _putenv_s("ANYPS2_VU1_THREAD", "1");
#else
    setenv("ANYPS2_VU1_THREAD", "1", 1);
#endif
    {
        Env e;
        CHECK_EQ(e.rt.vu1Stream().requestedThread(), 1u);
        CHECK(e.rt.vu1Stream().inlineMode());
        Pkt p(1);
        p.ad(TEST_1, 4);
        e.rt.vu1Stream().pushPath3(p.data(), p.qwords(), 0);  // funciona, na hora
        CHECK_EQ(e.rt.gs().reg(TEST_1), 4u);
    }
#ifdef _WIN32
    _putenv_s("ANYPS2_VU1_THREAD", "0");
#else
    setenv("ANYPS2_VU1_THREAD", "0", 1);
#endif
    {
        Env e;
        CHECK_EQ(e.rt.vu1Stream().requestedThread(), 0u);
    }
}
