#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace anyps2::rt {

class Iop;

// padman em HLE (protocolo do libpad: RPC 0x80000100/0x80000101 do
// XPADMAN/padman.irx e 0x8000010F/0x8000011F do PADMAN da ROM).
//
// Como o módulo real, a cada VBlank o "IOP" escreve na área do EE passada em
// padPortOpen a estrutura de estado do controle (buffer duplo, o campo frame
// diz qual é o mais novo): estado, modo atual e os bytes de botões/analógicos/
// pressão. O controle emulado é um DualShock 2: liga em modo digital (0x41),
// padSetMainMode passa a analógico (0x73) e padSetButtonInfo(0xFFF) liga a
// pressão (0x79). Só a porta física (slot 0) existe — sem multitap.
class PadMan {
public:
    explicit PadMan(Iop& iop);
    // Registra os servidores RPC (newProtocol: XPADMAN/padman.irx).
    void load(bool newProtocol);
    void reset();
    void vblank(std::uint32_t pc);

private:
    struct Slot {
        bool open = false;
        std::uint32_t area = 0;  // área do EE (2 estruturas)
        std::uint32_t frame = 0;
        std::uint8_t modeCurId = 0x41;
        bool analog = false, pressure = false, lock = false;
        std::uint8_t reqState = 0;     // PAD_RSTAT_*
        bool pending = false;          // pedido em andamento (completa no próximo VBlank)
        bool pendingAnalog = false, pendingPressure = false;
        std::array<std::uint8_t, 6> actDirect{}, actAlign{};
    };
    std::optional<std::vector<std::uint8_t>> rpc(std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                 std::uint32_t pc);
    void writeState(unsigned port, Slot& s, std::uint32_t pc);

    Iop& iop_;
    bool newProtocol_ = true;
    std::array<Slot, 2> slots_{};
    std::uint32_t statBuf_ = 0, statFrame_ = 0;  // padPortInit (XPADMAN)
    std::uint64_t vblanks_ = 0;
};

}  // namespace anyps2::rt
