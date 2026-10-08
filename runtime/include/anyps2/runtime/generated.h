#pragma once

// Cabeçalho incluído por todo arquivo gerado pelo recompilador.

#include <cstdint>

#include "anyps2/runtime/context.h"
#include "anyps2/runtime/ops.h"

// O código gerado não inclui runtime.h: só chama estas três funções. Assim
// mudar o Runtime não força recompilar o código de um jogo inteiro.
namespace anyps2::rt::gen {

// Despacho dinâmico: chama a função que contém c->pc (Runtime::call).
void rtCall(Context* c);
// Syscall (v1 = número).
void rtSyscall(Context* c, std::uint32_t pc);
// Orçamento de instruções esgotado (relógio, interrupções, troca de thread).
void rtSafepoint(Context* c, std::uint32_t pc);

// A função foi chamada com c->pc apontando para um endereço que não é
// início nem ponto de entrada conhecido dela.
[[noreturn]] void badEntry(Context* c, std::uint32_t functionStart);

}  // namespace anyps2::rt::gen
