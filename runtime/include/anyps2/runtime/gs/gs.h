#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "anyps2/runtime/gs/gs_access.h"
#include "anyps2/runtime/gs/gs_worker.h"
#include "anyps2/runtime/gs/vram.h"

namespace anyps2::rt {
class Runtime;
}

namespace anyps2::rt::gs {

class GsTrace;  // gs_trace.h: diagnóstico por ANYPS2_GS_PROBE/ANYPS2_GS_DRAWLOG

// Registradores gerais do GS (endereços usados no modo A+D e em REGLIST).
enum Reg : std::uint8_t {
    PRIM = 0x00, RGBAQ = 0x01, ST = 0x02, UV = 0x03, XYZF2 = 0x04, XYZ2 = 0x05, TEX0_1 = 0x06,
    TEX0_2 = 0x07, CLAMP_1 = 0x08, CLAMP_2 = 0x09, FOG = 0x0A, XYZF3 = 0x0C, XYZ3 = 0x0D,
    TEX1_1 = 0x14, TEX1_2 = 0x15, TEX2_1 = 0x16, TEX2_2 = 0x17, XYOFFSET_1 = 0x18, XYOFFSET_2 = 0x19,
    PRMODECONT = 0x1A, PRMODE = 0x1B, TEXCLUT = 0x1C, SCANMSK = 0x22, MIPTBP1_1 = 0x34,
    MIPTBP1_2 = 0x35, MIPTBP2_1 = 0x36, MIPTBP2_2 = 0x37, TEXA = 0x3B, FOGCOL = 0x3D,
    TEXFLUSH = 0x3F, SCISSOR_1 = 0x40, SCISSOR_2 = 0x41, ALPHA_1 = 0x42, ALPHA_2 = 0x43,
    DIMX = 0x44, DTHE = 0x45, COLCLAMP = 0x46, TEST_1 = 0x47, TEST_2 = 0x48, PABE = 0x49,
    FBA_1 = 0x4A, FBA_2 = 0x4B, FRAME_1 = 0x4C, FRAME_2 = 0x4D, ZBUF_1 = 0x4E, ZBUF_2 = 0x4F,
    BITBLTBUF = 0x50, TRXPOS = 0x51, TRXREG = 0x52, TRXDIR = 0x53, HWREG = 0x54, SIGNAL = 0x60,
    FINISH = 0x61, LABEL = 0x62,
};

// Nome do registrador geral, ou "" se o endereço não existe.
const char* regName(std::uint8_t reg);

struct Vertex {
    std::int32_t x = 0, y = 0;  // 12.4, já sem o XYOFFSET
    std::uint32_t z = 0;
    std::uint8_t r = 0, g = 0, b = 0, a = 0;
    float s = 0, t = 0, q = 1;
    std::uint32_t u = 0, v = 0;  // 10.4 (UV)
    std::uint8_t fog = 0;
};

// Imagem RGBA8 (R no byte menos significativo) produzida pela saída de vídeo.
struct Frame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint32_t> pixels;
};

// Buffer de CLUT interno (1 KB). Cada carga de CLUT produz uma versão nova, que
// depois de pronta não muda mais: os desenhos guardam a versão que valia quando
// foram enfileirados, e as faixas não leem estado global.
struct ClutBuffer {
    std::array<std::uint16_t, 512> v{};
    // Falso enquanto uma barreira ainda vai preencher `v` (carga que não pôde ser
    // feita no produtor); as faixas a marcam pronta, e só então o produtor copia.
    std::atomic<bool> ready{true};
};

