#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "anyps2/r5900/instruction.h"

namespace anyps2::r5900 {

struct DisasmText {
    std::string mnemonic;
    std::string operands;

    // Junta mnemônico e operandos (por padrão com TAB, como o objdump).
    std::string str(char separator = '\t') const {
        return operands.empty() ? mnemonic : mnemonic + separator + operands;
    }
};

struct DisasmOptions {
    // Se definido, é chamado para alvos de desvio/salto; o texto retornado
    // (por exemplo "main+0x10") é anexado como " <texto>", como no objdump.
    std::function<std::string(std::uint32_t)> symbolize;
};

// Desmonta uma instrução na sintaxe do GNU objdump (-m mips:5900
// -M no-aliases). Instruções inválidas viram ".word 0x...".
DisasmText disassemble(const Instruction& insn, const DisasmOptions& options = {});

// Atalho: decodifica e desmonta, retornando "mnemônico\toperandos".
std::string disassembleWord(std::uint32_t raw, std::uint32_t address = 0);

// Sufixo de máscara de destino do VU (".xyzw" sem o ponto).
std::string vuDestString(std::uint32_t dest);

}  // namespace anyps2::r5900
