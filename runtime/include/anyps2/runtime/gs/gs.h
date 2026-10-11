#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "anyps2/runtime/gs/gs_access.h"
#include "anyps2/runtime/gs/gs_texcache.h"
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

// Registradores privilegiados que a saída de vídeo lê (PMODE, SMODE2, DISPFB1/2,
// DISPLAY1/2, BGCOLOR), copiados num ponto do programa (o VBlank). A composição do quadro
// é uma função só deste estado e da VRAM: o quadro pedido não muda se o programa
// reconfigurar o vídeo logo depois.
struct DisplayState {
    std::uint64_t pmode = 0, smode2 = 0;
    std::uint64_t dispfb[2] = {};
    std::uint64_t display[2] = {};
    std::uint64_t bgcolor = 0;
};

// Buffer de CLUT interno (1 KB). Cada carga de CLUT produz uma versão nova, que
// depois de pronta não muda mais: os desenhos guardam a versão que valia quando
// foram enfileirados, e as faixas não leem estado global.
//
// As versões são preguiçosas e encadeadas: a carga só registra a `base` (versão
// anterior) e a sobreposição (as posições do buffer que ela troca). A tabela
// completa `v` é montada por resolve(), uma vez, quando alguém precisa lê-la: base
// resolvida, cópia dela, sobreposição aplicada. Assim uma carga não espera a versão
// anterior ficar pronta (uma carga por barreira não puxa as seguintes para barreira).
struct ClutBuffer {
    struct Cell {
        std::uint16_t pos = 0;  // posição em `v` (0..511)
        std::uint16_t val = 0;
    };
    // Só vale depois de resolve(). Mutável: resolve() é const para quem só lê a versão.
    mutable std::array<std::uint16_t, 512> v{};
    // Falso enquanto uma barreira ainda vai preencher a sobreposição (carga que não
    // pôde ler a VRAM no produtor); as faixas a marcam pronta, e só então ela pode
    // ser resolvida fora da ordem das faixas (pelo produtor).
    mutable std::atomic<bool> ready{true};
    // Versão anterior; solta (reset) quando esta é resolvida, para liberar a cadeia.
    mutable std::shared_ptr<const ClutBuffer> base;
    // Células carregadas por esta versão, na ordem da carga (a última vence).
    std::vector<Cell> overlay;
    // Verdadeiro depois de resolve() (com `v` montado e a base solta).
    mutable std::atomic<bool> done{false};
    // Leitura diferida (carga "coberta", Gs::loadClut): em vez de esperar uma barreira, a
    // versão guarda como ler a VRAM depois. Quem precisar dela (uma faixa em resolve(), ou a
    // tarefa que o produtor põe na fila antes da próxima barreira) executa a leitura uma vez
    // só; `ready` fica falso até lá. Definida antes de a versão ser compartilhada; só vale
    // enquanto nenhuma escrita conflitante tiver concluído (garantia de quem cria a versão).
    std::function<void(ClutBuffer&)> lazyFill;
    mutable std::once_flag lazyOnce;

    ClutBuffer() = default;
    ClutBuffer(const ClutBuffer&) = delete;
    ClutBuffer& operator=(const ClutBuffer&) = delete;
    // Solta a cadeia de versões sem recursão (uma cadeia longa não resolvida
    // estouraria a pilha no destrutor encadeado do shared_ptr).
    ~ClutBuffer();