// Contadores de serialização das faixas (diagnóstico do ANYPS2_PROFILE): por que o
// EE espera o GS e por que os desenhos não se espalham pelas faixas. Inteiros
// simples, incrementados só pelo produtor (a thread do EE); não mudam o
// comportamento.
struct BandStats {
    // Desenhos que não entram nas faixas, mas rodam sozinhos (operação global).
    enum SelfReason : unsigned {
        SelfWrap,     // endereço que dá a volta (FBW = 0, largura > FBW·64, passa do fim da VRAM)
        SelfTexture,  // textura caindo no próprio FRAME/ZBUF
        SelfFrameZ,   // FRAME × ZBUF (ou leitura de Z sobre o FRAME)
        SelfCount
    };
    // Barreiras postas antes de um desenho por conflito com acessos pendentes.
    enum ConflictReason : unsigned {
        ConflictFull,     // fila de acessos pendentes cheia
        ConflictWrite,    // escrita × pendente
        ConflictTexture,  // leitura × pendente (textura, ou Z lido)
        ConflictCount
    };
    // Operações globais (Gs::submit).
    enum SubmitKind : unsigned {
        SubmitLocal,      // LOCAL→LOCAL
        SubmitClut,       // carga de CLUT
        SubmitReset,      // reset do GS
        SubmitDraw,       // desenho sozinho
        SubmitCount
    };
    // Esperas do EE pelas faixas (waitIdle).
    enum WaitReason : unsigned {
        WaitDisplay,   // saída de vídeo (VBlank/display)
        WaitVram,      // vram() lida de fora
        WaitDownload,  // LOCAL→HOST
        WaitSignal,    // SIGNAL
        WaitFinish,    // FINISH no relógio real
        WaitHost,      // HOST→LOCAL: destino com acesso não concluído, ou ordem com um desenho que o lê
        WaitOther,
        WaitCount
    };
    // Os pares (FBP do desenho, TBP0 da textura) mais frequentes: tabela pequena
    // de tamanho fixo; chaves novas com a tabela cheia só incrementam `lost`.
    struct Pair {
        std::uint32_t fbp = 0;  // página do FRAME (FBP)
        std::uint32_t tbp = 0;  // TBP0 (blocos de 64 palavras)
        std::uint64_t n = 0;
    };
    struct PairTable {
        static constexpr std::size_t kMax = 48;
        std::array<Pair, kMax> items{};
        std::size_t used = 0;
        std::uint64_t lost = 0;
        void add(std::uint32_t fbp, std::uint32_t tbp) {
            for (std::size_t i = 0; i < used; ++i) {
                if (items[i].fbp == fbp && items[i].tbp == tbp) {
                    ++items[i].n;
                    return;
                }
            }
            if (used < kMax) {
                items[used++] = Pair{fbp, tbp, 1};
            } else {
                ++lost;
            }
        }
        std::uint64_t count(std::uint32_t fbp, std::uint32_t tbp) const {
            for (std::size_t i = 0; i < used; ++i) {
                if (items[i].fbp == fbp && items[i].tbp == tbp) return items[i].n;
            }
            return 0;
        }
    };

    std::uint64_t bandDraws = 0;     // desenhos enfileirados nas faixas
    std::uint64_t lanesTouched = 0;  // soma de faixas tocadas por esses desenhos
    std::uint64_t self[SelfCount] = {};
    std::uint64_t conflict[ConflictCount] = {};
    std::uint64_t submit[SubmitCount] = {};
    std::uint64_t clutDirect = 0;   // cargas de CLUT feitas no produtor, sem barreira
    std::uint64_t hostDirect = 0;   // transferências HOST→LOCAL escritas direto pelo produtor, sem esperar
    std::uint64_t hostBarrier = 0;  // ...que esperaram as faixas no início (destino com acesso não concluído)
    std::uint64_t hostOrder = 0;    // esperas no meio: palavra nova depois de um desenho que lê/escreve o destino aberto
    std::uint64_t waits[WaitCount] = {};
    std::int64_t waitNs[WaitCount] = {};  // só medido com ANYPS2_PROFILE
    PairTable selfPairs;                  // desenhos sozinhos por textura no próprio buffer
    PairTable conflictPairs;              // barreiras por leitura de textura
};

// Graphics Synthesizer em software: estado completo dos registradores,
// memória local, transferências e um rasterizador de referência que segue as
// regras do hardware (cobertura top-left, Z de 32 bits inteiro, blending
// (A−B)·C/128+D, texturas com CLUT/TEXA etc.).
//
// O desenho roda em faixas, cada uma numa thread (GsWorker, gs_bands.h): o EE só
// faz a parte que tem de acontecer no ponto do GIF (validação, tempo, contagem de
// pixels) e enfileira o resto na ordem do programa. Quem lê a memória local pela
// API pública espera as faixas ficarem ociosas antes. ANYPS2_GS_THREAD=0 volta ao
// caminho síncrono; ANYPS2_GS_THREADS=N escolhe o número de faixas.
class Gs {
public:
    // Limite de faixas (ANYPS2_GS_THREADS é limitado a este valor).
    static constexpr unsigned kMaxLanes = 16;

    explicit Gs(Runtime* rt);
    ~Gs();
    Gs(const Gs&) = delete;
    Gs& operator=(const Gs&) = delete;

