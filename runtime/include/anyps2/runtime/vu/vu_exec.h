#pragma once

// Execução de um par de instruções do VU (upper + lower), inline.
//
// Incluído pelo interpretador (vu.cpp) e pelo código gerado para os
// microprogramas recompilados: com a instrução constante, o compilador
// elimina a decodificação e os switches e sobra só a operação do par, com o
// mesmo modelo de pipeline (stalls, latência de flags, Q e P) do
// interpretador — os dois caminhos executam literalmente o mesmo código.

#include <algorithm>
#include <cstring>
#include <string>

#include "anyps2/common/error.h"

#include "anyps2/runtime/vu/vu.h"

namespace anyps2::rt {

namespace vuexec {
// Latência (ciclos) do FMAC e do FDIV.
inline constexpr std::uint64_t kFmacLatency = 4;
inline constexpr std::uint64_t fdivLatency(vu::L op) {
    return op == vu::L::RSQRT ? 13 : 7;
}
}  // namespace vuexec

// ---- Pipeline ---------------------------------------------------------------

// Caso comum (nada pendente ficou pronto): uma comparação. nextCommit_ é
// sempre ≤ o primeiro evento pendente (baixá-lo é sempre seguro).
ANYPS2_VU_INLINE void Vu::commitReady() {
    if (cycle_ >= nextCommit_) commitPending();
}

ANYPS2_VU_INLINE void Vu::commitQ() {
    regs_.vi[vucore::reg::Q] = q_.value;
    regs_.vi[vucore::reg::Status] = vucore::statusWithDiv(regs_.vi[vucore::reg::Status], q_.flags);
    q_.active = false;
}

ANYPS2_VU_INLINE void Vu::commitP() {
    regs_.vi[vucore::reg::P] = p_.value;
    p_.active = false;
}

ANYPS2_VU_INLINE void Vu::stallOn(unsigned vf) {
    if (vf != 0) stallUntil(vfReady_[vf]);
}

// UOp/LOp: operação conhecida em tempo de compilação (código recompilado) ou
// COUNT_ = escolhida pela instrução em tempo de execução (interpretador).
template <vu::U UOp, vu::L LOp>
ANYPS2_VU_INLINE void Vu::execPairOp(const vu::Instr& in, std::uint32_t pc, Flow& flow) {
    flow.taken = false;
    pairBegin(in, pc);
    vucore::UpperResult ur;
    if constexpr (UOp == vu::U::COUNT_) {
        ur = vucore::computeUpper(regs_, in.upper, regs_.vi[vucore::reg::Q], regs_.vi[vucore::reg::I]);
    } else {
        ur = vucore::computeUpperOp<UOp>(regs_, in.upper, regs_.vi[vucore::reg::Q], regs_.vi[vucore::reg::I]);
    }
    if (!in.i) {
        if constexpr (LOp == vu::L::COUNT_) execLower(in, pc, flow);
        else execLowerOp<LOp>(in, pc, flow);
    }
    pairEnd(in, ur);
}

ANYPS2_VU_INLINE void Vu::execPair(const vu::Instr& in, std::uint32_t pc, Flow& flow) {
    execPairOp<vu::U::COUNT_, vu::L::COUNT_>(in, pc, flow);
}

template <vu::L Op>
ANYPS2_VU_INLINE void Vu::execLowerOp(const vu::Instr& in, std::uint32_t pc, Flow& flow) {
    const vu::Lower& l = in.lower;
    std::uint32_t* vi = regs_.vi;
    Reg128* vf = regs_.vf;
    [[maybe_unused]] auto setVi = [&](unsigned r, std::uint32_t v) ANYPS2_VU_LAMBDA_INLINE {
        if (r != 0) vi[r] = v & 0xFFFFu;
    };
    [[maybe_unused]] auto viS = [&](unsigned r) ANYPS2_VU_LAMBDA_INLINE { return static_cast<std::int16_t>(vi[r] & 0xFFFF); };
    [[maybe_unused]] auto writeVf = [&](unsigned r, const Reg128& v, unsigned dest) ANYPS2_VU_LAMBDA_INLINE {
        if (r == 0) return;
        for (unsigned c = 0; c < 4; ++c) {
            if (vucore::hasComp(dest, c)) vf[r].uw[c] = v.uw[c];
        }
        vfReady_[r] = cycle_ + vuexec::kFmacLatency;
    };
    [[maybe_unused]] auto firstComp = [](unsigned dest) ANYPS2_VU_LAMBDA_INLINE -> unsigned {
        for (unsigned c = 0; c < 4; ++c) {
            if (vucore::hasComp(dest, c)) return c;
        }
        return 0;
    };
    [[maybe_unused]] auto branchTo = [&](std::uint32_t target) ANYPS2_VU_LAMBDA_INLINE {
        flow.taken = true;
        flow.target = target & (microSize_ - 1);
    };
    [[maybe_unused]] auto rel = [&] { return vu::branchTarget(in, pc); };

    if constexpr (Op == vu::L::NOP || Op == vu::L::WAITQ || Op == vu::L::WAITP || Op == vu::L::LOI) {
        if (l.op == vu::L::WAITQ && q_.active) {
            stallUntil(q_.ready);
            commitQ();
        }
        if (l.op == vu::L::WAITP && p_.active) {
            stallUntil(p_.ready);
            commitP();
        }
    } else if constexpr (Op == vu::L::INVALID) {
        fail("instrução lower inválida " + anyps2::hex(in.lowerWord), pc);
    }
    // ---- Load/store ----------------------------------------------------
    else if constexpr (Op == vu::L::LQ) {
        writeVf(l.ft, mem(static_cast<std::uint32_t>(viS(l.fs) + l.imm), pc), l.dest);
    } else if constexpr (Op == vu::L::SQ) {
        stallOn(l.fs);
        Reg128& m = mem(static_cast<std::uint32_t>(viS(l.ft) + l.imm), pc);
        for (unsigned c = 0; c < 4; ++c) {
            if (vucore::hasComp(l.dest, c)) m.uw[c] = vf[l.fs].uw[c];
        }
    } else if constexpr (Op == vu::L::LQI) {
        writeVf(l.ft, mem(vi[l.fs], pc), l.dest);
        setVi(l.fs, vi[l.fs] + 1);
    } else if constexpr (Op == vu::L::LQD) {
        setVi(l.fs, vi[l.fs] - 1);
        writeVf(l.ft, mem(vi[l.fs], pc), l.dest);
    } else if constexpr (Op == vu::L::SQI || Op == vu::L::SQD) {
        stallOn(l.fs);
        if (l.op == vu::L::SQD) setVi(l.ft, vi[l.ft] - 1);
        Reg128& m = mem(vi[l.ft], pc);
        for (unsigned c = 0; c < 4; ++c) {
            if (vucore::hasComp(l.dest, c)) m.uw[c] = vf[l.fs].uw[c];
        }
        if (l.op == vu::L::SQI) setVi(l.ft, vi[l.ft] + 1);
    } else if constexpr (Op == vu::L::ILW || Op == vu::L::ILWR) {
        const std::uint32_t addr = static_cast<std::uint32_t>(viS(l.fs) + (l.op == vu::L::ILW ? l.imm : 0));
        setVi(l.ft, mem(addr, pc).uw[firstComp(l.dest)]);
    } else if constexpr (Op == vu::L::ISW || Op == vu::L::ISWR) {
        const std::uint32_t addr = static_cast<std::uint32_t>(viS(l.fs) + (l.op == vu::L::ISW ? l.imm : 0));
        Reg128& m = mem(addr, pc);
        for (unsigned c = 0; c < 4; ++c) {
            if (vucore::hasComp(l.dest, c)) m.uw[c] = vi[l.ft] & 0xFFFFu;
        }
    }
    // ---- Inteiros --------------------------------------------------------
    else if constexpr (Op == vu::L::IADD) {
        setVi(l.fd, vi[l.fs] + vi[l.ft]);
    } else if constexpr (Op == vu::L::ISUB) {
        setVi(l.fd, vi[l.fs] - vi[l.ft]);
    } else if constexpr (Op == vu::L::IAND) {
        setVi(l.fd, vi[l.fs] & vi[l.ft]);
    } else if constexpr (Op == vu::L::IOR) {
        setVi(l.fd, vi[l.fs] | vi[l.ft]);
    } else if constexpr (Op == vu::L::IADDI) {
        setVi(l.ft, vi[l.fs] + static_cast<std::uint32_t>(l.imm));
    } else if constexpr (Op == vu::L::IADDIU) {
        setVi(l.ft, vi[l.fs] + static_cast<std::uint32_t>(l.imm));
    } else if constexpr (Op == vu::L::ISUBIU) {
        setVi(l.ft, vi[l.fs] - static_cast<std::uint32_t>(l.imm));
    }
    // ---- Movimentação ----------------------------------------------------
    else if constexpr (Op == vu::L::MOVE) {
        stallOn(l.fs); writeVf(l.ft, vf[l.fs], l.dest);
    } else if constexpr (Op == vu::L::MR32) {
        stallOn(l.fs);
        Reg128 v;
        const Reg128& s = vf[l.fs];
        v.uw[0] = s.uw[1];
        v.uw[1] = s.uw[2];
        v.uw[2] = s.uw[3];
        v.uw[3] = s.uw[0];
        writeVf(l.ft, v, l.dest);
    } else if constexpr (Op == vu::L::MFIR) {
        Reg128 v;
        const auto x = static_cast<std::uint32_t>(static_cast<std::int32_t>(viS(l.fs)));
        v.uw[0] = v.uw[1] = v.uw[2] = v.uw[3] = x;
        writeVf(l.ft, v, l.dest);
    } else if constexpr (Op == vu::L::MTIR) {
        stallOn(l.fs); setVi(l.ft, vf[l.fs].uw[l.fsf]);
    } else if constexpr (Op == vu::L::MFP) {
        commitReady();
        Reg128 v;
        v.uw[0] = v.uw[1] = v.uw[2] = v.uw[3] = regs_.vi[vucore::reg::P];
        writeVf(l.ft, v, l.dest);
    }
    // ---- FDIV --------------------------------------------------------------
    else if constexpr (Op == vu::L::DIV || Op == vu::L::SQRT || Op == vu::L::RSQRT) {
        stallOn(l.fs);
        stallOn(l.ft);
        if (q_.active) {  // FDIV ocupado: espera o anterior
            stallUntil(q_.ready);
            commitQ();
        }
        const vucore::DivResult d = vucore::fdiv(l.op, vf[l.fs].uw[l.fsf], vf[l.ft].uw[l.ftf]);
        q_ = {true, cycle_ + vuexec::fdivLatency(l.op), d.q, d.flags};
        nextCommit_ = std::min(nextCommit_, q_.ready);
    }
    // ---- R -----------------------------------------------------------------
    else if constexpr (Op == vu::L::RINIT) {
        stallOn(l.fs);
        vi[vucore::reg::R] = 0x3F800000u | (vf[l.fs].uw[l.fsf] & 0x007FFFFFu);
    } else if constexpr (Op == vu::L::RXOR) {
        stallOn(l.fs);
        vi[vucore::reg::R] = 0x3F800000u | ((vi[vucore::reg::R] ^ vf[l.fs].uw[l.fsf]) & 0x007FFFFFu);
    } else if constexpr (Op == vu::L::RGET || Op == vu::L::RNEXT) {
        if (l.op == vu::L::RNEXT) vi[vucore::reg::R] = vucore::advanceR(vi[vucore::reg::R]);
        Reg128 v;
        v.uw[0] = v.uw[1] = v.uw[2] = v.uw[3] = vi[vucore::reg::R];
        writeVf(l.ft, v, l.dest);
    }
    // ---- Flags ---------------------------------------------------------------
    else if constexpr (Op == vu::L::FSAND) {
        setVi(l.ft, vi[vucore::reg::Status] & static_cast<std::uint32_t>(l.imm));
    } else if constexpr (Op == vu::L::FSOR) {
        setVi(l.ft, (vi[vucore::reg::Status] & 0xFFF) | static_cast<std::uint32_t>(l.imm));
    } else if constexpr (Op == vu::L::FSEQ) {
        setVi(l.ft, (vi[vucore::reg::Status] & 0xFFF) == static_cast<std::uint32_t>(l.imm) ? 1 : 0);
    } else if constexpr (Op == vu::L::FSSET) {
        vi[vucore::reg::Status] = (vi[vucore::reg::Status] & 0x3Fu) | (static_cast<std::uint32_t>(l.imm) & 0xFC0u);
    } else if constexpr (Op == vu::L::FMAND) {
        setVi(l.ft, vi[vucore::reg::Mac] & vi[l.fs]);
    } else if constexpr (Op == vu::L::FMOR) {
        setVi(l.ft, vi[vucore::reg::Mac] | vi[l.fs]);
    } else if constexpr (Op == vu::L::FMEQ) {
        setVi(l.ft, (vi[vucore::reg::Mac] & 0xFFFF) == (vi[l.fs] & 0xFFFF) ? 1 : 0);
    } else if constexpr (Op == vu::L::FCAND) {
        setVi(1, (vi[vucore::reg::Clip] & static_cast<std::uint32_t>(l.imm)) ? 1 : 0);
    } else if constexpr (Op == vu::L::FCOR) {
        setVi(1, ((vi[vucore::reg::Clip] | static_cast<std::uint32_t>(l.imm)) & 0xFFFFFFu) == 0xFFFFFFu ? 1 : 0);
    } else if constexpr (Op == vu::L::FCEQ) {
        setVi(1, (vi[vucore::reg::Clip] & 0xFFFFFFu) == static_cast<std::uint32_t>(l.imm) ? 1 : 0);
    } else if constexpr (Op == vu::L::FCSET) {
        vi[vucore::reg::Clip] = static_cast<std::uint32_t>(l.imm) & 0xFFFFFFu;
    } else if constexpr (Op == vu::L::FCGET) {
        setVi(l.ft, vi[vucore::reg::Clip] & 0xFFF);
    }
    // ---- Desvios ---------------------------------------------------------------
    else if constexpr (Op == vu::L::B) {
        branchTo(rel());
    } else if constexpr (Op == vu::L::BAL) {
        setVi(l.ft, (pc + 16) / 8); branchTo(rel());
    } else if constexpr (Op == vu::L::JR) {
        branchTo((vi[l.fs] & 0xFFFF) * 8);
    } else if constexpr (Op == vu::L::JALR) {
        const std::uint32_t target = (vi[l.fs] & 0xFFFF) * 8;
        setVi(l.ft, (pc + 16) / 8);
        branchTo(target);
    } else if constexpr (Op == vu::L::IBEQ) {
        if ((vi[l.ft] & 0xFFFF) == (vi[l.fs] & 0xFFFF)) branchTo(rel());
    } else if constexpr (Op == vu::L::IBNE) {
        if ((vi[l.ft] & 0xFFFF) != (vi[l.fs] & 0xFFFF)) branchTo(rel());
    } else if constexpr (Op == vu::L::IBLTZ) {
        if (viS(l.fs) < 0) branchTo(rel());
    } else if constexpr (Op == vu::L::IBGTZ) {
        if (viS(l.fs) > 0) branchTo(rel());
    } else if constexpr (Op == vu::L::IBLEZ) {
        if (viS(l.fs) <= 0) branchTo(rel());
    } else if constexpr (Op == vu::L::IBGEZ) {
        if (viS(l.fs) >= 0) branchTo(rel());
    }
    // ---- VU1 <-> VIF/GIF -----------------------------------------------------------
    else if constexpr (Op == vu::L::XTOP || Op == vu::L::XITOP) {
        setVi(l.ft, vifTop(l.op == vu::L::XITOP, pc));
    } else if constexpr (Op == vu::L::XGKICK) {
        if (unit_ != 1) fail("XGKICK só existe no VU1", pc);
        xgkick((vi[l.fs] & 0x3FF) * 16, pc);
    } else {
        execEfu(l, pc);
    }
}

ANYPS2_VU_INLINE void Vu::execLower(const vu::Instr& in, std::uint32_t pc, Flow& flow) {
    switch (in.lower.op) {
        case vu::L::LQ: execLowerOp<vu::L::LQ>(in, pc, flow); return;
        case vu::L::SQ: execLowerOp<vu::L::SQ>(in, pc, flow); return;
        case vu::L::ILW: execLowerOp<vu::L::ILW>(in, pc, flow); return;
        case vu::L::ISW: execLowerOp<vu::L::ISW>(in, pc, flow); return;
        case vu::L::IADDIU: execLowerOp<vu::L::IADDIU>(in, pc, flow); return;
        case vu::L::ISUBIU: execLowerOp<vu::L::ISUBIU>(in, pc, flow); return;
        case vu::L::FCEQ: execLowerOp<vu::L::FCEQ>(in, pc, flow); return;
        case vu::L::FCSET: execLowerOp<vu::L::FCSET>(in, pc, flow); return;
        case vu::L::FCAND: execLowerOp<vu::L::FCAND>(in, pc, flow); return;
        case vu::L::FCOR: execLowerOp<vu::L::FCOR>(in, pc, flow); return;
        case vu::L::FSEQ: execLowerOp<vu::L::FSEQ>(in, pc, flow); return;
        case vu::L::FSSET: execLowerOp<vu::L::FSSET>(in, pc, flow); return;
        case vu::L::FSAND: execLowerOp<vu::L::FSAND>(in, pc, flow); return;
        case vu::L::FSOR: execLowerOp<vu::L::FSOR>(in, pc, flow); return;
        case vu::L::FMEQ: execLowerOp<vu::L::FMEQ>(in, pc, flow); return;
        case vu::L::FMAND: execLowerOp<vu::L::FMAND>(in, pc, flow); return;
        case vu::L::FMOR: execLowerOp<vu::L::FMOR>(in, pc, flow); return;
        case vu::L::FCGET: execLowerOp<vu::L::FCGET>(in, pc, flow); return;
        case vu::L::B: execLowerOp<vu::L::B>(in, pc, flow); return;
        case vu::L::BAL: execLowerOp<vu::L::BAL>(in, pc, flow); return;
        case vu::L::JR: execLowerOp<vu::L::JR>(in, pc, flow); return;
        case vu::L::JALR: execLowerOp<vu::L::JALR>(in, pc, flow); return;
        case vu::L::IBEQ: execLowerOp<vu::L::IBEQ>(in, pc, flow); return;
        case vu::L::IBNE: execLowerOp<vu::L::IBNE>(in, pc, flow); return;
        case vu::L::IBLTZ: execLowerOp<vu::L::IBLTZ>(in, pc, flow); return;
        case vu::L::IBGTZ: execLowerOp<vu::L::IBGTZ>(in, pc, flow); return;
        case vu::L::IBLEZ: execLowerOp<vu::L::IBLEZ>(in, pc, flow); return;
        case vu::L::IBGEZ: execLowerOp<vu::L::IBGEZ>(in, pc, flow); return;
        case vu::L::IADD: execLowerOp<vu::L::IADD>(in, pc, flow); return;
        case vu::L::ISUB: execLowerOp<vu::L::ISUB>(in, pc, flow); return;
        case vu::L::IADDI: execLowerOp<vu::L::IADDI>(in, pc, flow); return;
        case vu::L::IAND: execLowerOp<vu::L::IAND>(in, pc, flow); return;
        case vu::L::IOR: execLowerOp<vu::L::IOR>(in, pc, flow); return;
        case vu::L::MOVE: execLowerOp<vu::L::MOVE>(in, pc, flow); return;
        case vu::L::MR32: execLowerOp<vu::L::MR32>(in, pc, flow); return;
        case vu::L::LQI: execLowerOp<vu::L::LQI>(in, pc, flow); return;
        case vu::L::SQI: execLowerOp<vu::L::SQI>(in, pc, flow); return;
        case vu::L::LQD: execLowerOp<vu::L::LQD>(in, pc, flow); return;
        case vu::L::SQD: execLowerOp<vu::L::SQD>(in, pc, flow); return;
        case vu::L::DIV: execLowerOp<vu::L::DIV>(in, pc, flow); return;
        case vu::L::SQRT: execLowerOp<vu::L::SQRT>(in, pc, flow); return;
        case vu::L::RSQRT: execLowerOp<vu::L::RSQRT>(in, pc, flow); return;
        case vu::L::WAITQ: execLowerOp<vu::L::WAITQ>(in, pc, flow); return;
        case vu::L::MTIR: execLowerOp<vu::L::MTIR>(in, pc, flow); return;
        case vu::L::MFIR: execLowerOp<vu::L::MFIR>(in, pc, flow); return;
        case vu::L::ILWR: execLowerOp<vu::L::ILWR>(in, pc, flow); return;
        case vu::L::ISWR: execLowerOp<vu::L::ISWR>(in, pc, flow); return;
        case vu::L::RNEXT: execLowerOp<vu::L::RNEXT>(in, pc, flow); return;
        case vu::L::RGET: execLowerOp<vu::L::RGET>(in, pc, flow); return;
        case vu::L::RINIT: execLowerOp<vu::L::RINIT>(in, pc, flow); return;
        case vu::L::RXOR: execLowerOp<vu::L::RXOR>(in, pc, flow); return;
        case vu::L::MFP: execLowerOp<vu::L::MFP>(in, pc, flow); return;
        case vu::L::XTOP: execLowerOp<vu::L::XTOP>(in, pc, flow); return;
        case vu::L::XITOP: execLowerOp<vu::L::XITOP>(in, pc, flow); return;
        case vu::L::XGKICK: execLowerOp<vu::L::XGKICK>(in, pc, flow); return;
        case vu::L::ESADD: execLowerOp<vu::L::ESADD>(in, pc, flow); return;
        case vu::L::ERSADD: execLowerOp<vu::L::ERSADD>(in, pc, flow); return;
        case vu::L::ELENG: execLowerOp<vu::L::ELENG>(in, pc, flow); return;
        case vu::L::ERLENG: execLowerOp<vu::L::ERLENG>(in, pc, flow); return;
        case vu::L::EATANxy: execLowerOp<vu::L::EATANxy>(in, pc, flow); return;
        case vu::L::EATANxz: execLowerOp<vu::L::EATANxz>(in, pc, flow); return;
        case vu::L::ESUM: execLowerOp<vu::L::ESUM>(in, pc, flow); return;
        case vu::L::ESQRT: execLowerOp<vu::L::ESQRT>(in, pc, flow); return;
        case vu::L::ERSQRT: execLowerOp<vu::L::ERSQRT>(in, pc, flow); return;
        case vu::L::ERCPR: execLowerOp<vu::L::ERCPR>(in, pc, flow); return;
        case vu::L::WAITP: execLowerOp<vu::L::WAITP>(in, pc, flow); return;
        case vu::L::ESIN: execLowerOp<vu::L::ESIN>(in, pc, flow); return;
        case vu::L::EATAN: execLowerOp<vu::L::EATAN>(in, pc, flow); return;
        case vu::L::EEXP: execLowerOp<vu::L::EEXP>(in, pc, flow); return;
        case vu::L::NOP: execLowerOp<vu::L::NOP>(in, pc, flow); return;
        case vu::L::LOI: execLowerOp<vu::L::LOI>(in, pc, flow); return;
        case vu::L::INVALID: execLowerOp<vu::L::INVALID>(in, pc, flow); return;
        default: execLowerOp<vu::L::INVALID>(in, pc, flow); return;
    }
}

// ---- Laço: um par com delay slot e bit E -------------------------------------

ANYPS2_VU_INLINE void Vu::countPair(std::uint32_t pc) {
    if (++pairs_ > kMaxPairs) tooManyPairs(pc);
}

template <vu::U UOp, vu::L LOp>
ANYPS2_VU_INLINE void Vu::step(const vu::Instr& in, std::uint32_t pc, VuCursor& cur) {
    countPair(pc);
    const bool endAfter = cur.ending;  // este par é o delay slot do bit E
    Flow flow;
    execPairOp<UOp, LOp>(in, pc, flow);
    std::uint32_t next = wrap(pc + 8);
    if (cur.hasDelayed) {
        next = cur.delayedTarget;
        cur.hasDelayed = false;
    }
    if (flow.taken) {
        cur.hasDelayed = true;
        cur.delayedTarget = flow.target;
    }
    cur.pc = next;
    if (endAfter) {
        cur.done = true;
        return;
    }
    cur.ending = in.e;
}

inline bool Vu::pairIs(std::uint32_t pc, std::uint32_t lowerWord, std::uint32_t upperWord) const {
    std::uint32_t w[2];
    std::memcpy(w, micro_ + pc, 8);
    return w[0] == lowerWord && w[1] == upperWord;
}

}  // namespace anyps2::rt
