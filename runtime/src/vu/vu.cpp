#include "anyps2/runtime/vu/vu.h"
#include "anyps2/runtime/host_profile.h"

#include <filesystem>
#include <fstream>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "anyps2/common/error.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/vif.h"
#include "anyps2/runtime/vu/vu_exec.h"
#include "anyps2/runtime/vu/vu_trace.h"

namespace anyps2::rt {

using vu::L;
using vu::U;
using namespace vucore;

namespace {

// Latências (ciclos) do EFU.
std::uint64_t efuLatency(L op) {
    switch (op) {
        case L::ESADD: return 11;
        case L::ERSADD: return 18;
        case L::ELENG: return 18;
        case L::ERLENG: return 24;
        case L::EATANxy: case L::EATANxz: case L::EATAN: return 54;
        case L::ESUM: return 12;
        case L::ESQRT: return 12;
        case L::ERSQRT: return 18;
        case L::ERCPR: return 12;
        case L::ESIN: return 29;
        case L::EEXP: return 44;
        default: return 1;
    }
}


}  // namespace

Vu::Vu(Runtime* rt, unsigned unit, vucore::Regs regs, std::uint8_t* data, std::uint32_t dataSize,
       std::uint8_t* micro, std::uint32_t microSize)
    : rt_(rt),
      unit_(unit),
      regs_(regs),
      data_(data),
      dataSize_(dataSize),
      micro_(micro),
      microSize_(microSize),
      cache_(microSize / 8) {
    regs_.vf[0].uw[0] = regs_.vf[0].uw[1] = regs_.vf[0].uw[2] = 0;
    regs_.vf[0].uw[3] = 0x3F800000u;
    regs_.vi[reg::R] = 0x3F800000u;
}

void Vu::reset() {
    for (unsigned i = 1; i < 32; ++i) regs_.vf[i] = Reg128{};
    for (unsigned i = 0; i < 16; ++i) regs_.vi[i] = 0;
    *regs_.acc = Reg128{};
    regs_.vi[reg::Status] = regs_.vi[reg::Mac] = regs_.vi[reg::Clip] = 0;
    regs_.vi[reg::Q] = regs_.vi[reg::I] = regs_.vi[reg::P] = 0;
    regs_.vi[reg::R] = 0x3F800000u;
    pendCount_ = 0;
    q_ = {};
    p_ = {};
    nextCommit_ = kNoCommit;
}

void Vu::setPrograms(const VuProgramEntry* programs, std::size_t count) {
    index_.clear();
    for (std::size_t i = 0; i < count; ++i) {
        const VuProgramEntry& p = programs[i];
        for (std::uint32_t w = 0; w + 1 < p.wordCount; w += 2) {
            const std::uint64_t key = (std::uint64_t{p.words[w + 1]} << 32) | p.words[w];
            index_.emplace(key, BlockRef{&p, w * 4});
        }
    }
}

void Vu::seedPipeline(const VuPipelineSeed& seed) {
    for (unsigned i = 1; i < 32; ++i) vfReady_[i] = cycle_ + seed.vfBusy[i];
    accReady_ = cycle_ + seed.accBusy;
    std::vector<VuPipelineSeed::Flags> flags = seed.flags;
    if (flags.size() > kPending) flags.resize(kPending);
    std::stable_sort(flags.begin(), flags.end(),
                     [](const auto& a, const auto& b) { return a.delay < b.delay; });  // a fila é por ordem de chegada
    pendHead_ = seed.head % kPending;
    pendCount_ = static_cast<unsigned>(flags.size());
    for (unsigned i = 0; i < pendCount_; ++i) {
        const auto& f = flags[i];
        pending_[(pendHead_ + i) % kPending] = {cycle_ + f.delay, f.hasMac, f.mac, f.hasClip, f.clip};
    }
    q_ = {seed.q.active, cycle_ + seed.q.delay, seed.q.value, seed.q.flags};
    p_ = {seed.p.active, cycle_ + seed.p.delay, seed.p.value, 0};
    nextCommit_ = kNoCommit;
    if (pendCount_ > 0) nextCommit_ = pending_[pendHead_].ready;
    if (q_.active) nextCommit_ = std::min(nextCommit_, q_.ready);
    if (p_.active) nextCommit_ = std::min(nextCommit_, p_.ready);
}

std::string Vu::where(std::uint32_t pc) const {
    return "VU" + std::to_string(unit_) + " em " + anyps2::hex(pc, 4) + " (microprograma iniciado em " +
           anyps2::hex(startPc_, 4) + (rt_ ? " pelo EE em " + rt_->describe(eePc_) : std::string()) + ")";
}

std::string Vu::macroWhere() const {
    return "macroinstrução do VU0 em " + (rt_ ? rt_->describe(eePc_) : anyps2::hex(eePc_));
}

void Vu::fail(const std::string& what, std::uint32_t pc) const {
    throw Unimplemented(what + " — " + (inMacro_ ? macroWhere() : where(pc)), eePc_);
}

// ---------------------------------------------------------------------------
// Pipeline
// ---------------------------------------------------------------------------

void Vu::commitPending() {
    ANYPS2_VU_STAT(++stats_.commitPendingCalls);
    while (pendCount_ > 0 && pending_[pendHead_].ready <= cycle_) {
        ANYPS2_VU_STAT(++stats_.flagsCommitted);
        const PendingFlags& p = pending_[pendHead_];
        if (p.hasMac) {
            regs_.vi[reg::Mac] = p.mac;
            regs_.vi[reg::Status] = statusFromMac(regs_.vi[reg::Status], p.mac);
        }
        if (p.hasClip) regs_.vi[reg::Clip] = ((regs_.vi[reg::Clip] << 6) | p.clip) & 0xFFFFFFu;
        pendHead_ = (pendHead_ + 1) % kPending;
        --pendCount_;
    }
    if (q_.active && q_.ready <= cycle_) {
        ANYPS2_VU_STAT(++stats_.qCommitted);
        commitQ();
    }
    if (p_.active && p_.ready <= cycle_) {
        ANYPS2_VU_STAT(++stats_.pCommitted);
        commitP();
    }
    nextCommit_ = kNoCommit;
    if (pendCount_ > 0) nextCommit_ = pending_[pendHead_].ready;
    if (q_.active) nextCommit_ = std::min(nextCommit_, q_.ready);
    if (p_.active) nextCommit_ = std::min(nextCommit_, p_.ready);
}

// Caminhos de erro de pairBegin, fora de linha: o caminho comum (em vu_exec.h)
// só testa a condição e chama isto.
void Vu::pairRejected(const vu::Instr& in, std::uint32_t pc) const {
    if (in.upper.op == U::INVALID) {
        fail("instrução upper inválida " + anyps2::hex(in.upperWord), pc);
    }
    fail(std::string("bit ") + (in.d ? "D" : "T") + " (parada de depuração do VU) não emulado", pc);
}

void Vu::finish() {
    // Conclui tudo o que está no pipeline: o EE (CFC2) e o próximo
    // microprograma veem o estado final.
    while (pendCount_ > 0) {
        stallUntil(pending_[pendHead_].ready);
        commitReady();
    }
    if (q_.active) commitQ();
    if (p_.active) commitP();
}

void Vu::tooManyPairs(std::uint32_t pc) const {
    throw GuestError("microprograma não terminou após 200 milhões de instruções (laço infinito?) — " + where(pc),
                     eePc_);
}

// ---------------------------------------------------------------------------
// Memória de dados
// ---------------------------------------------------------------------------

// VU0 enxerga os registradores do VU1 em 0x4000–0x43FF. Fica fora de linha:
// o acesso comum à memória de dados (mem, em vu_exec.h) nem chega aqui.
Reg128& Vu::memVu0Cross(std::uint32_t index, std::uint32_t pc) {
    if (!rt_) fail("acesso aos registradores do VU1 pelo VU0 sem runtime", pc);
    const vucore::Regs& r1 = rt_->vu1().regs();
    const std::uint32_t n = index & 0x3F;
    if (n < 32) return r1.vf[n];
    fail("acesso do VU0 aos registradores inteiros/controle do VU1 (" + anyps2::hex(index * 16, 4) +
             ") ainda não suportado",
         pc);
    return r1.vf[0];  // inalcançável: fail() lança
}

// ---------------------------------------------------------------------------
// Execução
// ---------------------------------------------------------------------------

void Vu::execEfu(const vu::Lower& l, std::uint32_t pc) {
    if (unit_ != 1) fail(std::string("instrução do EFU ") + vu::lowerName(l.op) + " (só existe no VU1)", pc);
    stallOn(l.fs);
    if (p_.active) {
        stallUntil(p_.ready);
        commitP();
    }
    const Reg128& s = regs_.vf[l.fs];
    const float x = ps2f::in(s.uw[0]), y = ps2f::in(s.uw[1]), z = ps2f::in(s.uw[2]), w = ps2f::in(s.uw[3]);
    const float f = ps2f::in(s.uw[l.fsf]);
    float r = 0;
    switch (l.op) {
        case L::ESADD: r = x * x + y * y + z * z; break;
        case L::ERSADD: r = 1.0f / (x * x + y * y + z * z); break;
        case L::ELENG: r = std::sqrt(x * x + y * y + z * z); break;
        case L::ERLENG: r = 1.0f / std::sqrt(x * x + y * y + z * z); break;
        case L::EATANxy: r = std::atan2(y, x); break;
        case L::EATANxz: r = std::atan2(z, x); break;
        case L::ESUM: r = x + y + z + w; break;
        case L::ESQRT: r = std::sqrt(std::fabs(f)); break;
        case L::ERSQRT: r = 1.0f / std::sqrt(std::fabs(f)); break;
        case L::ERCPR: r = 1.0f / f; break;
        case L::ESIN: r = std::sin(f); break;
        case L::EATAN: r = std::atan(f); break;
        case L::EEXP: r = std::exp(-f); break;
        default: fail("instrução lower não tratada " + std::string(vu::lowerName(l.op)), pc);
    }
    p_ = {true, cycle_ + efuLatency(l.op), ps2f::out(r).bits, 0};
    nextCommit_ = std::min(nextCommit_, p_.ready);
}

std::uint32_t Vu::vifTop(bool itop, std::uint32_t pc) {
    if (unit_ != 1 || !rt_) fail(std::string(itop ? "XITOP" : "XTOP") + " só existe no VU1", pc);
    return itop ? rt_->vif1().itop() : rt_->vif1().top();
}

void Vu::xgkick(std::uint32_t addr, std::uint32_t pc) {
    ANYPS2_VU_STAT(++stats_.xgkicks);
    if (xgkickLog_) xgkickLog_->push_back({addr, cycle_});
    if (!rt_ && xgkickLog_) return;
    if (!rt_) fail("XGKICK sem runtime", pc);
    if (tracing_) trace_->kick(*this, addr, pc);  // antes do envio: se o GIF falhar, o registro fica
    rt_->gif().kick(data_, dataSize_, addr, pc);
}

void Vu::macro(const vu::Instr& in, std::uint32_t eePc) {
    eePc_ = eePc;
    inMacro_ = true;
    Flow flow;
    try {
        execPair(in, 0, flow);
    } catch (...) {
        inMacro_ = false;
        throw;
    }
    inMacro_ = false;
    finish();
}

// ---------------------------------------------------------------------------
// Laço: código recompilado + interpretador
// ---------------------------------------------------------------------------

const vu::Instr& Vu::fetch(std::uint32_t at) {
    Cached& c = cache_[at / 8];
    std::uint32_t w[2];
    std::memcpy(w, micro_ + at, 8);
    if (!c.valid || c.lo != w[0] || c.up != w[1]) {
        c.in = vu::decode(w[0], w[1]);
        c.lo = w[0];
        c.up = w[1];
        c.valid = true;
    }
    return c.in;
}

// Escolhe o bloco recompilado para continuar em pc: entre os blocos que têm
// o par de pc, o que coincide com a micro memória por mais pares adiante
// (empate: o que foi carregado no endereço conhecido pelo MPG).
Vu::Match Vu::findBlock(std::uint32_t pc) const {
    ANYPS2_VU_STAT(++stats_.findBlockCalls);
    std::uint32_t w[2];
    std::memcpy(w, micro_ + pc, 8);
    const auto range = index_.equal_range((std::uint64_t{w[1]} << 32) | w[0]);
    Match best;
    unsigned bestScore = 0;
    bool bestHint = false;
    constexpr unsigned kWindow = 32;
    for (auto it = range.first; it != range.second; ++it) {
        const VuProgramEntry& p = *it->second.entry;
        const std::uint32_t off = it->second.offset;
        if (off > pc) continue;
        const std::uint32_t base = pc - off;
        if (base + p.wordCount * 4 > microSize_) continue;
        unsigned score = 0;
        for (std::uint32_t o = off; o + 8 <= p.wordCount * 4 && score < kWindow; o += 8, ++score) {
            if (std::memcmp(micro_ + base + o, p.words + o / 4, 8) != 0) break;
        }
        const bool hint = p.loadHint == base;
        if (score > bestScore || (score == bestScore && hint && !bestHint)) {
            best = {&p, base};
            bestScore = score;
            bestHint = hint;
        }
    }
    return best;
}

void Vu::start(std::uint32_t pc, std::uint32_t eePc) {
    run(pc, eePc, mode_ != Mode::Interpret);
}

void Vu::interpret(std::uint32_t pc, std::uint32_t eePc) {
    run(pc, eePc, false);
}

void Vu::run(std::uint32_t pc, std::uint32_t eePc, bool compiled) {
    const HostProfile::Scope prof(unit_ == 0 ? HostProfile::Vu0 : HostProfile::Vu1);
    startPc_ = pc & (microSize_ - 8);
    eePc_ = eePc;
    pairs_ = 0;
    if (rt_ && rt_->options().traceGs) {
        std::fprintf(stderr, "[vu%u] início em 0x%04x\n", unit_, startPc_);
    }
    if (tracing_) trace_->beginRun(*this, startPc_, eePc, rt_ ? rt_->vblanks() : 0);
    VuCursor cur;
    cur.pc = startPc_;
    std::uint64_t viaCompiled = 0, viaInterpreter = 0;
    while (!cur.done) {
        if (compiled && !index_.empty()) {
            const Match m = findBlock(cur.pc);
            if (m.entry) {
                ANYPS2_VU_STAT(++stats_.findBlockHits);
                const std::uint64_t before = pairs_;
                m.entry->fn(*this, m.base, cur);
                viaCompiled += pairs_ - before;
                if (pairs_ != before) continue;
            }
        }
        if (compiled && mode_ == Mode::CompiledOnly) {
            fail("par sem versão recompilada (ANYPS2_VU=compiled): " + vu::disassemble(fetch(cur.pc), cur.pc), cur.pc);
        }
        // Interpreta até o fluxo sair da sequência (desvio): no alvo pode
        // haver um bloco recompilado.
        for (;;) {
            const std::uint32_t at = cur.pc;
            step(fetch(at), at, cur);
            ++viaInterpreter;
            if (cur.done || cur.pc != wrap(at + 8)) break;
        }
    }
    finish();
    ANYPS2_VU_STAT((++stats_.starts, stats_.pairs += pairs_, stats_.maxPairsPerStart = std::max(stats_.maxPairsPerStart, pairs_)));
    regs_.vi[reg::TPC] = cur.pc / 8;
    compiledPairs_ += viaCompiled;
    interpretedPairs_ += viaInterpreter;
    if (viaCompiled) ++compiledRuns_;
    else ++interpretedRuns_;
    if (viaInterpreter && !dumpDir_.empty()) dumpMicro();
}

void Vu::dumpMicro() {
    std::uint64_t h = 0xCBF29CE484222325ull;  // FNV-1a
    for (std::uint32_t i = 0; i < microSize_; ++i) {
        h ^= micro_[i];
        h *= 0x100000001B3ull;
    }
    if (!dumped_.insert(h).second) return;
    char name[48];
    std::snprintf(name, sizeof(name), "vu%u_%016llx.bin", unit_, static_cast<unsigned long long>(h));
    const std::filesystem::path path = std::filesystem::path(dumpDir_) / name;
    std::error_code ec;
    std::filesystem::create_directories(dumpDir_, ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) throw anyps2::Error("ANYPS2_VU_DUMP: não foi possível criar " + path.string());
    f.write(reinterpret_cast<const char*>(micro_), static_cast<std::streamsize>(microSize_));
}

}  // namespace anyps2::rt
