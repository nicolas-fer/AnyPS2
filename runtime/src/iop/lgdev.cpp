// Driver do volante Logitech (LgDev_tb_rb_Driver, lgdev.irx) em HLE, sem
// volante conectado. RPC 0x046D046D (o vendor ID da Logitech duas vezes).
// Protocolo levantado do lado do EE (Gran Turismo 4):
//   fn 12 (576 bytes de ida e volta): versão do módulo em [4] — o jogo exige
//   0x010B2400 (1.11) e trava de propósito se não bater.
//   fn 1 (enumeração): [4] = índice do dispositivo; [0] = código (negativo =
//   não há dispositivo nesse índice, o jogo para de enumerar; senão 24 bytes de
//   descrição em [112]). Os erros que o cliente conhece são 0x80000004
//   (argumento inválido) e 0x80000008 (ocupado); sem volante respondemos
//   0x80000001. As demais funções recebem o handle de um dispositivo aberto,
//   então sem volante não deveriam ser chamadas.

#include <string>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/iop/iop.h"

namespace anyps2::rt {

using namespace iopio;

void registerLgDev(Iop& iop) {
    iop.registerServer(0x046D046Du, "lgdev",
                       [](std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc)
                           -> std::optional<std::vector<std::uint8_t>> {
                           std::vector<std::uint8_t> out = in;
                           out.resize(576, 0);
                           switch (fn) {
                               case 1:  // enumeração: nenhum volante conectado
                                   wr32(out, 0, 0x80000001u);
                                   return out;
                               case 12:  // versão
                                   wr32(out, 4, 0x010B2400u);
                                   return out;
                               default:
                                   throw Unimplemented("lgdev (volante Logitech): função " + std::to_string(fn) +
                                                           " ainda não implementada no HLE",
                                                       pc);
                           }
                       });
}

}  // namespace anyps2::rt