    void set(std::size_t pos, std::uint16_t val) {
        overlay.push_back(Cell{static_cast<std::uint16_t>(pos), val});
    }
    // Torna a versão diferida: `fill` preenche a sobreposição (set) quando for chamada, uma
    // vez só, por qualquer thread. Antes de compartilhar a versão.
    void makeLazy(std::function<void(ClutBuffer&)> fill) {
        lazyFill = std::move(fill);
        ready.store(false, std::memory_order_relaxed);
    }
    bool lazy() const { return static_cast<bool>(lazyFill); }
    // Executa a leitura diferida se ainda não foi feita (no-op para versões diretas ou de
    // barreira) e marca a versão pronta. Seguro de várias threads; curto e sem espera.
    void fillLazy() const;
    // Monta `v`. Seguro de várias threads; exige que as sobreposições desta versão e
    // das anteriores estejam preenchidas (ordem das barreiras, ou `ready`), exceto as
    // diferidas: resolve() executa a leitura delas (fillLazy), por isso só quem pode lê-las
    // naquele instante (as faixas, ou o produtor com tudo pronto) o chama. Iterativo:
    // a cadeia não resolvida pode ter centenas de versões.
    void resolve() const;
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
        SubmitHost,       // palavras de uma HOST→LOCAL em modo fila (hostQueued)
        SubmitDisplay,    // composição de um quadro de vídeo (requestDisplay)
        SubmitCount
    };
    // Tipo das barreiras, para o tempo de espera das faixas (GsWorker::barrierWaitNs).
    enum BarrierKind : unsigned {
        BkClut,       // carga de CLUT
        BkHost,       // palavras de HOST→LOCAL em fila
        BkDisplay,    // composição de vídeo
        BkConflict,   // barreira posta antes de um desenho por conflito
        BkReset,      // reset do GS
        BkLocal,      // LOCAL→LOCAL
        BkDraw,       // desenho sozinho
        BkOther,
        BkCount
    };
    // Esperas do EE pelas faixas (waitIdle).
    enum WaitReason : unsigned {
        WaitDisplay,   // saída de vídeo (VBlank/display)
        WaitVram,      // vram() lida de fora
        WaitDownload,  // LOCAL→HOST
        WaitSignal,    // SIGNAL
        WaitFinish,    // FINISH no relógio real
        WaitHost,      // HOST→LOCAL direta: ordem com um desenho que lê/escreve o destino aberto (em modo fila não há espera)
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
    std::uint64_t hostBarrier = 0;  // espera no início (destino com acesso não concluído): o modo fila a substituiu, deve ficar em 0
    std::uint64_t hostOrder = 0;    // esperas no meio: palavra nova depois de um desenho que lê/escreve o destino aberto
    // HOST→LOCAL em modo fila: o destino tinha acesso não concluído nas faixas, então as
    // palavras vão para um buffer do produtor e uma barreira ordenada as escreve na VRAM.
    std::uint64_t hostQueued = 0;         // transferências abertas em modo fila
    std::uint64_t hostQueuedSubmits = 0;  // barreiras de escrita enfileiradas por elas (parciais + final)
    std::uint64_t hostQueuedWords = 0;    // palavras de 64 bits levadas por essas barreiras
    // Saída de vídeo assíncrona (Gs::requestDisplay): quadros compostos por tarefa
    // ordenada na fila e vezes que o EE esperou o quadro anterior (limite de um quadro de atraso).
    std::uint64_t displayQueued = 0;
    std::uint64_t displayLagWaits = 0;
    // Cargas de CLUT que viram barreira (diagnóstico do perfil, fatia 1 do plano da
    // dependência de CLUT; só medem, não mudam a ordem). Calculados em Gs::loadClut
    // quando a carga não pode ser direta. "Coberta" = toda escrita não concluída que
    // conflita com a área já está atrás de uma barreira emitida e nada conflitante foi
    // posto desde a última (o que uma leitura adiada poderia ignorar); "recente" = há
    // escrita conflitante desde a última barreira (no pending_), por número de faixas
    // que a escrevem (índice 0: nenhuma conhecida; 4: quatro ou mais). Os demais
    // (host, lost) se sobrepõem a esses; o resto das barreiras é cascata não coberta.
    static constexpr unsigned kLagBuckets = 12;
    std::uint64_t clutBarriers = 0;           // cargas que viraram barreira
    std::uint64_t clutCovered = 0;
    std::uint64_t clutRecent[5] = {};
    std::uint64_t clutRecentTrueBlock = 0;    // das recentes: sobreposição verdadeira em bloco de 256 B
    std::uint64_t clutHost = 0;               // o conflito vem de HOST→LOCAL (fila ou transferência aberta)
    std::uint64_t clutLazy = 0;               // cargas feitas sem barreira, com leitura diferida (ANYPS2_GS_CLUT_DEP)
    std::uint64_t clutLost = 0;               // outstanding_ perdeu rastro (cheio)
    std::uint64_t clutWarAfter = 0;           // a leitura foi seguida, antes da próxima barreira, de escrita que colide com ela
    std::uint64_t clutWarAfterCovered = 0;    // ...só entre as cobertas
    std::uint64_t clutLag[kLagBuckets] = {};  // issued − done na carga: 0, 1, 2–3, 4–7, ...
    // Barreiras por tipo (produtor) e espera das faixas nelas (chegada → liberação,
    // somada nas faixas): por tipo e por faixa. Só medido com o perfil ligado.
    static constexpr unsigned kLaneSlots = 16;
    std::uint64_t barrierKind[BkCount] = {};
    std::int64_t barrierWaitNs[BkCount] = {};
    std::int64_t laneWaitNs[kLaneSlots] = {};
    unsigned lanes = 0;
    std::uint64_t waits[WaitCount] = {};
    std::int64_t waitNs[WaitCount] = {};  // só medido com ANYPS2_PROFILE
    PairTable selfPairs;                  // desenhos sozinhos por textura no próprio buffer
    PairTable conflictPairs;              // barreiras por leitura de textura
    // Pixels por desenho (conta analítica), em potências de 2: o balde i guarda os
    // desenhos com 2^(i-1) <= pixels < 2^i (balde 0: zero pixels; o último junta o resto).
    static constexpr unsigned kPixBuckets = 24;
    std::uint64_t pixHist[kPixBuckets] = {};     // todos os desenhos
    std::uint64_t pixHistTex[kPixBuckets] = {};  // só os texturizados (TME)
};

