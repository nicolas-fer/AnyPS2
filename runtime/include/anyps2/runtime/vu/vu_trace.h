#pragma once

// Rastreador do VU1 para diagnóstico de dados (ANYPS2_VU_TRACE e variáveis
// irmãs; ver vu_trace.cpp). Para cada microprograma do VU1 dentro do intervalo
// de VBlanks, guarda uma cópia da memória de dados e dos registradores na
// entrada; se durante a execução houver um XGKICK num endereço da lista, grava
// no arquivo o estado da entrada, o estado e a memória de dados no XGKICK e o
// pacote GIF que foi enviado, decodificado.
//
// Sem a variável o Vu não tem rastreador (trace_ é nulo) e o microprograma não
// paga nada além de um teste de bool. Com o rastreador ligado, o código
// recompilado e o interpretador geram o mesmo registro: os ganchos ficam em
// Vu::run e Vu::xgkick, não no gerador.

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "anyps2/runtime/context.h"

namespace anyps2::rt {

class Vu;

// Configuração do rastreador (as variáveis de ambiente são uma forma de montá-la).
struct VuTraceConfig {
    std::string out = "vu_trace.txt";
    // Intervalo de VBlanks, inclusivo: o VBlank n é o n-ésimo onVblank do
    // Runtime; 0 é o que vem antes do primeiro.
    std::uint64_t from = 0, to = ~std::uint64_t{0};
    // Endereços (bytes da micro memória) das instruções XGKICK a registrar.
    // Vazio: todos os XGKICK do intervalo.
    std::vector<std::uint32_t> kicks;
    // Máximo de microprogramas registrados (os que tiveram ao menos um XGKICK
    // gravado); depois disso o rastreador deixa de copiar memória.
    unsigned maxPrograms = 4;
};

class VuTrace {
public:
    static constexpr unsigned kDefaultMaxPrograms = 4;

    // Lê ANYPS2_VU_TRACE (arquivo), _FROM, _TO, _KICK (lista de endereços,
    // separados por vírgula ou espaço, decimais ou 0x...) e _MAX. Devolve nulo
    // sem ANYPS2_VU_TRACE ou se o arquivo não pôde ser aberto.
    static std::unique_ptr<VuTrace> fromEnv();

    // Troca a configuração e abre (zerando) o arquivo de saída. Se ele não
    // abrir, o rastreador fica desligado.
    void configure(const VuTraceConfig& cfg);
    bool active() const { return out_.is_open(); }

    // Ganchos de Vu::run (início do microprograma) e Vu::xgkick (antes do
    // envio pelo PATH1). `vblank` é o contador de VBlanks do Runtime.
    void beginRun(Vu& vu, std::uint32_t startPc, std::uint32_t eePc, std::uint64_t vblank);
    void kick(Vu& vu, std::uint32_t addr, std::uint32_t pc);

    std::uint64_t runsSeen() const { return runs_; }
    unsigned runsRecorded() const { return recorded_; }

private:
    struct Snapshot {
        bool active = false;      // há um microprograma em execução dentro do intervalo
        bool recorded = false;    // já gravou algum XGKICK deste microprograma
        std::uint64_t number = 0, vblank = 0, cycle = 0;
        std::uint32_t startPc = 0, eePc = 0;
        Reg128 vf[32];
        std::uint32_t vi[32] = {};
        Reg128 acc;
        std::vector<std::uint8_t> data;
    };

    std::ofstream out_;
    VuTraceConfig cfg_;
    Snapshot run_;
    std::uint64_t runs_ = 0;
    unsigned recorded_ = 0;
};

}  // namespace anyps2::rt
