#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace anyps2::rt {

class Iop;

// Pilha de controles do SDK 3.0 em HLE: dbcman (gerenciador DBC) +
// sio2d (SIO2) + ds2u_d (driver do DualShock 2), usada pela libdbc/libpad2
// do EE no lugar do padman. Protocolo levantado por engenharia reversa do
// lado do EE (sem código da Sony):
//   - RPC 0x80001300, funções 0x800013xx, um buffer de 0x90 bytes de ida e
//     volta: versão (0x80001363), área de trabalho no EE (0x80001304),
//     criar/apagar socket (0x80001301/02), iniciar socket (0x80001303),
//     consulta ao dispositivo (0x8000131A);
//   - RPC 0x8000131E/1F: comandos assíncronos ao dispositivo (vibração);
//   - área de trabalho: uma palavra de estado por socket (1 = dispositivo
//     conectado);
//   - cada socket tem dois quadros de 128 bytes no EE (buffer duplo) que o
//     IOP reescreve a cada leitura do controle: {estado, -, tamanho dos dados,
//     tamanho do perfil, status, ..., dados a partir de 28, perfil logo
//     depois, contador em 124}.
// O dispositivo emulado é um DualShock 2 (16 botões digitais + 16 entradas
// analógicas: 4 eixos e 12 pressões), com os dados no formato do relatório
// do DS2.
class DbcMan {
public:
    explicit DbcMan(Iop& iop);
    void load();  // registra os servidores RPC
    void reset();
    void vblank(std::uint32_t pc);

private:
    struct Socket {
        bool used = false, started = false;
        unsigned port = 0;
        std::array<std::uint32_t, 2> frames{};  // endereços dos quadros no EE
        std::uint32_t counter = 0;
        unsigned next = 0;
    };
    std::vector<std::uint8_t> main(std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc);
    std::vector<std::uint8_t> command(std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc);
    void writeStates(std::uint32_t pc);
    void writeFrame(Socket& s, std::uint32_t pc);
    bool connected(unsigned port);

    Iop& iop_;
    std::array<Socket, 16> sockets_{};
    std::uint32_t workArea_ = 0;
    std::uint64_t vblanks_ = 0;
};

}  // namespace anyps2::rt