// Diagnóstico de texturas (ANYPS2_PROFILE): somas das faixas, vistas pelo produtor.
// Não mudam o comportamento nem a imagem.
struct TexReport {
    // Buscas de texel por formato.
    enum Fmt : unsigned { FmtT4, FmtT8, FmtCT16, FmtCT32, FmtOther, FmtCount };
    std::uint64_t fetch[FmtCount] = {};
    std::uint64_t samples = 0;    // chamadas de sampleTexture
    std::uint64_t linear = 0;     // ...com filtro bilinear (em ao menos um nível)
    std::uint64_t trilinear = 0;  // ...que misturaram dois níveis de mipmap
    // Tempo amostrado (1 de cada 64 chamadas), em ns. *Timed = chamadas medidas.
    std::uint64_t sampleTimed = 0, sampleNs = 0;  // sampleTexture
    std::uint64_t shadeCalls = 0, shadeTimed = 0, shadeNs = 0;  // shadePixel (pixels sombreados)
    // Pixels por desenho (cópia do histograma do produtor).
    std::uint64_t pixHist[BandStats::kPixBuckets] = {};
    std::uint64_t pixHistTex[BandStats::kPixBuckets] = {};
};

// Simulação da taxa de acerto de um cache de texturas (ANYPS2_PROFILE; só o produtor
// mexe). Não guarda texels: usa as gerações por página da VRAM (Gs::pageGen_) para
// dizer se a textura de um desenho seria a mesma versão da última vista com a mesma
// chave, e por que não foi. Não muda o comportamento nem a imagem.
struct TexSimReport {
    // Motivo do miss: a escrita mais recente numa página da textura que mudou.
    enum Miss : unsigned { MissFirst, MissFrame, MissZbuf, MissHost, MissCopy, MissCount };
    // Desenhos texturizados fora do cache (mesmo motivo único de BandStats::self).
    enum Off : unsigned { OffWrap, OffTexture, OffFrameZ, OffCount };
    std::uint64_t texDraws = 0;  // desenhos texturizados vistos
    std::uint64_t hits = 0;
    std::uint64_t miss[MissCount] = {};
    std::uint64_t off[OffCount] = {};
};

