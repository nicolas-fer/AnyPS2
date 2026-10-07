#pragma once

#include <cstdint>

#include "anyps2/r5900/instruction.h"

namespace anyps2::r5900 {

// Decodifica uma palavra do EE. Nunca lança exceção: codificações que não
// existem no R5900 retornam Op::Invalid, e cabe a quem consome (gerador de
// código) transformar isso num erro com endereço e palavra.
//
// A decodificação imita o hardware: campos reservados são ignorados para
// escolher a operação, mas Instruction::canonical fica false quando algum
// bit reservado está ligado (útil para detectar dados confundidos com
// código).
Instruction decode(std::uint32_t raw, std::uint32_t address = 0);

}  // namespace anyps2::r5900
