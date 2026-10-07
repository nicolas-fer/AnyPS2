#pragma once

#include <cstdint>

#include "anyps2/r5900/opcodes.h"

namespace anyps2::r5900 {

// Uma instrução decodificada. É um objeto leve (12 bytes): os campos são
// extraídos sob demanda da palavra original, e as propriedades semânticas
// vêm da tabela OpInfo.
struct Instruction {
    std::uint32_t address = 0;  // endereço virtual da instrução
    std::uint32_t raw = 0;      // palavra de 32 bits original
    Op op = Op::Invalid;
    bool canonical = true;      // false se bits reservados estão ligados

    // ---- Estado da decodificação ---------------------------------------
    bool valid() const { return op != Op::Invalid; }
    const OpInfo& info() const { return opInfo(op); }
    std::uint32_t flags() const { return info().flags; }
    bool has(std::uint32_t flag) const { return (flags() & flag) != 0; }

    // ---- Campos brutos ---------------------------------------------------
    std::uint32_t opcode() const { return raw >> 26; }
    std::uint32_t rs() const { return (raw >> 21) & 0x1F; }
    std::uint32_t rt() const { return (raw >> 16) & 0x1F; }
    std::uint32_t rd() const { return (raw >> 11) & 0x1F; }
    std::uint32_t sa() const { return (raw >> 6) & 0x1F; }
    std::uint32_t funct() const { return raw & 0x3F; }
    std::uint16_t imm16() const { return static_cast<std::uint16_t>(raw & 0xFFFF); }
    std::int32_t simm16() const { return static_cast<std::int16_t>(raw & 0xFFFF); }
    std::uint32_t target26() const { return raw & 0x03FFFFFF; }

    // COP1: registradores de ponto flutuante.
    std::uint32_t ft() const { return rt(); }
    std::uint32_t fs() const { return rd(); }
    std::uint32_t fd() const { return sa(); }

    // COP2 / VU0 em modo macro.
    std::uint32_t vuDest() const { return (raw >> 21) & 0xF; }  // bit3=x bit2=y bit1=z bit0=w
    std::uint32_t vuFt() const { return rt(); }
    std::uint32_t vuFs() const { return rd(); }
    std::uint32_t vuFd() const { return sa(); }
    std::uint32_t vuBc() const { return raw & 0x3; }          // 0=x 1=y 2=z 3=w
    std::uint32_t vuFsf() const { return (raw >> 21) & 0x3; } // componente de fs
    std::uint32_t vuFtf() const { return (raw >> 23) & 0x3; } // componente de ft
    std::int32_t vuImm5() const {                             // VIADDI
        auto v = static_cast<std::int32_t>(sa());
        return (v & 0x10) ? v - 0x20 : v;
    }
    std::uint32_t vuImm15() const { return (raw >> 6) & 0x7FFF; }  // VCALLMS (em palavras de 64 bits)
    bool vuInterlock() const { return (raw & 1) != 0; }            // QMFC2.I / CFC2.I ...

    // ---- Códigos de exceção ----------------------------------------------
    std::uint32_t syscallCode() const { return (raw >> 6) & 0xFFFFF; }
    std::uint32_t trapCode() const { return (raw >> 6) & 0x3FF; }

    // ---- Controle de fluxo -----------------------------------------------
    bool isBranch() const { return has(Flag::Branch); }
    bool isJump() const { return has(Flag::Jump); }
    bool isJumpRegister() const { return has(Flag::JumpReg); }
    bool isLikely() const { return has(Flag::Likely); }
    bool isLink() const { return has(Flag::Link); }
    bool hasDelaySlot() const { return has(Flag::DelaySlot); }

    // Alvo de desvios relativos (B*, BC*).
    std::uint32_t branchTarget() const {
        return address + 4 + static_cast<std::uint32_t>(simm16() * 4);
    }
    // Alvo de J/JAL: 4 bits altos do PC do delay slot + índice << 2.
    std::uint32_t jumpTarget() const {
        return ((address + 4) & 0xF0000000u) | (target26() << 2);
    }
    // Alvo estático (J/JAL/B*); 0 para instruções sem alvo conhecido.
    std::uint32_t staticTarget() const {
        if (isJump()) return jumpTarget();
        if (isBranch()) return branchTarget();
        return 0;
    }
    // Desvio que sempre é tomado: BEQ r,r / BGEZ zero / BGEZAL zero / BLEZ zero.
    bool isUnconditionalBranch() const;

    // Retorno de função típico: "jr ra".
    bool isReturn() const { return op == Op::JR && rs() == 31; }
};

}  // namespace anyps2::r5900