// Cache de texels crus (ANYPS2_GS_TEXCACHE=1): contadores das faixas (somas, lidas de
// relance) e do produtor. Não mudam o comportamento nem a imagem.
struct TexCacheReport {
    bool enabled = false;
    // Faixas (só contam com o perfil ligado).
    std::uint64_t blockHits = 0;    // texels servidos por bloco já pronto
    std::uint64_t blockFills = 0;   // blocos preenchidos (por leitura da VRAM)
    std::uint64_t fallbacks = 0;    // texels fora da grade do nível, lidos direto da VRAM
    // Produtor.
    std::uint64_t texDraws = 0;     // desenhos texturizados que receberam versões
    std::uint64_t offDraws = 0;     // desenhos texturizados sem cache (selfWrap/selfTexture/selfFrameZ)
    std::uint64_t versions = 0;     // versões criadas
    std::uint64_t reused = 0;       // níveis atendidos pela versão corrente
    std::uint64_t evictions = 0;    // versões despejadas do índice
    std::uint64_t bytes = 0;        // bytes no índice agora
    std::uint64_t peakBytes = 0;    // pico do índice
    std::uint64_t liveBytes = 0;    // bytes vivos (índice + versões em voo)
    std::uint64_t peakLiveBytes = 0;
    std::uint64_t entries = 0;
    std::uint64_t budget = 0;
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

    // Compõe a imagem exibida (circuitos de leitura 1 e 2, PMODE, BGCOLOR). Síncrono:
    // espera as faixas e lê a VRAM de agora.
    Frame display() const;
    bool displayEnabled() const;

    // Saída de vídeo sem esperar as faixas (ANYPS2_GS_ASYNC_DISPLAY=0 desliga; só vale com
    // worker em thread). requestDisplay captura os registradores de vídeo agora, submete
    // as palavras da HOST→LOCAL em fila e enfileira uma barreira que compõe o quadro quando
    // todas as faixas chegam nela: o quadro é idêntico ao display() deste ponto. O EE só
    // espera se o quadro pedido antes ainda não ficou pronto (nunca mais de um quadro de
    // atraso; conta em waits[WaitDisplay] e displayLagWaits). latestDisplay entrega o
    // quadro pronto mais recente que ainda não foi entregue, sem esperar: troca-o com
    // `out` (o buffer antigo de `out` volta para a caixa de correio e é reaproveitado) e
    // devolve verdadeiro; falso se não há quadro novo. Funciona também sem thread (a
    // barreira roda na hora), o que serve de referência nos testes.
    bool asyncDisplay() const { return asyncDisplay_; }
    void requestDisplay();
    bool latestDisplay(Frame& out);

