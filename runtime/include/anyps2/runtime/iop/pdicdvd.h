#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace anyps2::rt {

class Iop;

// Driver de disco da Polyphony Digital (PDI_CDVD_Manager, pdicdvd.irx) em
// HLE. Protocolo levantado do lado do EE (Gran Turismo 4) — sem código da
// Polyphony nem da Sony:
//   - RPC "PCDV" (0x50434456), pedido de 64 bytes:
//       fn 1: pronto (sem dados);
//       fn 2: {setor do PVD, hash do setor} — o jogo lê o ISO 9660 sozinho;
//       fn 3: leitura {setor, tamanho em bytes, destino no EE, modo} → [0]=0;
//       fn 4: início da camada 1 do DVD → {1, setor};
//   - RPC "Pcdv" (0x50636476): fn 0 registra no IOP um buffer de status de 64
//     bytes no EE, que o driver atualiza sozinho: [0] relógio em segundos.
// As leituras terminam na hora (a resposta assíncrona chega no próximo
// atendimento do SIF, e o callback do EE sinaliza quem espera).
class PdiCdvd {
public:
    explicit PdiCdvd(Iop& iop);
    void load();
    void reset();
    void vblank(std::uint32_t pc);

private:
    std::vector<std::uint8_t> pcdv(std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc);
    std::vector<std::uint8_t> status(std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc);
    void writeStatus(std::uint32_t pc);

    Iop& iop_;
    std::uint32_t statusAddr_ = 0;
    std::uint64_t vblanks_ = 0;
};

}  // namespace anyps2::rt