    // Espera as faixas terminarem tudo o que já foi enfileirado.
    void waitIdle(BandStats::WaitReason why = BandStats::WaitOther);
    bool threaded() const { return worker_->threaded(); }
    unsigned lanes() const { return worker_->lanes(); }

    // ---- Registradores privilegiados (0x1200_0000) --------------------------
    std::uint64_t readPrivileged(std::uint32_t addr, std::uint32_t pc);
    void writePrivileged(std::uint32_t addr, std::uint64_t value, std::uint32_t pc);
    std::uint64_t csr();
    std::uint64_t imr() const { return imr_; }
    void setImr(std::uint64_t v);
    void setCrt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field);

    // ---- Registradores gerais (via GIF) ----------------------------------------
    void writeRegister(std::uint8_t reg, std::uint64_t value, std::uint32_t pc);
    // Dados de transferência HOST→LOCAL (HWREG ou modo IMAGE do GIF): 64 bits.
    void writeTransferData(std::uint64_t data, std::uint32_t pc);
    // Transferência LOCAL→HOST (TRXDIR=1): o retângulo de origem, empacotado
    // como numa HOST→LOCAL do mesmo PSM e completado até quadword, sai pelo
    // VIF1 (DMA do canal 1 com DIR=0 ou leitura do VIF1_FIFO) com BUSDIR=1.
    // Copia `qwords` quadwords para dst (zeros depois do fim dos dados).
    void readDownload(std::uint8_t* dst, std::size_t qwords);
    std::size_t downloadRemaining() const { return (download_.size() - downloadPos_) / 16; }
    bool busDirToHost() const { return busdir_ != 0; }
    std::uint64_t reg(std::uint8_t r) const { return regs_[r]; }

    // ---- Eventos de vídeo ----------------------------------------------------
    void vblankStart();
    void vblankEnd();
    // Linha HSYNC (o HSINT é calculado sob demanda a partir do relógio).
    void setHsyncSource(std::uint64_t (*now)(void*), void* ctx, std::uint64_t cyclesPerLine);
    // Relógio real: o GS em software gasta tempo de verdade desenhando, e
    // FINISH separados no hardware podem vencer juntos; com isto eles são
    // entregues um por vez (o próximo depois que o programa limpar o bit).
    void setRealTimeClock(bool realTime) { realTime_ = realTime; }
    // Eventos de desenho atrasados (FINISH): o GS processa os dados na hora,
    // mas o evento só aparece depois do tempo estimado de trabalho do GS
    // (transferência pelo GIF + preenchimento de pixels). Sem relógio
    // (testes do GS isolado) o evento é imediato.
    std::uint64_t nextEventTime() const;  // ciclo do EE, ~0 se nenhum
    void processEvents(std::uint64_t now);

    // Compõe a imagem exibida (circuitos de leitura 1 e 2, PMODE, BGCOLOR).
    Frame display() const;
    bool displayEnabled() const;

    // Memória local: espera o worker (quem lê ou escreve aqui vê os pixels já
    // desenhados pelo GIF até este ponto).
    Vram& vram() {
        waitIdle(BandStats::WaitVram);
        return vram_;
    }
    const Vram& vram() const {
        syncConst(BandStats::WaitVram);
        return vram_;
    }
    std::uint64_t drawCount() const { return drawCount_; }
    // Rastreador de desenhos (gs_trace.h). Criado sob demanda; sem as variáveis de
    // ambiente ele fica inativo e o desenho não gasta nada com ele.
    GsTrace& trace();
    // Origem dos dados que o GIF entrega agora (só para o diagnóstico): 1 a 3 =
    // PATH1 (XGKICK do VU1), PATH2 (DIRECT do VIF1), PATH3 (DMA); 0 = escrita
    // direta no Gs, sem passar pelo GIF. noteXgkick conta os XGKICK do VU1.
    void setSource(unsigned path) { source_ = path; }
    void noteXgkick() { ++xgkicks_; }
    unsigned source() const { return source_; }
    std::uint64_t xgkicks() const { return xgkicks_; }
    // Pixels que os desenhos escreveriam pela conta analítica (produtor) e os que
    // o rasterizador de fato escreveu depois do SCISSOR e do SCANMSK (worker).
    // Têm de ser iguais: é o que o teste gs_coverage confere.
    std::uint64_t pixelsCovered() const { return coveredTotal_; }
    std::uint64_t pixelsShaded() const;
    // Contadores de serialização das faixas (só o produtor os altera; não espera).
    const BandStats& bandStats() const { return bandStats_; }

