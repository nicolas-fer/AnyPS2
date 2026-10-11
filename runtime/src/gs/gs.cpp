#include "anyps2/runtime/gs/gs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>
#include <vector>

#include "anyps2/common/error.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/gs/gs_bands.h"
#include "anyps2/runtime/gs/gs_trace.h"
#include "anyps2/runtime/host_profile.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt::gs {

namespace {

constexpr std::uint64_t bits(std::uint64_t v, unsigned lo, unsigned n) {
    return (v >> lo) & ((n >= 64) ? ~0ull : ((1ull << n) - 1));
}

// ANYPS2_GS_THREAD=0 desliga o worker: todo desenho roda na thread do EE, como
// antes. Serve para comparar a saída das duas formas.
bool threadedFromEnv() {
    const char* v = std::getenv("ANYPS2_GS_THREAD");
    return !(v && std::strcmp(v, "0") == 0);
}

// ANYPS2_GS_THREADS=N escolhe as faixas (1 = todas as linhas numa faixa só, com um único worker). Sem a variável: min(4, núcleos − 2), no mínimo 2, para deixar
// núcleos para o EE e o VU1.
unsigned lanesFromEnv() {
    const char* v = std::getenv("ANYPS2_GS_THREADS");
    if (v && *v) {
        const long n = std::strtol(v, nullptr, 10);
        if (n >= 1) return static_cast<unsigned>(std::min<long>(n, Gs::kMaxLanes));
    }
    const auto cores = static_cast<int>(std::thread::hardware_concurrency());
    return static_cast<unsigned>(std::max(2, std::min(4, cores - 2)));
}

float asFloat(std::uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// Bits de evento do CSR
constexpr unsigned kSignal = 0, kFinish = 1, kHsint = 2, kVsint = 3;
constexpr std::uint64_t kCsrField = 1ull << 13;
constexpr std::uint64_t kCsrReset = 1ull << 9;
// ID 0x55, revisão 0x1B, FIFO vazio (bits 14–15 = 01).
constexpr std::uint64_t kCsrConst = 0x551B0000ull | (1ull << 14);

struct RegNameEntry {
    std::uint8_t reg;
    const char* name;
};
constexpr RegNameEntry kRegNames[] = {
    {PRIM, "PRIM"}, {RGBAQ, "RGBAQ"}, {ST, "ST"}, {UV, "UV"}, {XYZF2, "XYZF2"}, {XYZ2, "XYZ2"},
    {TEX0_1, "TEX0_1"}, {TEX0_2, "TEX0_2"}, {CLAMP_1, "CLAMP_1"}, {CLAMP_2, "CLAMP_2"}, {FOG, "FOG"},
    {XYZF3, "XYZF3"}, {XYZ3, "XYZ3"}, {TEX1_1, "TEX1_1"}, {TEX1_2, "TEX1_2"}, {TEX2_1, "TEX2_1"},
    {TEX2_2, "TEX2_2"}, {XYOFFSET_1, "XYOFFSET_1"}, {XYOFFSET_2, "XYOFFSET_2"},
    {PRMODECONT, "PRMODECONT"}, {PRMODE, "PRMODE"}, {TEXCLUT, "TEXCLUT"}, {SCANMSK, "SCANMSK"},
    {MIPTBP1_1, "MIPTBP1_1"}, {MIPTBP1_2, "MIPTBP1_2"}, {MIPTBP2_1, "MIPTBP2_1"},
    {MIPTBP2_2, "MIPTBP2_2"}, {TEXA, "TEXA"}, {FOGCOL, "FOGCOL"}, {TEXFLUSH, "TEXFLUSH"},
    {SCISSOR_1, "SCISSOR_1"}, {SCISSOR_2, "SCISSOR_2"}, {ALPHA_1, "ALPHA_1"}, {ALPHA_2, "ALPHA_2"},
    {DIMX, "DIMX"}, {DTHE, "DTHE"}, {COLCLAMP, "COLCLAMP"}, {TEST_1, "TEST_1"}, {TEST_2, "TEST_2"},
    {PABE, "PABE"}, {FBA_1, "FBA_1"}, {FBA_2, "FBA_2"}, {FRAME_1, "FRAME_1"}, {FRAME_2, "FRAME_2"},
    {ZBUF_1, "ZBUF_1"}, {ZBUF_2, "ZBUF_2"}, {BITBLTBUF, "BITBLTBUF"}, {TRXPOS, "TRXPOS"},
    {TRXREG, "TRXREG"}, {TRXDIR, "TRXDIR"}, {HWREG, "HWREG"}, {SIGNAL, "SIGNAL"}, {FINISH, "FINISH"},
    {LABEL, "LABEL"},
};

}  // namespace

const char* regName(std::uint8_t reg) {
    for (const auto& r : kRegNames) {
        if (r.reg == reg) return r.name;
    }
    return "";
}

Gs::Gs(Runtime* rt)
    : worker_(std::make_unique<GsWorker>(threadedFromEnv(), lanesFromEnv())), rt_(rt) {
    regs_[PRMODECONT] = 1;
    csr_ = 0;
    trace_ = GsTrace::fromEnv();
}

Gs::~Gs() {
    // Antes de qualquer membro ser destruído: as faixas ainda usam a VRAM e as versões da CLUT.
    worker_->stop();
}

GsTrace& Gs::trace() {
    if (!trace_) trace_ = std::make_unique<GsTrace>();
    return *trace_;
}

bool Gs::trackOutstanding() {
    if (worker_->idle()) {
        outstanding_.clear();
        outstandingLostNeed_ = 0;
        return true;
    }
    const std::uint64_t done = worker_->barriersDone();
    outstanding_.dropDone(done);
    return outstandingLostNeed_ <= done;
}

void Gs::noteOutstanding(VramAccess a) {
    a.seq = worker_->barriersIssued() + 1;
    if (outstanding_.full()) {
        outstandingLostNeed_ = std::max(outstandingLostNeed_, a.seq);
        return;
    }
    outstanding_.add(a);
}

bool Gs::outstandingConflicts(const VramAccess& a) {
    if (!trackOutstanding()) return true;
    // A transferência aberta é escrita pelo próprio produtor, na ordem do programa,
    // então não colide com o que ele mesmo faz: só o que está nas faixas conta.
    return outstanding_.conflictsWith(a);
}

void Gs::noteHostHazard(const VramAccess* acc, unsigned n) {
    if (!hostOpenValid_ || hostDrain_) return;
    for (unsigned i = 0; i < n; ++i) {
        if (accessesOverlap(hostOpen_, acc[i])) {
            // A operação já está nas faixas e as palavras que faltam ainda não foram
            // escritas: uma leitura (textura, CLUT, cópia) tem de ver só as que já foram, e
            // uma escrita não pode ser sobrescrita fora de ordem. A próxima palavra espera
            // as faixas terminarem (writeTransferData); uma espera cobre todas as operações
            // enfileiradas até lá. Não dá para saber, sem refazer o endereçamento da
            // palavra, se ela cai na área da operação: a espera é feita por garantia.
            hostDrain_ = true;
            return;
        }
    }
}

void Gs::closeHostTransfer() {
    // Tudo o que foi enviado já está na VRAM (modo direto): não há nada a concluir.
    hostOpenValid_ = false;
    hostDrain_ = false;
}

// Retângulo de pixels de uma transferência como acesso. Fora de 0..2047 o
// endereço dá a volta (x e y são cortados em 2047): vale a VRAM inteira.
VramAccess Gs::rectAccess(bool write, std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t x0,
                          std::uint32_t y0, std::uint32_t w, std::uint32_t h) const {
    VramAccess a;
    a.write = write;
    a.surface = 3;
    a.base = bp / 32;
    a.bw = bw;
    a.psm = psm;
    if (w == 0 || h == 0 || x0 + w > 2048 || y0 + h > 2048) {
        a.span = VramSpan{0, Vram::kSize / 8192 - 1};
        return a;
    }
    const PageSpans p = pageSpan(psm, bp, bw, y0, y0 + h - 1, x0, x0 + w - 1);
    a.span = p.span;
    a.wrap = p.wrap;
    return a;
}

void Gs::waitIdle(BandStats::WaitReason why) {
    // Depois da espera nada está pendente nas faixas: os acessos de desenhos já
    // enfileirados não podem mais colidir com a ordem de uma transferência aberta.
    hostDrain_ = false;
    pending_.clear();
    if (!worker_->threaded()) {
        trackOutstanding();
        return;
    }
    // Conta as esperas do EE pelas faixas (no modo síncrono não há espera).
    ++bandStats_.waits[why];
    const HostProfile::Scope prof(HostProfile::GsWait);
    if (!HostProfile::enabled()) {
        worker_->drain();
        trackOutstanding();
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    worker_->drain();
    trackOutstanding();
    bandStats_.waitNs[why] +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
}

std::uint64_t Gs::pixelsShaded() const {
    syncConst(BandStats::WaitOther);
    std::uint64_t n = 0;
    for (const LaneCount& c : laneShaded_) n += c.shaded;
    return n;
}

void Gs::submit(GsWorker::Op op, BandStats::SubmitKind kind) {
    ++bandStats_.submit[kind];
    // A operação global espera as faixas, então nada pendente sobrevive a ela.
    pending_.clear();
    worker_->barrier(std::move(op));
}

void Gs::unsupported(const std::string& what, std::uint32_t pc) const {
    throw Unimplemented("GS: " + what, pc);
}

void Gs::warnOnce(const std::string& what) {
    if (warned_.insert(what).second) {
        std::fprintf(stderr, "anyps2: aviso: GS: %s\n", what.c_str());
    }
}

// ---------------------------------------------------------------------------
// Registradores privilegiados
// ---------------------------------------------------------------------------

void Gs::setHsyncSource(std::uint64_t (*now)(void*), void* ctx, std::uint64_t cyclesPerLine) {
    hsyncNow_ = now;
    hsyncCtx_ = ctx;
    cyclesPerLine_ = cyclesPerLine;
}

void Gs::raiseEvent(unsigned bit) {
    const std::uint64_t mask = 1ull << bit;
    if (csr_ & mask) return;
    csr_ |= mask;
    // IMR: bits 8–12 mascaram SIGNAL, FINISH, HSINT, VSINT, EDWINT.
    if (!(imr_ & (1ull << (bit + 8))) && rt_) rt_->kernel().raiseIntc(0);  // INTC_GS
}

// Custo (ciclos do EE, 294 MHz) usado para atrasar o FINISH: o GIF entrega
// ~1 registrador por ciclo do barramento (147 MHz) e o GS preenche ~8 pixels
// por ciclo do GS. É uma estimativa: só precisa ser da ordem do hardware para
// que padrões como "envia DMA → limpa VSINT/FINISH → espera FINISH" funcionem.
constexpr std::uint64_t kCyclesPerRegister = 2;
constexpr std::uint64_t kPixelsPerCycle = 4;
constexpr std::uint64_t kNoEvent = ~std::uint64_t{0};

std::uint64_t Gs::nextEventTime() const {
    return finishDue_.empty() ? kNoEvent : finishDue_.front();
}

void Gs::processEvents(std::uint64_t now) {
    std::size_t n = 0;
    while (n < finishDue_.size() && finishDue_[n] <= now) ++n;
    if (n == 0) return;
    if (realTime_) {
        // Relógio real: o GS em software leva tempo de verdade, e dois FINISH
        // separados no hardware podem vencer juntos aqui — uma única limpeza
        // do CSR apagaria os dois e quem espera o segundo esperaria para
        // sempre. No relógio real o FINISH espera o worker, então o desenho já
        // terminou e um FINISH adiantado não faz mal: entrega um por vez, o
        // próximo depois que o programa limpar o anterior.
        if (csr_ & (1ull << kFinish)) return;
        finishDue_.erase(finishDue_.begin());
        raiseEvent(kFinish);
        return;
    }
    finishDue_.erase(finishDue_.begin(), finishDue_.begin() + static_cast<std::ptrdiff_t>(n));
    raiseEvent(kFinish);
}

std::uint64_t Gs::csr() {
    if (hsyncNow_ && cyclesPerLine_ && !(csr_ & (1ull << kHsint))) {
        const std::uint64_t t = hsyncNow_(hsyncCtx_);
        if (t / cyclesPerLine_ > hsyncClearedAt_ / cyclesPerLine_) csr_ |= 1ull << kHsint;
    }
    return csr_ | kCsrConst;
}

void Gs::setImr(std::uint64_t v) {
    imr_ = v & 0x7F00;
}

void Gs::setCrt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field) {
    // SMODE1 com o modo de vídeo; SMODE2: INT e FFMD.
    priv_[1] = (std::uint64_t{mode} << 8);
    priv_[2] = (interlace & 1) | ((field & 1) << 1);
}

std::uint64_t Gs::readPrivileged(std::uint32_t addr, std::uint32_t pc) {
    const std::uint32_t off = addr - 0x12000000u;
    if (off < 0xF0) return priv_[off >> 4];
    switch (off & ~0xFu) {
        case 0x1000: return csr();
        case 0x1010: return imr_;
        case 0x1040: return 0;  // BUSDIR
        case 0x1080: return siglblid_;
        default: break;
    }
    throw Unimplemented("leitura de registrador privilegiado do GS inexistente " + anyps2::hex(addr), pc);
}

void Gs::writePrivileged(std::uint32_t addr, std::uint64_t value, std::uint32_t pc) {
    const std::uint32_t off = addr - 0x12000000u;
    if (off < 0xF0) {
        priv_[off >> 4] = value;
        return;
    }
    switch (off & ~0xFu) {
        case 0x1000:
            if (value & kCsrReset) {
                // Reset do GS: cancela transferência e fila de vértices, limpa eventos.
                // Os desenhos já enfileirados terminam antes (no hardware e na
                // versão síncrona eles já tinham sido feitos); só o estado de
                // transferência é zerado, pela própria fila, na mesma ordem.
                closeHostTransfer();
                xferDirect_ = {};
                submit([] {}, BandStats::SubmitReset);
                waitIdle();
                queued_ = 0;
                csr_ &= kCsrField;
                finishDue_.clear();
                work_ = 0;
                pixels_ = 0;
                return;
            }
            // SIGNAL/FINISH/HSINT/VSINT/EDWINT: escrever 1 limpa.
            if (value & (1ull << kHsint) && hsyncNow_) hsyncClearedAt_ = hsyncNow_(hsyncCtx_);
            csr_ &= ~(value & 0x1F);
            return;
        case 0x1010: setImr(value); return;
        case 0x1040: busdir_ = value & 1; return;  // BUSDIR
        case 0x1080: siglblid_ = value; return;
        default: break;
    }
    throw Unimplemented("escrita em registrador privilegiado do GS inexistente " + anyps2::hex(addr), pc);
}

// ---------------------------------------------------------------------------
// VBlank
// ---------------------------------------------------------------------------

void Gs::vblankStart() {
    ++vblanks_;
    csr_ ^= kCsrField;
    raiseEvent(kVsint);
}

void Gs::vblankEnd() {}

// ---------------------------------------------------------------------------
// Registradores gerais
// ---------------------------------------------------------------------------

void Gs::addWork(std::uint64_t cycles) {
    // O GS começa um lote de trabalho quando os dados chegam: o FINISH
    // depende do trabalho desde então, não de tudo o que veio antes.
    if (work_ == 0 && pixels_ == 0 && hsyncNow_) workStart_ = hsyncNow_(hsyncCtx_);
    work_ += cycles;
}

void Gs::writeRegister(std::uint8_t reg, std::uint64_t v, std::uint32_t pc) {
    regs_[reg] = v;
    addWork(kCyclesPerRegister);
    switch (reg) {
        case PRIM:
            queued_ = 0;
            return;
        case RGBAQ:
            current_.r = static_cast<std::uint8_t>(v);
            current_.g = static_cast<std::uint8_t>(v >> 8);
            current_.b = static_cast<std::uint8_t>(v >> 16);
            current_.a = static_cast<std::uint8_t>(v >> 24);
            current_.q = asFloat(static_cast<std::uint32_t>(v >> 32));
            return;
        case ST:
            current_.s = asFloat(static_cast<std::uint32_t>(v));
            current_.t = asFloat(static_cast<std::uint32_t>(v >> 32));
            return;
        case UV:
            current_.u = static_cast<std::uint32_t>(bits(v, 0, 14));
            current_.v = static_cast<std::uint32_t>(bits(v, 16, 14));
            return;
        case XYZF2: case XYZF3:
            current_.x = static_cast<std::int32_t>(bits(v, 0, 16));
            current_.y = static_cast<std::int32_t>(bits(v, 16, 16));
            current_.z = static_cast<std::uint32_t>(bits(v, 32, 24));
            current_.fog = static_cast<std::uint8_t>(v >> 56);
            vertexKick(reg == XYZF2, pc);
            return;
        case XYZ2: case XYZ3:
            current_.x = static_cast<std::int32_t>(bits(v, 0, 16));
            current_.y = static_cast<std::int32_t>(bits(v, 16, 16));
            current_.z = static_cast<std::uint32_t>(v >> 32);
            vertexKick(reg == XYZ2, pc);
            return;
        case FOG:
            current_.fog = static_cast<std::uint8_t>(v >> 56);
            return;
        case TEX0_1: case TEX0_2:
            writeTex0(reg - TEX0_1, v, pc);
            return;
        case TEX2_1: case TEX2_2: {
            // TEX2 só altera PSM e os campos de CLUT do TEX0.
            const std::uint64_t mask = (0x3Full << 20) | (~0ull << 37);
            const unsigned n = reg - TEX2_1;
            writeTex0(n, (ctx_[n].tex0 & ~mask) | (v & mask), pc);
            return;
        }
        case CLAMP_1: case CLAMP_2: ctx_[reg - CLAMP_1].clamp = v; return;
        case TEX1_1: case TEX1_2: ctx_[reg - TEX1_1].tex1 = v; return;
        case XYOFFSET_1: case XYOFFSET_2: ctx_[reg - XYOFFSET_1].xyoffset = v; return;
        case MIPTBP1_1: case MIPTBP1_2: ctx_[reg - MIPTBP1_1].miptbp1 = v; return;
        case MIPTBP2_1: case MIPTBP2_2: ctx_[reg - MIPTBP2_1].miptbp2 = v; return;
        case SCISSOR_1: case SCISSOR_2: ctx_[reg - SCISSOR_1].scissor = v; return;
        case ALPHA_1: case ALPHA_2: ctx_[reg - ALPHA_1].alpha = v; return;
        case TEST_1: case TEST_2: ctx_[reg - TEST_1].test = v; return;
        case FBA_1: case FBA_2: ctx_[reg - FBA_1].fba = v; return;
        case FRAME_1: case FRAME_2: ctx_[reg - FRAME_1].frame = v; return;
        case ZBUF_1: case ZBUF_2: ctx_[reg - ZBUF_1].zbuf = v; return;
        case PRMODECONT: case PRMODE: case TEXCLUT: case SCANMSK: case TEXA: case FOGCOL:
        case TEXFLUSH: case DIMX: case DTHE: case COLCLAMP: case PABE: case BITBLTBUF: case TRXPOS:
        case TRXREG:
            return;
        case TRXDIR:
            startTransfer(pc);
            return;
        case HWREG:
            writeTransferData(v, pc);
            return;
        case SIGNAL: {
            const std::uint64_t mask = v >> 32;
            siglblid_ = (siglblid_ & ~mask) | (v & mask & 0xFFFFFFFFull);
            // O sinal vale depois de todos os desenhos anteriores.
            waitIdle(BandStats::WaitSignal);
            raiseEvent(kSignal);
            return;
        }
        case FINISH: {
            // Com relógio real o FINISH só aparece quando o trabalho do GS acabou
            // de fato, então quem o recebe já pode ler a memória. Com o relógio
            // virtual o tempo vem da conta analítica e o worker segue sem espera.
            if (realTime_) waitIdle(BandStats::WaitFinish);
            if (!hsyncNow_) {
                raiseEvent(kFinish);
                return;
            }
            const std::uint64_t now = hsyncNow_(hsyncCtx_);
            const std::uint64_t start = std::max(busyUntil_, workStart_);
            busyUntil_ = std::max(start + work_ + pixels_ / kPixelsPerCycle, now + kCyclesPerRegister);
            work_ = 0;
            pixels_ = 0;
            finishDue_.push_back(busyUntil_);
            return;
        }
        case LABEL: {
            const std::uint64_t mask = (v >> 32) << 32;
            siglblid_ = (siglblid_ & ~mask) | ((v << 32) & mask);
            return;
        }
        default:
            break;
    }
    unsupported("escrita no registrador geral inexistente " + anyps2::hex(reg, 2) + " (valor " +
                    anyps2::hex(v) + ")",
                pc);
}

void Gs::writeTex0(unsigned n, std::uint64_t v, std::uint32_t pc) {
    ctx_[n].tex0 = v;
    const std::uint32_t psm = static_cast<std::uint32_t>(bits(v, 20, 6));
    const bool indexed = psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH;
    const std::uint32_t cbp = static_cast<std::uint32_t>(bits(v, 37, 14));
    bool load = false;
    switch (bits(v, 61, 3)) {
        case 0: break;
        case 1: load = true; break;
        case 2: load = true; cbp0_ = cbp; break;
        case 3: load = true; cbp1_ = cbp; break;
        case 4: load = cbp != cbp0_; cbp0_ = cbp; break;
        case 5: load = cbp != cbp1_; cbp1_ = cbp; break;
        default:
            unsupported("TEX0.CLD = " + std::to_string(bits(v, 61, 3)) + " (reservado)", pc);
    }
    if (load && indexed) {
        if (trace_) trace_->vramClutLoad(*this, v, pc);
        loadClut(v, pc);
    }
}

void Gs::loadClut(std::uint64_t tex0, std::uint32_t pc) {
    const std::uint32_t cpsm = static_cast<std::uint32_t>(bits(tex0, 51, 4));
    const bool csm2 = bits(tex0, 55, 1) != 0;
    if (cpsm != PSMCT32 && cpsm != PSMCT16 && cpsm != PSMCT16S) {
        unsupported("CLUT com CPSM inválido " + psmName(cpsm), pc);
    }
    if (csm2 && cpsm == PSMCT32) unsupported("CLUT CSM2 com CPSM=PSMCT32 (proibido pelo hardware)", pc);
    // TEXCLUT é lido agora, na ordem do programa. A CLUT é um buffer imutável por
    // versão: a carga cria uma versão nova (cópia da corrente com as células
    // trocadas) e os desenhos seguintes a levam consigo, então não há estado global
    // para as faixas e a carga não precisa de barreira, desde que o produtor possa
    // ler a área da CLUT na VRAM agora: nenhum acesso ainda não concluído das faixas
    // (desenho ou LOCAL→LOCAL) a escreve. Senão a carga vira uma operação global.
    const std::uint64_t texclut = regs_[TEXCLUT];
    const VramAccess src = clutSource(tex0, texclut);
    auto next = std::make_shared<ClutBuffer>();
    if (clut_->ready.load(std::memory_order_acquire) && !outstandingConflicts(src)) {
        next->v = clut_->v;
        loadClutCells(*next, tex0, texclut);
        clut_ = std::move(next);
        ++bandStats_.clutDirect;
        return;
    }
    noteOutstanding(src);
    next->ready.store(false, std::memory_order_relaxed);
    submit([this, prev = clut_, next, tex0, texclut] {
        next->v = prev->v;
        loadClutCells(*next, tex0, texclut);
        next->ready.store(true, std::memory_order_release);
    }, BandStats::SubmitClut);
    noteHostHazard(&src, 1);
    clut_ = std::move(next);
}

// Área da VRAM que a carga lê (leitura), a mesma de loadClutCells.
VramAccess Gs::clutSource(std::uint64_t tex0, std::uint64_t texclut) const {
    const std::uint32_t psm = static_cast<std::uint32_t>(bits(tex0, 20, 6));
    const std::uint32_t cbp = static_cast<std::uint32_t>(bits(tex0, 37, 14));
    const std::uint32_t cpsm = static_cast<std::uint32_t>(bits(tex0, 51, 4));
    const bool csm2 = bits(tex0, 55, 1) != 0;
    const bool eight = psm == PSMT8 || psm == PSMT8H;
    const unsigned count = eight ? 256 : 16;
    if (csm2) {
        const auto x0 = static_cast<std::uint32_t>(bits(texclut, 6, 6)) * 16;
        return rectAccess(false, cpsm, cbp, static_cast<std::uint32_t>(bits(texclut, 0, 6)), x0,
                          static_cast<std::uint32_t>(bits(texclut, 12, 10)), count, 1);
    }
    return eight ? rectAccess(false, cpsm, cbp, 1, 0, 0, 16, 16) : rectAccess(false, cpsm, cbp, 1, 0, 0, 8, 2);
}

void Gs::loadClutCells(ClutBuffer& dst, std::uint64_t tex0, std::uint64_t texclut) const {
    const std::uint32_t psm = static_cast<std::uint32_t>(bits(tex0, 20, 6));
    const std::uint32_t cbp = static_cast<std::uint32_t>(bits(tex0, 37, 14));
    const std::uint32_t cpsm = static_cast<std::uint32_t>(bits(tex0, 51, 4));
    const bool csm2 = bits(tex0, 55, 1) != 0;
    const std::uint32_t csa = static_cast<std::uint32_t>(bits(tex0, 56, 5));
    const bool eight = psm == PSMT8 || psm == PSMT8H;
    const unsigned count = eight ? 256 : 16;
    for (unsigned i = 0; i < count; ++i) {
        std::uint32_t x, y, bw;
        if (csm2) {
            x = static_cast<std::uint32_t>(bits(texclut, 6, 6)) * 16 + i;
            y = static_cast<std::uint32_t>(bits(texclut, 12, 10));
            bw = static_cast<std::uint32_t>(bits(texclut, 0, 6));
        } else if (eight) {
            // CSM1: índices com os bits 3 e 4 trocados num retângulo 16x16.
            const unsigned p = (i & ~0x18u) | ((i & 0x08u) << 1) | ((i & 0x10u) >> 1);
            x = p & 15;
            y = p >> 4;
            bw = 1;
        } else {
            x = i & 7;
            y = i >> 3;
            bw = 1;
        }
        const std::uint32_t c = vram_.readPixel(cpsm, cbp, bw, x, y);
        const unsigned e = csa * 16 + i;
        if (cpsm == PSMCT32) {
            dst.v[e & 255] = static_cast<std::uint16_t>(c);
            dst.v[(e & 255) + 256] = static_cast<std::uint16_t>(c >> 16);
        } else {
            dst.v[e & 511] = static_cast<std::uint16_t>(c);
        }
    }
}

void Gs::vertexKick(bool drawIt, std::uint32_t pc) {
    const std::uint64_t prim = regs_[PRIM];
    const unsigned type = static_cast<unsigned>(prim & 7);
    if (type == 7) unsupported("PRIM.PRIM = 7 (reservado)", pc);
    const bool useprim = regs_[PRMODECONT] & 1;
    const unsigned ctxt = static_cast<unsigned>(bits(useprim ? prim : regs_[PRMODE], 9, 1));
    Vertex v = current_;
    const std::uint64_t off = ctx_[ctxt].xyoffset;
    v.x = current_.x - static_cast<std::int32_t>(bits(off, 0, 16));
    v.y = current_.y - static_cast<std::int32_t>(bits(off, 32, 16));
    static constexpr unsigned kNeeded[7] = {1, 2, 2, 3, 3, 3, 2};
    queue_[queued_++] = v;
    if (queued_ < kNeeded[type]) return;
    if (drawIt) draw(pc);
    switch (type) {
        case 2:  // line strip
            queue_[0] = queue_[1];
            queued_ = 1;
            break;
        case 4:  // triangle strip
            queue_[0] = queue_[1];
            queue_[1] = queue_[2];
            queued_ = 2;
            break;
        case 5:  // triangle fan: mantém o primeiro
            queue_[1] = queue_[2];
            queued_ = 2;
            break;
        default:
            queued_ = 0;
            break;
    }
}

// ---------------------------------------------------------------------------
// Transferências
// ---------------------------------------------------------------------------

void Gs::startTransfer(std::uint32_t pc) {
    const unsigned dir = static_cast<unsigned>(regs_[TRXDIR] & 3);
    const std::uint64_t buf = regs_[BITBLTBUF], pos = regs_[TRXPOS], rr = regs_[TRXREG];
    switch (dir) {
        case 0: {  // HOST → LOCAL
            Transfer t;
            t.active = true;
            t.dbp = static_cast<std::uint32_t>(bits(buf, 32, 14));
            t.dbw = static_cast<std::uint32_t>(bits(buf, 48, 6));
            t.psm = static_cast<std::uint32_t>(bits(buf, 56, 6));
            t.x0 = static_cast<std::uint32_t>(bits(pos, 32, 11));
            t.y0 = static_cast<std::uint32_t>(bits(pos, 48, 11));
            t.w = static_cast<std::uint32_t>(bits(rr, 0, 12));
            t.h = static_cast<std::uint32_t>(bits(rr, 32, 12));
            if (!isValidPsm(t.psm)) unsupported("transferência HOST→LOCAL com " + psmName(t.psm), pc);
            if (t.w == 0 || t.h == 0) t.active = false;
            if (trace_ && t.active) trace_->vramHostStart(*this, t, pc);
            // O produtor escreve as palavras direto na VRAM, na hora (writeTransferData),
            // sem passar pelas faixas. Isso só vale se nenhum acesso ainda não concluído das
            // faixas (desenho que lê ou escreve, cópia, CLUT) toca o destino: se toca, uma
            // única espera (waitIdle) termina o que está pendente e a transferência segue
            // direta do mesmo jeito, em vez de ir em lotes pelas faixas, o que deixava o
            // destino como escrita pendente e fazia cada carga de CLUT seguinte esperar.
            //
            // Desenhos enfileirados depois que tocam o destino aberto veem só as palavras já
            // escritas se as faixas terminarem antes da palavra seguinte (noteHostHazard).
            // O modo em lotes não existe mais: o produtor sempre pode esperar, então não há
            // caso (nem transferência partida) que precise dele.
            closeHostTransfer();
            xferDirect_.active = false;  // a anterior, se ainda aberta, é substituída
            if (t.active) {
                const VramAccess dest = rectAccess(true, t.psm, t.dbp, t.dbw, t.x0, t.y0, t.w, t.h);
                if (outstandingConflicts(dest)) {
                    ++bandStats_.hostBarrier;
                    waitIdle(BandStats::WaitHost);
                } else {
                    ++bandStats_.hostDirect;
                }
                hostOpen_ = dest;
                hostWordsLeft_ = (std::uint64_t{t.w} * t.h * psmTransferBits(t.psm) + 63) / 64;
                hostOpenValid_ = true;
            }
            xferDirect_ = t;
            return;
        }
        case 1:
            localToHost(pc);
            return;
        case 2:
            localToLocal(pc);
            return;
        default:
            closeHostTransfer();
            xferDirect_.active = false;
            return;
    }
}

void Gs::writeTransferData(std::uint64_t data, std::uint32_t pc) {
    (void)pc;
    addWork(1);  // 64 bits: meio ciclo do barramento
    if (trace_) trace_->vramHostWord(*this);
    if (hostDrain_) {
        // Há um desenho (ou cópia, CLUT) nas faixas que toca o destino: ele vê só as
        // palavras anteriores a esta se as faixas terminarem antes de ela ser escrita.
        hostDrain_ = false;
        if (!worker_->idle()) {
            ++bandStats_.hostOrder;
            waitIdle(BandStats::WaitHost);
        }
    }
    hostWord(xferDirect_, data);  // na VRAM agora
    if (hostOpenValid_ && --hostWordsLeft_ == 0) closeHostTransfer();
}

void Gs::hostWord(Transfer& xfer, std::uint64_t data) {
    if (!xfer.active) {
        // Dados de preenchimento depois do fim de uma transferência são
        // descartados pelo hardware.
        return;
    }
    const unsigned bpp = psmTransferBits(xfer.psm);
    auto put = [&](std::uint32_t value) {
        if (!xfer.active) return;
        vram_.writePixel(xfer.psm, xfer.dbp, xfer.dbw, xfer.x0 + xfer.x, xfer.y0 + xfer.y, value);
        if (++xfer.x == xfer.w) {
            xfer.x = 0;
            if (++xfer.y == xfer.h) xfer.active = false;
        }
    };
    if (bpp == 24) {
        // Pixels de 3 bytes atravessam as palavras de 64 bits.
        for (unsigned i = 0; i < 8; ++i) {
            xfer.bits |= ((data >> (8 * i)) & 0xFF) << xfer.nbits;
            xfer.nbits += 8;
            if (xfer.nbits == 24) {
                put(static_cast<std::uint32_t>(xfer.bits));
                xfer.bits = 0;
                xfer.nbits = 0;
            }
        }
        return;
    }
    const std::uint64_t mask = bpp == 32 ? 0xFFFFFFFFull : (1ull << bpp) - 1;
    for (unsigned i = 0; i < 64 / bpp; ++i) put(static_cast<std::uint32_t>((data >> (i * bpp)) & mask));
}

void Gs::localToLocal(std::uint32_t pc) {
    const std::uint64_t buf = regs_[BITBLTBUF], pos = regs_[TRXPOS], rr = regs_[TRXREG];
    LocalCopy c{};
    c.sbp = static_cast<std::uint32_t>(bits(buf, 0, 14));
    c.sbw = static_cast<std::uint32_t>(bits(buf, 16, 6));
    c.spsm = static_cast<std::uint32_t>(bits(buf, 24, 6));
    c.dbp = static_cast<std::uint32_t>(bits(buf, 32, 14));
    c.dbw = static_cast<std::uint32_t>(bits(buf, 48, 6));
    c.dpsm = static_cast<std::uint32_t>(bits(buf, 56, 6));
    c.sx = static_cast<std::uint32_t>(bits(pos, 0, 11));
    c.sy = static_cast<std::uint32_t>(bits(pos, 16, 11));
    c.dx = static_cast<std::uint32_t>(bits(pos, 32, 11));
    c.dy = static_cast<std::uint32_t>(bits(pos, 48, 11));
    c.w = static_cast<std::uint32_t>(bits(rr, 0, 12));
    c.h = static_cast<std::uint32_t>(bits(rr, 32, 12));
    if (!isValidPsm(c.spsm) || !isValidPsm(c.dpsm)) unsupported("transferência LOCAL→LOCAL com PSM inválido", pc);
    if (psmTransferBits(c.spsm) != psmTransferBits(c.dpsm)) {
        unsupported("transferência LOCAL→LOCAL entre " + psmName(c.spsm) + " e " + psmName(c.dpsm) +
                        " (tamanhos de pixel diferentes)",
                    pc);
    }
    if (trace_) trace_->vramLocalCopy(*this, c, pc);
    const VramAccess touched[2] = {rectAccess(false, c.spsm, c.sbp, c.sbw, c.sx, c.sy, c.w, c.h),
                                   rectAccess(true, c.dpsm, c.dbp, c.dbw, c.dx, c.dy, c.w, c.h)};
    noteOutstanding(touched[0]);
    noteOutstanding(touched[1]);
    submit([this, c] { copyLocal(c); }, BandStats::SubmitLocal);
    noteHostHazard(touched, 2);
}

void Gs::copyLocal(const LocalCopy& c) {
    // Lê tudo antes de escrever: origem e destino podem se sobrepor.
    std::vector<std::uint32_t> tmp(std::size_t{c.w} * c.h);
    for (std::uint32_t y = 0; y < c.h; ++y)
        for (std::uint32_t x = 0; x < c.w; ++x)
            tmp[std::size_t{y} * c.w + x] = vram_.readPixel(c.spsm, c.sbp, c.sbw, c.sx + x, c.sy + y);
    for (std::uint32_t y = 0; y < c.h; ++y)
        for (std::uint32_t x = 0; x < c.w; ++x)
            vram_.writePixel(c.dpsm, c.dbp, c.dbw, c.dx + x, c.dy + y, tmp[std::size_t{y} * c.w + x]);
}

void Gs::localToHost(std::uint32_t pc) {
    // O EE lê a VRAM agora: tudo o que foi desenhado antes tem de estar pronto.
    waitIdle(BandStats::WaitDownload);
    const std::uint64_t buf = regs_[BITBLTBUF], pos = regs_[TRXPOS], rr = regs_[TRXREG];
    const auto sbp = static_cast<std::uint32_t>(bits(buf, 0, 14));
    const auto sbw = static_cast<std::uint32_t>(bits(buf, 16, 6));
    const auto spsm = static_cast<std::uint32_t>(bits(buf, 24, 6));
    const auto sx = static_cast<std::uint32_t>(bits(pos, 0, 11));
    const auto sy = static_cast<std::uint32_t>(bits(pos, 16, 11));
    const auto w = static_cast<std::uint32_t>(bits(rr, 0, 12));
    const auto h = static_cast<std::uint32_t>(bits(rr, 32, 12));
    if (!isValidPsm(spsm)) unsupported("transferência LOCAL→HOST com " + psmName(spsm), pc);
    // Mesmo empacotamento da HOST→LOCAL: o primeiro pixel nos bits baixos;
    // 24 bits atravessam os bytes.
    const unsigned bpp = psmTransferBits(spsm);
    download_.clear();
    downloadPos_ = 0;
    std::uint64_t acc = 0;
    unsigned nbits = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint64_t v = vram_.readPixel(spsm, sbp, sbw, sx + x, sy + y) & ((1ull << bpp) - 1);
            acc |= v << nbits;
            nbits += bpp;
            while (nbits >= 8) {
                download_.push_back(static_cast<std::uint8_t>(acc));
                acc >>= 8;
                nbits -= 8;
            }
        }
    }
    if (nbits) download_.push_back(static_cast<std::uint8_t>(acc));
    download_.resize((download_.size() + 15) & ~std::size_t{15}, 0);
    addWork(download_.size() / 8);
}

