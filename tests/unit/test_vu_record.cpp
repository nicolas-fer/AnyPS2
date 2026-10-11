// Testes do gravador/replay do VU1 (vu_record.cpp): um Vu sintético sem Runtime roda um
// microprograma pequeno (FMAC com stall, SQ e XGKICK) com o gravador ligado; o arquivo relido e
// reproduzido numa máquina de replay tem de dar o mesmo hash que a execução direta, e as
// variantes (intervalo de VBlanks, interpretador) também.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "anyps2/common/error.h"
#include "anyps2/runtime/vu/vu.h"
#include "anyps2/runtime/vu/vu_record.h"
#include "minitest.h"

using namespace anyps2::rt;

namespace {

constexpr std::uint32_t kLNop = 0x8000033Cu, kUNop = 0x000002FFu;
constexpr std::uint32_t kSize = 0x4000;

std::uint32_t lo(std::uint32_t op, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs, std::int32_t imm11) {
    return (op << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (static_cast<std::uint32_t>(imm11) & 0x7FF);
}
std::uint32_t lox(std::uint32_t idx, std::uint32_t dest, std::uint32_t ft, std::uint32_t fs) {
    return (0x40u << 25) | (dest << 21) | (ft << 16) | (fs << 11) | ((idx >> 2) << 6) | 0x3C | (idx & 3);
}
std::uint32_t up(std::uint32_t op, std::uint32_t dest, std::uint32_t fd, std::uint32_t fs, std::uint32_t ft) {
    return (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | op;
}

// Um VU1 sem Runtime com o microprograma carregado em 0.
struct Machine {
    Reg128 vf[32]{};
    std::uint32_t vi[32]{};
    Reg128 acc{};
    std::vector<std::uint8_t> data = std::vector<std::uint8_t>(kSize);
    std::vector<std::uint8_t> micro = std::vector<std::uint8_t>(kSize);
    std::vector<VuXgkickEvent> kicks;
    Vu vu{nullptr, 1, vucore::Regs{vf, vi, &acc}, data.data(), kSize, micro.data(), kSize};

    Machine() {
        vu.setXgkickLog(&kicks);
        const std::pair<std::uint32_t, std::uint32_t> prog[] = {
            {kLNop, up(0x28, 0xF, 1, 2, 3)},          // 00 add.xyzw vf1, vf2, vf3 (abre flags pendentes)
            {kLNop, up(0x2A, 0xF, 4, 1, 2)},          // 08 mul.xyzw vf4, vf1, vf2 (espera o vf1)
            {lo(0x01, 0xF, 0, 4, 0x20), kUNop},       // 10 sq.xyzw vf4, 0x20(vi0)
            {lo(0x08, 0, 1, 0, 0x20), kUNop},         // 18 iaddiu vi1, vi0, 0x20
            {lox(0x6C, 0, 0, 1), kUNop},              // 20 xgkick vi1
            {kLNop, kUNop | (1u << 30)},              // 28 nop[e]
            {kLNop, kUNop},                           // 30 (delay slot)
        };
        for (std::size_t i = 0; i < std::size(prog); ++i) {
            std::memcpy(micro.data() + i * 8, &prog[i].first, 4);
            std::memcpy(micro.data() + i * 8 + 4, &prog[i].second, 4);
        }
    }

    // Estado de entrada determinístico, diferente a cada `seed`.
    void load(std::uint32_t seed) {
        for (unsigned r = 1; r < 32; ++r) {
            for (unsigned c = 0; c < 4; ++c) vf[r].uw[c] = 0x3F000000u + (seed * 977u + r * 131u + c * 17u) * 4099u;
        }
        for (std::size_t i = 0; i < kSize; i += 4) {
            const std::uint32_t w = 0x40000000u + static_cast<std::uint32_t>(i) * 7u + seed;
            std::memcpy(data.data() + i, &w, 4);
        }
        kicks.clear();
    }
    VuRunHash runDirect(bool interpret) {
        kicks.clear();
        const std::uint64_t c0 = vu.cycle();
        if (interpret) vu.interpret(0, 0);
        else vu.start(0, 0);
        return hashVuRun(vu, c0, kicks);
    }
};

// Pipeline em voo na entrada: VF ocupado, flags pendentes e um Q em andamento.
VuPipelineSeed busyPipeline() {
    VuPipelineSeed s;
    s.vfBusy[2] = 3;
    s.vfBusy[3] = 1;
    s.flags.push_back({2, true, 0x0102, true, 0x15});
    s.flags.push_back({3, true, 0x0F00, false, 0});
    s.head = 5;
    s.q = {true, 5, 0x3F800000u, 0x3};
    return s;
}

}  // namespace

TEST_CASE(vu_record, replay_gives_the_same_hash_as_direct_execution) {
    const char* path = "anyps2_vu_record_test.avur";
    std::vector<VuRunHash> direct;
    {
        Machine m;
        VuRecorder rec;
        CHECK(rec.open(path));
        m.vu.setRecorder(&rec);
        m.vu.setVifTop(0x10, 0x20);
        for (std::uint32_t i = 0; i < 3; ++i) {
            m.load(i);
            if (i == 1) m.vu.seedPipeline(busyPipeline());
            direct.push_back(m.runDirect(i == 2));
        }
        CHECK_EQ(rec.entries(), 3u);
    }

    const VuRecording r = VuRecording::load(path);
    REQUIRE(r.entries.size() == 3);
    CHECK_EQ(r.microSize, kSize);
    CHECK_EQ(r.dataSize, kSize);
    CHECK(r.entries[0].micro.get() == r.entries[2].micro.get());  // a imagem repetida é guardada uma vez
    CHECK_EQ(r.entries[1].pipe.flags.size(), 2u);
    CHECK_EQ(r.entries[1].vifTop, 0x10u);

    VuReplayMachine machine(r.microSize, r.dataSize, r.unit);
    for (std::size_t i = 0; i < r.entries.size(); ++i) {
        // Por start (sem programas, cai no interpretador) e por interpret: coincidem entre si e
        // com a execução direta.
        const VuRunHash a = machine.replay(r.entries[i], false, true, nullptr);
        const VuRunHash b = machine.replay(r.entries[i], true, true, nullptr);
        CHECK(a == direct[i]);
        CHECK(b == direct[i]);
        CHECK_EQ(direct[i].pairs, 7u);
    }
    // Entradas diferentes dão hashes diferentes (o hash enxerga o estado).
    CHECK(!(direct[0] == direct[1]));
    CHECK(direct[0].diff(direct[1]).find("dados") != std::string::npos);

    // O golden em texto sobrevive ao ida e volta.
    VuRunHash parsed;
    CHECK(VuRunHash::parse(direct[1].str(), parsed));
    CHECK(parsed == direct[1]);
    CHECK(!VuRunHash::parse("lixo", parsed));
    std::remove(path);
}

TEST_CASE(vu_record, records_only_the_requested_vblank_range_and_rejects_bad_files) {
    const char* path = "anyps2_vu_record_test2.avur";
    {
        Machine m;
        VuRecorder rec;
        CHECK(rec.open(path, 5, 6));
        m.vu.setRecorder(&rec);
        m.load(1);
        for (std::uint64_t vb : {4ull, 5ull, 6ull, 7ull}) m.vu.startAt(0, 0, vb);
        CHECK_EQ(rec.entries(), 2u);
    }
    const VuRecording r = VuRecording::load(path);
    REQUIRE(r.entries.size() == 2);
    CHECK_EQ(r.entries[0].vblank, 5u);
    CHECK_EQ(r.entries[1].vblank, 6u);
    std::remove(path);

    {  // arquivo que não é uma gravação
        std::FILE* f = std::fopen(path, "wb");
        REQUIRE(f != nullptr);
        std::fputs("nao e uma gravacao", f);
        std::fclose(f);
    }
    bool threw = false;
    try {
        (void)VuRecording::load(path);
    } catch (const anyps2::Error&) {
        threw = true;
    }
    CHECK(threw);
    std::remove(path);
}