private:
    friend class GsTrace;

    struct Context {
        std::uint64_t tex0 = 0, tex1 = 0, clamp = 0, xyoffset = 0, miptbp1 = 0, miptbp2 = 0;
        std::uint64_t scissor = 0, alpha = 0, test = 0, fba = 0, frame = 0, zbuf = 0;
    };

    // Estado de uma transferência HOST→LOCAL.
    struct Transfer {
        bool active = false;
        std::uint32_t dbp = 0, dbw = 0, psm = 0;
        std::uint32_t x0 = 0, y0 = 0, w = 0, h = 0;
        std::uint32_t x = 0, y = 0;
        std::uint64_t bits = 0;  // dados de 24 bits/4 bits pendentes
        unsigned nbits = 0;
    };

    // Transferência LOCAL→LOCAL já validada no EE; o worker só copia.
    struct LocalCopy {
        std::uint32_t sbp, sbw, spsm, dbp, dbw, dpsm, sx, sy, dx, dy, w, h;
    };

    void raiseEvent(unsigned bit);
    void addWork(std::uint64_t cycles);
    void vertexKick(bool draw, std::uint32_t pc);
    void draw(std::uint32_t pc);
    void writeTex0(unsigned ctx, std::uint64_t value, std::uint32_t pc);
    void loadClut(std::uint64_t tex0, std::uint32_t pc);
    void startTransfer(std::uint32_t pc);
    void localToLocal(std::uint32_t pc);
    void localToHost(std::uint32_t pc);
    [[noreturn]] void unsupported(const std::string& what, std::uint32_t pc) const;
    void warnOnce(const std::string& what);

    // Operações enfileiradas (executadas pelas faixas, na ordem do GIF).
    // syncConst: a espera de leituras feitas por métodos const. Não muda o estado
    // observável; o único efeito é enfileirar o lote pendente e drená-lo.
    void syncConst(BandStats::WaitReason why = BandStats::WaitOther) const {
        const_cast<Gs*>(this)->waitIdle(why);
    }
    // Operação global: barreira em todas as faixas (ver GsWorker::barrier).
    void submit(GsWorker::Op op, BandStats::SubmitKind kind);
    // Acessos diretos do produtor à VRAM (CLUT): vê se o que as faixas ainda podem
    // estar fazendo não toca as páginas. outstanding_ guarda os acessos de tudo o que
    // foi enfileirado desde que as faixas foram vistas ociosas (diferente de pending_,
    // que uma barreira esvazia, mas não termina na hora).
    bool trackOutstanding();
    // Registra um acesso que a próxima barreira enfileirada conclui (chamar com o
    // lote HOST→LOCAL já descarregado e depois da barreira de conflito).
    void noteOutstanding(VramAccess a);
    // Há acesso não concluído que colide com `a` (ou a lista perdeu o rastro).
    bool outstandingConflicts(const VramAccess& a);
    // Fim da transferência HOST→LOCAL aberta (o produtor já escreveu tudo na VRAM).
    void closeHostTransfer();
    VramAccess rectAccess(bool write, std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x0,
                          std::uint32_t y0, std::uint32_t w, std::uint32_t h) const;
    VramAccess clutSource(std::uint64_t tex0, std::uint64_t texclut) const;
    void hostWord(Transfer& x, std::uint64_t data);
    // Uma operação enfileirada nas faixas toca o destino da transferência HOST→LOCAL
    // aberta: a próxima palavra só é escrita depois de as faixas terminarem (hostDrain_).
    void noteHostHazard(const VramAccess* acc, unsigned n);
    void copyLocal(const LocalCopy& c);
    void loadClutCells(ClutBuffer& dst, std::uint64_t tex0, std::uint64_t texclut) const;

    // Rasterização (gs_draw.cpp)
    struct DrawEnv;
    void setupEnv(DrawEnv& e, std::uint32_t pc);
    void rasterize(const DrawEnv& e, const Vertex& v0, const Vertex& v1, const Vertex& v2);
    void drawPoint(const DrawEnv& e, const Vertex& v);
    void drawLine(const DrawEnv& e, const Vertex& a, const Vertex& b);
    void drawTriangle(const DrawEnv& e, const Vertex& a, const Vertex& b, const Vertex& c);
    void drawSprite(const DrawEnv& e, const Vertex& a, const Vertex& b);
    struct Fragment;
    void shadePixel(const DrawEnv& e, int x, int y, Fragment& f);
    std::uint32_t sampleTexture(const DrawEnv& e, float u, float v, float lod) const;
    std::uint32_t fetchTexel(const DrawEnv& e, unsigned level, int u, int v) const;

    // Faixas do GS. Tudo que está abaixo de "faixas" é delas: o EE só mexe nesses
    // campos depois de waitIdle() ou dentro de uma operação global (barreira).
    std::unique_ptr<GsWorker> worker_;
    // Acessos à VRAM dos desenhos enfileirados desde a última barreira (produtor).
    PendingAccess pending_;
    PendingAccess outstanding_{256};  // produtor; ver trackOutstanding()
    std::uint64_t outstandingLostNeed_ = 0;  // lista cheia: sem rastro até a barreira desse número
    // Transferência HOST→LOCAL aberta (faltam palavras): o destino conta como escrita
    // em andamento enquanto o produtor ainda pode enviar dados.
    bool hostOpenValid_ = false;
    bool hostDrain_ = false;  // há operação enfileirada que toca o destino aberto: esperar antes da próxima palavra
    VramAccess hostOpen_;
    std::uint64_t hostWordsLeft_ = 0;

    Runtime* rt_;
    Vram vram_;  // worker
    std::array<std::uint64_t, 256> regs_{};
    Context ctx_[2];
    // CLUT corrente (versão mais recente; produtor).
    std::shared_ptr<ClutBuffer> clut_ = std::make_shared<ClutBuffer>();
    std::uint32_t cbp0_ = 0, cbp1_ = 0;

    // Fila de vértices
    Vertex current_;
    Vertex queue_[3];
    unsigned queued_ = 0;
    unsigned fanFirst_ = 0;

    // Transferência HOST→LOCAL em andamento: o produtor escreve as palavras na VRAM na
    // hora (ver startTransfer), depois de esperar as faixas se o destino estava em uso.
    Transfer xferDirect_;

    // Transferência LOCAL→HOST: dados prontos para o VIF1 e quanto já saiu
    // (produtor: sai de uma leitura da VRAM feita depois de waitIdle()).
    std::vector<std::uint8_t> download_;
    std::size_t downloadPos_ = 0;
    std::uint64_t busdir_ = 0;  // BUSDIR (0x1200_1040): 1 = GS → EE

    // Registradores privilegiados
    std::array<std::uint64_t, 16> priv_{};  // 0x1200_0000 + i*0x10 (PMODE..BGCOLOR)
    std::uint64_t csr_ = 0;
    std::uint64_t imr_ = 0x7F00;
    std::uint64_t siglblid_ = 0;
    std::uint64_t (*hsyncNow_)(void*) = nullptr;
    void* hsyncCtx_ = nullptr;
    std::uint64_t cyclesPerLine_ = 0;
    std::uint64_t hsyncClearedAt_ = 0;

    // Modelo de tempo do GS (em ciclos do EE), tudo no produtor
    std::uint64_t work_ = 0;        // trabalho acumulado desde o último evento
    std::uint64_t pixels_ = 0;      // pixels (conta analítica) desde o último evento
    std::uint64_t busyUntil_ = 0;   // fim do trabalho já agendado
    bool realTime_ = false;
    std::uint64_t workStart_ = 0;   // chegada do primeiro dado do lote atual
    std::vector<std::uint64_t> finishDue_;  // FINISH pendentes (ordem crescente)

    std::uint64_t drawCount_ = 0;
    BandStats bandStats_;  // produtor
    std::uint64_t coveredTotal_ = 0;  // pixels da conta analítica, sem reset
    // Pixels escritos pelo rasterizador, por faixa (cada faixa soma a sua; em
    // linhas separadas da cache para não disputar a mesma linha).
    // (preenchimento explícito: alignas num membro gera o aviso C4324 no MSVC)
    struct LaneCount {
        std::uint64_t shaded = 0;
        std::uint64_t pad[7] = {};
    };
    std::array<LaneCount, kMaxLanes> laneShaded_{};
    std::set<std::string> warned_;

    // Diagnóstico: VBlanks recebidos (vblankStart) e o rastreador, nulo sem as
    // variáveis de ambiente. O produtor é o único que mexe nisto.
    std::uint64_t vblanks_ = 0;
    unsigned source_ = 0;
    std::uint64_t xgkicks_ = 0;
    std::unique_ptr<GsTrace> trace_;
};

}  // namespace anyps2::rt::gs