void Gs::readDownload(std::uint8_t* dst, std::size_t qwords) {
    const std::size_t want = qwords * 16;
    const std::size_t have = std::min(want, download_.size() - downloadPos_);
    std::memcpy(dst, download_.data() + downloadPos_, have);
    std::memset(dst + have, 0, want - have);
    downloadPos_ += have;
}

// ---------------------------------------------------------------------------
// Saída de vídeo
// ---------------------------------------------------------------------------

bool Gs::displayEnabled() const {
    return (priv_[0] & 3) != 0;
}

Frame Gs::display() const {
    // A saída de vídeo lê a VRAM: os desenhos feitos até aqui já estão prontos.
    syncConst(BandStats::WaitDisplay);
    struct Circuit {
        bool on = false;
        std::uint32_t fbp, fbw, psm, dbx, dby;
        std::int32_t dx, dy;
        std::uint32_t w, h;
    } c[2];
    const std::uint64_t pmode = priv_[0];
    // SMODE2 com INT e FFMD (entrelaçado, modo campo): o framebuffer tem meia
    // altura e cada linha dele aparece nos dois campos — a saída repete a
    // linha (o DISPLAY conta as linhas da tela inteira).
    const std::uint32_t lineDiv = (priv_[2] & 3) == 3 ? 2 : 1;
    std::uint32_t outW = 0, outH = 0;
    std::int32_t minX = 1 << 30, minY = 1 << 30;
    for (unsigned n = 0; n < 2; ++n) {
        if (!(pmode & (1u << n))) continue;
        const std::uint64_t fb = priv_[n == 0 ? 7 : 9];
        const std::uint64_t d = priv_[n == 0 ? 8 : 10];
        Circuit& k = c[n];
        k.on = true;
        k.fbp = static_cast<std::uint32_t>(bits(fb, 0, 9));
        k.fbw = static_cast<std::uint32_t>(bits(fb, 9, 6));
        k.psm = static_cast<std::uint32_t>(bits(fb, 15, 5));
        k.dbx = static_cast<std::uint32_t>(bits(fb, 32, 11));
        k.dby = static_cast<std::uint32_t>(bits(fb, 43, 11));
        const auto magh = static_cast<std::uint32_t>(bits(d, 23, 4)) + 1;
        const auto magv = static_cast<std::uint32_t>(bits(d, 27, 2)) + 1;
        k.w = (static_cast<std::uint32_t>(bits(d, 32, 12)) + 1) / magh;
        k.h = (static_cast<std::uint32_t>(bits(d, 44, 11)) + 1) / magv;
        k.dx = static_cast<std::int32_t>(bits(d, 0, 12) / magh);
        k.dy = static_cast<std::int32_t>(bits(d, 12, 11) / magv);
        minX = std::min(minX, k.dx);
        minY = std::min(minY, k.dy);
    }
    Frame f;
    if (!c[0].on && !c[1].on) return f;
    for (const auto& k : c) {
        if (!k.on) continue;
        outW = std::max(outW, static_cast<std::uint32_t>(k.dx - minX) + k.w);
        outH = std::max(outH, static_cast<std::uint32_t>(k.dy - minY) + k.h);
    }
    outW = std::min<std::uint32_t>(outW, 2048);
    outH = std::min<std::uint32_t>(outH, 2048);
    f.width = outW;
    f.height = outH;
    const std::uint64_t bg = priv_[14];
    const std::uint32_t bgColor = static_cast<std::uint32_t>(bg & 0xFFFFFF) | 0xFF000000u;
    f.pixels.assign(std::size_t{outW} * outH, bgColor);

    auto fetch = [&](const Circuit& k, std::int32_t ox, std::int32_t oy, bool& inside) -> std::uint32_t {
        const std::int32_t x = ox - (k.dx - minX), y = oy - (k.dy - minY);
        inside = x >= 0 && y >= 0 && static_cast<std::uint32_t>(x) < k.w && static_cast<std::uint32_t>(y) < k.h;
        if (!inside) return 0;
        const std::uint32_t px = k.dbx + static_cast<std::uint32_t>(x),
                            py = k.dby + static_cast<std::uint32_t>(y) / lineDiv;
        switch (k.psm) {
            case PSMCT32: return vram_.readPixel(PSMCT32, k.fbp * 32, k.fbw, px, py);
            case PSMCT24: return vram_.readPixel(PSMCT24, k.fbp * 32, k.fbw, px, py) | 0x80000000u;
            case PSMCT16: case PSMCT16S: {
                const std::uint32_t v = vram_.readPixel(k.psm, k.fbp * 32, k.fbw, px, py);
                return ((v & 0x1F) << 3) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 19) |
                       ((v & 0x8000) ? 0x80000000u : 0);
            }
            default: return 0;
        }
    };
    const bool both = c[0].on && c[1].on;
    const bool mmod = (pmode >> 5) & 1;
    const bool slbg = (pmode >> 7) & 1;
    const auto alp = static_cast<std::uint32_t>(bits(pmode, 8, 8));
    for (std::uint32_t y = 0; y < outH; ++y) {
        for (std::uint32_t x = 0; x < outW; ++x) {
            const auto ix = static_cast<std::int32_t>(x), iy = static_cast<std::int32_t>(y);
            bool in1 = false, in2 = false;
            const std::uint32_t p1 = c[0].on ? fetch(c[0], ix, iy, in1) : 0;
            const std::uint32_t p2 = c[1].on ? fetch(c[1], ix, iy, in2) : 0;
            std::uint32_t out = bgColor;
            if (!both) {
                if (in1) out = p1;
                else if (in2) out = p2;
            } else {
                const std::uint32_t back = (!slbg && in2) ? p2 : bgColor;
                if (in1) {
                    const std::uint32_t a = mmod ? alp : std::min<std::uint32_t>((p1 >> 24) * 2, 255);
                    std::uint32_t r = 0;
                    for (unsigned s = 0; s < 24; s += 8) {
                        const std::uint32_t c1 = (p1 >> s) & 0xFF, c2 = (back >> s) & 0xFF;
                        r |= ((c1 * a + c2 * (255 - a)) / 255) << s;
                    }
                    out = r;
                } else {
                    out = back;
                }
            }
            f.pixels[std::size_t{y} * outW + x] = (out & 0xFFFFFF) | 0xFF000000u;
        }
    }
    return f;
}

}  // namespace anyps2::rt::gs
