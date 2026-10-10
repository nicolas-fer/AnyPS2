#pragma once

// Diagnóstico do GS: mostra QUAIS desenhos escreveram nos pixels escolhidos e
// com que estado. Duas saídas, ligadas por variável de ambiente ou por código:
//  * sonda (ANYPS2_GS_PROBE): para cada desenho do intervalo de VBlanks cuja
//    caixa (depois do SCISSOR) contém um ponto da sonda, grava o estado do
//    desenho e a cor e o Z de cada ponto antes e depois dele;
//  * log de desenhos (ANYPS2_GS_DRAWLOG): uma linha por desenho do intervalo,
//    sem ler a VRAM.
// Sem as variáveis o Gs não cria o rastreador (trace_ é nulo) e o desenho não
// paga nada além de um teste de ponteiro.

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/gs/gs_coverage.h"

namespace anyps2::rt::gs {

// Configuração do rastreador (as variáveis de ambiente são uma forma de montá-la).
struct GsTraceConfig {
    // Pontos da sonda, em pixels do framebuffer (sem XYOFFSET). No máximo 8.
    std::vector<std::pair<int, int>> points;
    // Se filterFbp, só conta desenhos cujo campo FBP do FRAME vale fbp (o valor
    // do registrador, em páginas de 8 KB). Sem filtro, aceita qualquer FBP.
    bool filterFbp = false;
    std::uint32_t fbp = 0;
    // Intervalos de VBlanks, inclusivos. O VBlank n é o n-ésimo vblankStart do
    // Gs; 0 é o que vem antes do primeiro.
    std::uint64_t probeFrom = 0, probeTo = ~std::uint64_t{0};
    std::string probeOut = "gs_probe.txt";
    std::string drawLog;  // vazio: sem log de desenhos
    std::uint64_t drawFrom = 0, drawTo = ~std::uint64_t{0};
};

// Registradores que definem um desenho, copiados no momento em que ele é feito.
struct DrawState {
    unsigned ctxt = 0;
    bool useprim = false;  // PRMODECONT: o atributo vem do PRIM, não do PRMODE
    std::uint64_t prim = 0, attr = 0;
    std::uint64_t frame = 0, zbuf = 0, test = 0, alpha = 0, scissor = 0;
    std::uint64_t tex0 = 0, tex1 = 0, clamp = 0, texa = 0;
    std::uint64_t fba = 0, pabe = 0, dthe = 0, colclamp = 0, scanmsk = 0;
};

// Coordenadas de textura de um pixel (STQ e UV, como o vértice).
struct TexCoords {
    double s = 0, t = 0, q = 1, u = 0, v = 0;
};

class GsTrace {
public:
    static constexpr std::size_t kMaxPoints = 8;

    // Lê ANYPS2_GS_PROBE, _FROM, _TO, _FBP, _OUT e ANYPS2_GS_DRAWLOG, _FROM, _TO.
    // Devolve nulo se nenhuma das duas saídas está ligada (ou não pôde abrir os arquivos).
    static std::unique_ptr<GsTrace> fromEnv();

    // Troca a configuração e abre (zerando) os arquivos de saída. Sem pontos nem
    // log, o rastreador fica desligado.
    void configure(const GsTraceConfig& cfg);
    bool active() const { return !points_.empty() || log_.is_open(); }

    // Ganchos de Gs::draw. beginDraw devolve true se algum ponto da sonda cai
    // no desenho: então endDraw tem de ser chamado depois de o desenho ser
    // enfileirado. Os dois só leem o estado do GS e a VRAM depois de waitIdle().
    bool beginDraw(Gs& gs, std::uint32_t pc, unsigned type, const DrawWindow& w, const Vertex& v0,
                   const Vertex& v1, const Vertex& v2);
    void endDraw(Gs& gs);

private:
    struct Hit {
        int x = 0, y = 0;
        TexCoords at;
        std::uint32_t colorBefore = 0, colorAfter = 0, zBefore = 0, zAfter = 0;
    };
    struct Pending {
        bool active = false;
        std::uint64_t vblank = 0, draw = 0;
        std::uint32_t pc = 0;
        unsigned type = 0;
        Vertex v[3];
        DrawState state;
        std::vector<Hit> hits;
    };

    static DrawState capture(const Gs& gs);
    void writeLogLine(std::uint64_t vblank, std::uint64_t draw, std::uint32_t pc, unsigned type,
                      const DrawState& s);
    void writeProbe();

    std::vector<std::pair<int, int>> points_;
    std::uint64_t probeFrom_ = 0, probeTo_ = 0;
    bool filterFbp_ = false;
    std::uint32_t fbp_ = 0;
    std::uint64_t drawFrom_ = 0, drawTo_ = 0;
    std::ofstream probe_;
    std::ofstream log_;
    Pending pending_;
};

}  // namespace anyps2::rt::gs
