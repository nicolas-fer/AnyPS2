#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace anyps2::rt {

// Botões do controle do PS2 nos bits de padButtonStatus.btns (libpad). Aqui
// 1 = pressionado; o padman entrega ao jogo invertido (ativo em 0).
namespace padbtn {
inline constexpr std::uint16_t SELECT = 0x0001, L3 = 0x0002, R3 = 0x0004, START = 0x0008, UP = 0x0010,
                               RIGHT = 0x0020, DOWN = 0x0040, LEFT = 0x0080, L2 = 0x0100, R2 = 0x0200,
                               L1 = 0x0400, R1 = 0x0800, TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000,
                               SQUARE = 0x8000;
}

struct PadInput {
    std::uint16_t buttons = 0;                       // padbtn::*
    std::uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;  // analógicos (0x80 = centro)
    bool connected = true;
};

// Estado dos dois controles. A janela SDL (teclado e controles) escreve de
// outra thread; o padman lê a cada VBlank. Com ANYPS2_PAD_SCRIPT, um roteiro
// determinístico substitui a entrada do host (testes):
//   # vblank porta botões [lx ly rx ry]
//   60  0 START
//   90  0 CROSS+RIGHT 128 128 255 128
//   120 0 -
// Cada linha vale a partir daquele VBlank. "unplug" desconecta a porta.
class Input {
public:
    void setHost(unsigned port, const PadInput& state);
    PadInput sample(unsigned port, std::uint64_t vblank);
    // Lê o roteiro; erro claro (exceção) se a sintaxe estiver errada.
    void loadScript(const std::string& path);
    bool scripted() const { return scripted_; }

private:
    struct Step {
        std::uint64_t vblank;
        unsigned port;
        PadInput state;
    };
    std::mutex mutex_;
    PadInput host_[2];
    std::vector<Step> script_;
    bool scripted_ = false;
};

// Relatório de 18 bytes de um DualShock 2 (como o controle o envia e o
// libpad entrega depois de {0, modo}): 2 bytes de botões ativos em 0, os
// analógicos rx ry lx ly e 12 pressões (RIGHT LEFT UP DOWN TRIANGLE CIRCLE
// CROSS SQUARE L1 R1 L2 R2; sem sensor de pressão no host, pressionado = 255).
void ds2Report(const PadInput& in, std::uint8_t out[18]);

// "CROSS+START" → bits; "-" → 0. Nomes: SELECT L3 R3 START UP RIGHT DOWN
// LEFT L2 R2 L1 R1 TRIANGLE CIRCLE CROSS SQUARE.
std::uint16_t parsePadButtons(const std::string& text);

// Nomes dos botões do DS2 no arquivo de configuração (minúsculas), na ordem
// dos bits de padbtn: o índice i é o bit 1 << i. Analógicos: lx ly rx ry.
inline constexpr const char* kPadButtonNames[16] = {"select", "l3",       "r3",       "start",
                                                    "up",     "right",    "down",     "left",
                                                    "l2",     "r2",       "l1",       "r1",
                                                    "triangle", "circle", "cross",    "square"};
inline constexpr const char* kPadAnalogNames[4] = {"lx", "ly", "rx", "ry"};
// Índice do botão em kPadButtonNames, ou -1 se o nome não existir.
int padButtonIndex(const std::string& name);

// Eixo ligado a um botão digital: "leftx" (aciona nos dois sentidos), "+leftx"
// (só para cima/direita) ou "-leftx" (só para baixo/esquerda). Devolve o nome do
// eixo e o sentido em dir (0 = os dois, +1, -1).
std::string padAxisName(const std::string& bound, int& dir);
// Digital acionado pelo valor do eixo (-32768..32767) no sentido dir: |v| acima
// de um limiar. Gatilhos só dão valores positivos, então "lefttrigger" aciona
// como antes.
bool padAxisPressed(int dir, int value);

// Ligações de teclado e de controle de cada porta (0 = porta 1). Os nomes são
// os do SDL, resolvidos pela janela: tecla ("Z", "Up", "Return"), botão do
// controle ("a", "dpup", "leftshoulder") ou eixo ("leftx", "lefttrigger").
// Vazio = sem ligação. Os padrões reproduzem o mapeamento fixo de antes.
struct PadBindings {
    std::string key[2][16];     // tecla que aciona cada botão (índice = bit)
    std::string button[2][16];  // botão ou eixo do controle que aciona cada botão
    std::string analog[2][4];   // eixo do controle para lx, ly, rx, ry
    static PadBindings defaults();
};

}  // namespace anyps2::rt
