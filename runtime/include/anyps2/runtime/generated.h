#pragma once

// Cabeçalho incluído por todo arquivo gerado pelo recompilador.

#include <cstdint>

#include "anyps2/runtime/context.h"
#include "anyps2/runtime/ops.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt::gen {

// A função foi chamada com c->pc apontando para um endereço que não é
// início nem ponto de entrada conhecido dela.
[[noreturn]] void badEntry(Context* c, std::uint32_t functionStart);

}  // namespace anyps2::rt::gen
