#pragma once

// Reprodução de GS dumps do PCSX2 (formato novo, 0xFFFFFFFF, PCSX2 2.x) no GS em
// software do AnyPS2. O dump traz o estado do GS (registradores do ambiente, VRAM
// de 4 MB), os registradores privilegiados e a sequência de pacotes GIF de um
// quadro, com os VSyncs; a reprodução restaura o estado, entrega os pacotes ao
// Gif (sem Runtime) e devolve as imagens, para comparar com o hardware/PCSX2.
//
// Limitações conhecidas:
//  * o buffer da CLUT não está no estado do PCSX2: ele começa vazio e só é
//    preenchido pelo primeiro TEX0/TEX2 com CLD que carrega do quadro;
//  * a transferência HOST->LOCAL em andamento (m_tr.x/y e TRXDIR) e os vértices já
//    na fila de primitivas não são restaurados; se o primeiro pacote começar no
//    meio de um GIFtag ou de uma imagem, o Gif o trata como um GIFtag novo;
//  * cada caminho do GIF começa ocioso (o estado dos 4 caminhos é ignorado).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/gs/gs.h"

namespace anyps2::gsplay {

// Dump já lido: os bytes do arquivo e onde cada parte começa (validado).
struct Dump {
    std::vector<std::uint8_t> bytes;
    std::uint32_t stateVersion = 0;  // state_version do cabeçalho (9)
    std::uint32_t crc = 0;
    std::string serial;
    std::size_t stateOffset = 0, stateSize = 0;
    std::size_t privOffset = 0;      // 8192 bytes de registradores privilegiados
    std::size_t packetsOffset = 0;   // pacotes até o fim do arquivo
};

// Tamanho do trecho do estado antes da VRAM (PCSX2 GSState::Freeze, versão 9).
constexpr std::size_t kStateVramOffset = 380;

// Interpreta os bytes de um dump. Lança anyps2::Error se o formato não é o esperado.
Dump parseDump(std::vector<std::uint8_t> bytes);
Dump loadDump(const std::string& path);

struct Stats {
    std::uint64_t packets = 0;     // pacotes do dump (qualquer tipo)
    std::uint64_t transfers = 0;   // pacotes Transfer entregues ao Gif
    std::uint64_t vsyncs = 0;
    std::uint64_t readFifos = 0;   // ReadFIFO2 descartados
    std::uint64_t registers = 0;   // pacotes Registers (privilegiados novos)
    std::uint64_t draws = 0;       // desenhos feitos pelo GS
    std::uint64_t errors = 0;      // transferências que o GS recusou
    std::vector<std::string> messages;  // as primeiras mensagens de erro
};

class Player {
public:
    // Cria o GS, restaura o estado do dump (ambiente, VRAM e privilegiados).
    // O dump tem de viver mais que o Player.
    explicit Player(const Dump& dump);

    // Chamado a cada VSync, depois dos desenhos até ali (n é 1 para o primeiro).
    // Devolve false para parar a reprodução.
    using VsyncFn = std::function<bool(std::uint64_t n, rt::gs::Gs& gs)>;
    // Reproduz os pacotes do dump. Lança anyps2::Error se o dump estiver truncado.
    Stats run(const VsyncFn& onVsync = nullptr);

    rt::gs::Gs& gs() { return *gs_; }

private:
    void restoreState();

    const Dump& dump_;
    std::unique_ptr<rt::gs::Gs> gs_;
    std::unique_ptr<rt::Gif> gif_;
};

// Imagem de um buffer qualquer da VRAM (FBP em páginas de 8 KB, FBW em unidades
// de 64 pixels). PSMCT32/24/16/16S em cores; Z e índices em tons de cinza.
rt::gs::Frame readBuffer(rt::gs::Gs& gs, std::uint32_t fbp, std::uint32_t fbw, std::uint32_t psm,
                         std::uint32_t width, std::uint32_t height);

// Interpretador da linha de comando (main só chama isto).
int runCli(int argc, char** argv);

}  // namespace anyps2::gsplay