    // Memória local: espera o worker (quem lê ou escreve aqui vê os pixels já
    // desenhados pelo GIF até este ponto).
    // A versão não const entrega a VRAM para escrita fora dos pontos que bumpPages
    // audita (testes e ferramentas): conta como escrita em tudo, para o cache de
    // texturas nunca servir texels velhos.
    Vram& vram() {
        waitIdle(BandStats::WaitVram);
        bumpAllPages();
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
    // (A espera das faixas nas barreiras é lida do worker a cada chamada.)
    const BandStats& bandStats() const;
    // Diagnóstico de texturas: soma das faixas (leituras sem espera, aproximadas
    // enquanto elas desenham) e o histograma de pixels por desenho do produtor.
    TexReport texReport() const;

    // Simulação do cache de texturas (fatia 3 do plano): contadores do produtor. Só
    // anda com o perfil ligado ou com forceTexSim(true) (testes).
    const TexSimReport& texSim() const { return texSim_; }
    void forceTexSim(bool on) { texSimForced_ = on; }
    // Liga os contadores de CLUT e o tempo de espera das barreiras sem o perfil (testes).
    void forceClutMeasure(bool on) {
        forceClutMeasure_ = on;
        worker_->setBarrierTiming(on);
    }
    // Cache de texels crus (gs_texcache.h). Desligado por padrão: ANYPS2_GS_TEXCACHE=1 liga;
    // ANYPS2_GS_TEXCACHE_MB limita o índice (padrão 256); ANYPS2_GS_TEXCACHE_VERIFY=1
    // liga o cache (se ainda não estiver) e confere cada leitura do cache com a VRAM.
    bool texCacheEnabled() const { return texCache_ != nullptr; }
    TexCacheReport texCacheReport() const;
    // Leituras do cache que diferiram da VRAM (VERIFY). Com abort ligado (padrão) a
    // primeira diferença imprime o desenho, o nível, (x, y) e os valores e aborta;
    // os testes desligam o abort para contar.
    std::uint64_t texVerifyMismatches() const { return verifyMismatches_.load(std::memory_order_relaxed); }
    void setTexVerifyAbort(bool abortOnMismatch) { verifyAbort_ = abortOnMismatch; }
    // Só para testes: geração atual de uma página da VRAM (8 KB; 0..511).
    std::uint32_t pageGeneration(unsigned page) const { return pageState_[page].gen; }

    // Só para testes (gs_texel): os parâmetros de textura de um desenho, para buscar
    // um texel sem montar um desenho inteiro.
    struct TexelProbe {
        std::uint32_t tbp[7] = {}, tbw[7] = {};
        std::uint32_t tpsm = 0, tw = 0, th = 0, cpsm = 0, csa = 0;
        std::shared_ptr<const ClutBuffer> clut;
        unsigned wms = 0, wmt = 0, minu = 0, maxu = 0, minv = 0, maxv = 0;
        std::uint32_t ta0 = 0, ta1 = 0;
        bool aem = false;
    };
    std::uint32_t probeTexel(const TexelProbe& p, unsigned level, int u, int v);
    // Copia a paleta memorizada (n entradas, 16 ou 256) que a faixa 0 usaria.
    void probePalette(const TexelProbe& p, unsigned n, std::uint32_t* out) const;
    // Quantas vezes a paleta da faixa 0 foi reconstruída (para conferir a chave).
    std::uint64_t paletteBuilds() const { return lanePal_[0].builds; }

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
    DisplayState captureDisplay() const;
    // Compõe o quadro de `st` lendo `vram` em `out` (reaproveita o buffer de `out`).
    static void composeFrame(const Vram& vram, const DisplayState& st, Frame& out);
    // Os retângulos de framebuffer que a composição de `st` lê (até 2); devolve quantos.
    unsigned displayAccesses(const DisplayState& st, VramAccess (&out)[2]) const;
    [[noreturn]] void unsupported(const std::string& what, std::uint32_t pc) const;
    void warnOnce(const std::string& what);

    // Operações enfileiradas (executadas pelas faixas, na ordem do GIF).
    // syncConst: a espera de leituras feitas por métodos const. Não muda o estado
    // observável; o único efeito é esperar as faixas terminarem.
    void syncConst(BandStats::WaitReason why = BandStats::WaitOther) const {
        const_cast<Gs*>(this)->waitIdle(why);
    }
    // Operação global: barreira em todas as faixas (ver GsWorker::barrier).
    void submit(GsWorker::Op op, BandStats::SubmitKind kind);
    // Esvazia os acessos pendentes (toda barreira o faz) e as leituras de CLUT que o
    // diagnóstico de WAR acompanha.
    void clearPending() {
        pending_.clear();
        clutReads_.clear();
    }
    // Diagnóstico da fatia 1 (não mudam ordem nem resultado): classifica uma carga de CLUT
    // que vai virar barreira; confere se escritas de desenho colidem com cargas pendentes.
    bool measureClut() const;
    bool noteClutBarrier(const VramAccess& src);  // devolve se a carga era coberta
    void noteClutWar(const VramAccess* writes, unsigned n);
    // Acessos diretos do produtor à VRAM (CLUT): vê se o que as faixas ainda podem
    // estar fazendo não toca as páginas. outstanding_ guarda os acessos de tudo o que
    // foi enfileirado desde que as faixas foram vistas ociosas (diferente de pending_,
    // que uma barreira esvazia, mas não termina na hora).
    bool trackOutstanding();
    // Registra um acesso que a próxima barreira enfileirada conclui (chamar depois
    // da barreira de conflito). HOST→LOCAL direto não entra (o produtor o escreve na
    // hora); em modo fila, entra a cada submissão, com o número da própria barreira.
    void noteOutstanding(VramAccess a);
    // Há acesso não concluído que colide com `a` (ou a lista perdeu o rastro).
    bool outstandingConflicts(const VramAccess& a);
    // Fim da transferência HOST→LOCAL aberta: em modo direto o produtor já escreveu tudo
    // na VRAM; em modo fila submete o que falta (flushHostQueue).
    void closeHostTransfer();
    // Modo fila: enfileira como barreira as palavras guardadas desde a última submissão (a
    // transferência segue aberta). Depois dela a VRAM, para quem vier na fila, tem tudo o
    // que o programa enviou até aqui. Sem palavras guardadas não faz nada.
    void flushHostQueue();
    // Modo fila: submete antes de uma operação que toca `acc` (n acessos) e precisa ver as
    // palavras já enviadas, ou não pode ser sobrescrita por elas fora de ordem.
    void flushHostQueueIfTouches(const VramAccess* acc, unsigned n);
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
    static void fillEnv(DrawEnv& e, const TexelProbe& p);
    const std::uint32_t* palette(const DrawEnv& e, unsigned n) const;
    void setupEnv(DrawEnv& e, std::uint32_t pc);
    // Gerações de página (produtor, ordem do programa). Toda escrita na VRAM que o
    // produtor realiza ou enfileira passa por aqui, ANTES de qualquer desenho que a
    // veja: uma versão de textura fixada por um desenho vale enquanto as gerações das
    // suas páginas não mudam. Pontos de escrita: FRAME/ZBUF de cada desenho (draw,
    // inclusive os que vão sozinhos), início e palavras da HOST→LOCAL (startTransfer,
    // writeTransferData) e o destino da LOCAL→LOCAL (localToLocal). A carga de CLUT só
    // lê a VRAM. Qualquer escrita nova na VRAM tem de chamar bumpPages.
    enum WriteKind : std::uint8_t { WkFrame, WkZbuf, WkHost, WkCopy };
    void bumpPages(const PageSet& pages, WriteKind kind);
    void bumpAllPages();
    // Níveis do cache de texturas que o desenho fixa (nulo: sem cache). Produtor.
    std::shared_ptr<TexLevel> texLevelFor(const DrawEnv& e, unsigned level);
    void texVerifyFail(const DrawEnv& e, unsigned level, unsigned x, unsigned y, std::uint32_t cached,
                       std::uint32_t direct) const;
    void simTexture(const DrawEnv& e, int offReason);
    void rasterize(const DrawEnv& e, const Vertex& v0, const Vertex& v1, const Vertex& v2);
    void drawPoint(const DrawEnv& e, const Vertex& v);
    void drawLine(const DrawEnv& e, const Vertex& a, const Vertex& b);
    void drawTriangle(const DrawEnv& e, const Vertex& a, const Vertex& b, const Vertex& c);
    void drawSprite(const DrawEnv& e, const Vertex& a, const Vertex& b);
    struct Fragment;
    void shadePixel(const DrawEnv& e, int x, int y, Fragment& f);
    std::uint32_t sampleTexture(const DrawEnv& e, float u, float v, float lod) const;
    std::uint32_t fetchTexel(const DrawEnv& e, unsigned level, int u, int v) const;
    std::uint32_t texelRaw(const DrawEnv& e, unsigned level, int u, int v) const;
    static std::uint32_t expandTexel(const DrawEnv& e, std::uint32_t raw);

    // Faixas do GS. Tudo que está abaixo de "faixas" é delas: o EE só mexe nesses
    // campos depois de waitIdle() ou dentro de uma operação global (barreira).
    std::unique_ptr<GsWorker> worker_;
    // Caixa de correio dos quadros compostos na fila (requestDisplay/latestDisplay). Vive
    // num shared_ptr porque as barreiras a guardam; o mutex protege tudo menos `compose`,
    // que só as barreiras de composição (serializadas pelas faixas) tocam.
    struct DisplayBox {
        std::mutex m;
        std::condition_variable cv;
        Frame ready;         // último quadro pronto (vale se readySeq != 0)
        Frame compose;       // buffer de trabalho da barreira
        std::uint64_t readySeq = 0;      // pedido do quadro em `ready`
        std::uint64_t deliveredSeq = 0;  // pedido do último quadro entregue
        std::uint64_t finished = 0;      // pedidos cuja barreira já terminou
    };
    std::shared_ptr<DisplayBox> displayBox_ = std::make_shared<DisplayBox>();
    std::uint64_t displayRequested_ = 0;  // produtor
    bool asyncDisplay_ = false;
    // Acessos à VRAM dos desenhos enfileirados desde a última barreira (produtor).
    PendingAccess pending_;
    // Leituras de CLUT feitas por barreira desde a última barreira (só o diagnóstico de
    // WAR; vazio sem perfil). covered: a carga era coberta.
    struct ClutRead {
        VramAccess src;
        bool covered = false;
    };
    std::vector<ClutRead> clutReads_;
    bool forceClutMeasure_ = false;
    PendingAccess outstanding_{256};  // produtor; ver trackOutstanding()
    PageSeqTracker outstandingPages_;  // produtor; o que decide (a lista acima fica para o perfil)
    std::uint64_t outstandingLostNeed_ = 0;  // lista cheia: sem rastro até a barreira desse número
    // Transferência HOST→LOCAL aberta (faltam palavras): o destino conta como escrita
    // em andamento enquanto o produtor ainda pode enviar dados.
    bool hostOpenValid_ = false;
    bool hostDrain_ = false;  // (modo direto) há operação enfileirada que toca o destino aberto: esperar antes da próxima palavra
    // Modo fila (só com worker em thread): o destino tinha acesso não concluído quando a
    // transferência abriu, então as palavras NÃO vão à VRAM no produtor: ficam em hostWords_
    // e flushHostQueue as passa a uma barreira que roda hostWord sobre hostQueuedXfer_ (o
    // estado da transferência, de posse das barreiras: o produtor só o cria). Operações do
    // produtor que tocam o destino submetem antes (flushHostQueueIfTouches, waitIdle), então
    // as enfileiradas depois veem a VRAM com as palavras já enviadas e as enfileiradas antes
    // não. Nunca há hostDrain_ neste modo.
    bool hostQueued_ = false;
    std::vector<std::uint64_t> hostWords_;
    std::shared_ptr<Transfer> hostQueuedXfer_;
    VramAccess hostOpen_;
    std::uint64_t hostWordsLeft_ = 0;
    // Um desenho fixou uma versão de textura cujas páginas cruzam o destino aberto: a
    // próxima palavra (modo direto) ou a próxima submissão do buffer (modo fila,
    // flushHostQueue) incrementa as páginas do destino antes de escrever (senão um
    // desenho posterior acertaria numa versão com o conteúdo parcial). A marca vale
    // pelo alcance de leitura do desenho (reads[] de draw: o que ele pode preencher),
    // não pela grade inteira da versão. Basta: um bloco do cache cabe numa página lógica
    // do alcance, e coordenadas fora da grade w×h vão direto para readPixel.
    bool hostGenDirty_ = false;

    // Geração por página, época da última escrita e quem escreveu (só o produtor).
    struct PageState {
        std::uint64_t epoch = 0;
        std::uint32_t gen = 0;
        std::uint32_t kind = 0;
    };
    std::array<PageState, PageSet::kPages> pageState_{};
    std::uint64_t writeEpoch_ = 0;  // uma por incremento (evento de escrita)

    // Simulação do cache (ver TexSimReport).
    struct TexSimKey {
        std::array<std::uint32_t, 4 + 14> v{};
        bool operator==(const TexSimKey& o) const { return v == o.v; }
    };
    struct TexSimKeyHash {
        std::size_t operator()(const TexSimKey& k) const {
            std::uint64_t h = 1469598103934665603ull;
            for (std::uint32_t x : k.v) h = (h ^ x) * 1099511628211ull;
            return static_cast<std::size_t>(h);
        }
    };
    std::unordered_map<TexSimKey, std::vector<std::uint32_t>, TexSimKeyHash> texSimSeen_;
    std::vector<std::uint32_t> texSimScratch_;
    TexSimReport texSim_;
    bool texSimForced_ = false;

    // Cache de texels crus (nulo = desligado). O índice é do produtor; as versões
    // (TexLevel) são lidas e preenchidas pelas faixas.
    std::unique_ptr<TexCache> texCache_;
    bool texVerify_ = false;
    bool verifyAbort_ = true;
    mutable std::atomic<std::uint64_t> verifyMismatches_{0};
    std::vector<std::uint32_t> texGenScratch_;
    std::uint64_t texCacheDraws_ = 0, texCacheOff_ = 0;  // produtor

    Runtime* rt_;
    Vram vram_;  // worker
    std::array<std::uint64_t, 256> regs_{};
    Context ctx_[2];
    // CLUT corrente (versão mais recente; produtor).
    std::shared_ptr<ClutBuffer> clut_ = std::make_shared<ClutBuffer>();
    // Última versão carregada por barreira (produtor) e quantas cargas há desde que a
    // cadeia foi materializada: limitam o tamanho da cadeia de versões não resolvidas.
    static constexpr unsigned kClutChainLimit = 64;
    std::shared_ptr<ClutBuffer> clutBarrier_;
    unsigned clutChain_ = 0;
    // ANYPS2_GS_CLUT_DEP (ligado; '0' desliga): cargas "cobertas" não viram barreira.
    bool clutDep_ = true;
    unsigned clutLazyLane_ = 0;  // faixa que recebe a próxima tarefa de leitura diferida
    // Versões diferidas criadas depois da última de barreira (esta, ao ficar pronta, garante
    // as anteriores): o produtor só compacta a cadeia quando todas estão prontas.
    std::vector<std::shared_ptr<const ClutBuffer>> clutLazies_;
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
    // Contadores de textura por faixa (só com ANYPS2_PROFILE; ver TexReport). Cada
    // faixa escreve os seus (carga + store, sem instrução atômica de leitura-
    // modificação-escrita) e o produtor lê de relance; a linha de cache é da faixa.
    struct alignas(64) LaneTex {
        std::atomic<std::uint64_t> fetch[TexReport::FmtCount] = {};
        std::atomic<std::uint64_t> samples{0}, linear{0}, trilinear{0};
        std::atomic<std::uint64_t> sampleTimed{0}, sampleNs{0};
        std::atomic<std::uint64_t> shadeCalls{0}, shadeTimed{0}, shadeNs{0};
        std::atomic<std::uint64_t> cacheHit{0}, cacheFill{0}, cacheFallback{0};
    };
    mutable std::array<LaneTex, kMaxLanes> laneTex_{};
    // Paleta já expandida (CLUT + TEXA) da última versão de CLUT vista por cada faixa:
    // 16 entradas (T4) ou 256 (T8). Estado da faixa, sem sincronização (como
    // laneShaded_); segura a ClutBuffer para o endereço não ser reaproveitado por
    // outra versão enquanto a chave vale. O modo síncrono usa só a faixa 0.
    struct LanePal {
        std::shared_ptr<const ClutBuffer> clut;
        std::uint32_t cpsm = 0, csa = 0, ta0 = 0, ta1 = 0;
        bool aem = false;
        unsigned n = 0;  // entradas válidas (0: vazia)
        std::uint64_t builds = 0;
        std::array<std::uint32_t, 256> pal{};
    };
    mutable std::array<LanePal, kMaxLanes> lanePal_{};
    std::set<std::string> warned_;

    // Diagnóstico: VBlanks recebidos (vblankStart) e o rastreador, nulo sem as
    // variáveis de ambiente. O produtor é o único que mexe nisto.
    std::uint64_t vblanks_ = 0;
    unsigned source_ = 0;
    std::uint64_t xgkicks_ = 0;
    std::unique_ptr<GsTrace> trace_;
};

}  // namespace anyps2::rt::gs
