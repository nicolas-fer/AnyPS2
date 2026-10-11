#include "anyps2/runtime/vu1_stream.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/host_profile.h"
#include "anyps2/runtime/host_thread.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/timing.h"
#include "anyps2/runtime/vif.h"
#include "anyps2/runtime/vu/vu.h"

namespace anyps2::rt {

namespace {

// Iterações de cessão de CPU antes de o EE (ou o consumidor) dormir na condvar: esperas curtas
// (o consumidor quase alcançando o EE) não pagam a troca de contexto.
constexpr int kSpinYields = 64;
constexpr int kIdleSpins = 256;

// Depois de usado, um slot que guardou um bloco grande (um DMA de PATH3 de até 1 MB) devolve a
// memória; os pequenos mantêm a capacidade, para o anel não alocar em regime.
constexpr std::size_t kKeepBytes = 64 * 1024;
constexpr std::size_t kKeepWrites = 4096;
constexpr std::size_t kKeepWords = 16384;

constexpr std::size_t kConsumerStack = 16u * 1024 * 1024;

// A thread consumidora nunca espera a si mesma: um fence chamado dela (um caminho que, por engano,
// leia estado do consumidor) é um no-op em vez de um deadlock.
thread_local bool tlOnConsumer = false;

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

}  // namespace

Vu1Stream::Vu1Stream(Runtime& rt, VuMemory& vu, Gif& gif, gs::Gs& gs)
    : rt_(rt), gif_(gif), gs_(gs), vif1_(&rt, 1, vu, &gif) {
    // ANYPS2_VU1_THREAD=0 (padrão): o consumidor roda inline em cada push. =1: VU1, GIF e GS
    // produtor numa thread própria (ver vu1_stream.h).
    if (const char* v = std::getenv("ANYPS2_VU1_THREAD"); v && *v && std::strcmp(v, "0") != 0) {
        requestedThread_ = 1;
    }
    if (const char* v = std::getenv("ANYPS2_VU1_RING"); v && *v) {
        const auto n = static_cast<std::size_t>(std::strtoull(v, nullptr, 0));
        if (n >= 2) setRingSlots(n);
    }
    forceFence_ = envFlag("ANYPS2_VU1_FENCE_ALL");
    // Horizonte W (ciclos do EE): só vale no relógio virtual (no real não há horizonte).
    virtual_ = rt.options().virtualClock;
    // Sem a thread o horizonte não traz ganho e só atrasaria os eventos do GS: o padrão é 0
    // (comportamento de sempre); com a thread, 1/4 de quadro. ANYPS2_VU1_HORIZON escolhe
    // explicitamente (e então THREAD=0 e 1 com o mesmo W dão o mesmo resultado).
    horizon_ = requestedThread_ ? defaultHorizon() : 0;
    if (const char* v = std::getenv("ANYPS2_VU1_HORIZON"); v && *v) {
        horizon_ = static_cast<std::uint64_t>(std::strtoull(v, nullptr, 0));
    }
    w_ = virtual_ ? horizon_ : 0;
    if (const char* v = std::getenv("ANYPS2_VU1_JITTER"); v && *v) {
        jitterSeed_.store(std::strtoull(v, nullptr, 0), std::memory_order_relaxed);
    }
    if (requestedThread_) setThreaded(true);
}

Vu1Stream::~Vu1Stream() {
    shutdown();
}

std::uint64_t Vu1Stream::defaultHorizon() {
    return Timing::kCyclesPerField / 4;
}

void Vu1Stream::setHorizon(std::uint64_t cycles) {
    fence();
    horizon_ = cycles;
    w_ = virtual_ ? cycles : 0;
    horizonQ_.clear();
}

std::size_t Vu1Stream::pendingResponses() const {
    return due_.size() + respCount_.load(std::memory_order_acquire);
}

void Vu1Stream::setDeferred(bool on) {
    if (on == deferred_) return;
    if (on) {
        if (threaded_) setThreaded(false);
        ensureRing();
    } else {
        drain();
        applyResponses(rt_.timing().now());
    }
    deferred_ = on;
}

void Vu1Stream::setRingSlots(std::size_t slots) {
    std::size_t n = 2;
    while (n < slots) n <<= 1;
    // Só com a fila vazia (ninguém tem slot em uso).
    if (!ring_.empty() && queued() != 0) return;
    ringSlots_ = n;
    ring_.clear();
    ringMask_ = 0;
}

void Vu1Stream::ensureRing() {
    if (!ring_.empty()) return;
    ring_.resize(ringSlots_);
    ringMask_ = ringSlots_ - 1;
}

// ---------------------------------------------------------------------------
// Lado do EE
// ---------------------------------------------------------------------------

void Vu1Stream::push(Item& item) {
    // Espelho do PATH3 mascarado: só os itens abaixo o alteram, e o decodificador do VIF1 já os
    // emite na ordem do EE, então o EE o sabe sem esperar o consumidor (o DMA do GIF o consulta
    // a cada início).
    switch (item.kind) {
        case Item::Kind::Vif:
            if (item.vif.kind == VifItem::Kind::Path3Mask) path3MaskedEe_ = item.vif.masked;
            else if (item.vif.kind == VifItem::Kind::Reset) path3MaskedEe_ = false;
            break;
        case Item::Kind::GifReset: path3MaskedEe_ = false; break;
        case Item::Kind::GifWrite:
            if (item.addr == 0x10003000u && (item.value & 1)) path3MaskedEe_ = false;  // GIF_CTRL.RST
            break;
        default: break;
    }
    if (deferred_ || threaded_) {
        pushQueued(item);
        return;
    }
    rethrowPending();  // erro de um item anterior: relançado no enfileiramento
    item.seq = nextSeq_++;
    noteHorizon(item);
    consume(item);
    // As respostas com at <= agora valem já (W = 0: todas, como antes); as outras esperam o
    // safepoint ou a leitura que chegar ao horizonte delas, igual na thread.
    if (respCount_.load(std::memory_order_acquire) != 0 || !due_.empty()) applyResponses(rt_.timing().now());
    rethrowPending();
}

void Vu1Stream::pushQueued(Item& item) {
    if (failed_.load(std::memory_order_acquire)) {
        // Adiado: o item que falhou já foi consumido por drain. Thread: espera o consumidor
        // esvaziar o anel (ele pula o resto), aplica as respostas e relança.
        if (threaded_) fenceSlow();
        else rethrowPending();
    }
    ensureRing();
    const std::uint64_t cap = ring_.size();
    if (nextSeq_ - processedSeq_.load(std::memory_order_acquire) > cap) {
        // Anel cheio: o slot do próximo seq ainda é do item nextSeq_ - cap. Só os itens já
        // preenchidos (até nextSeq_ - 1) são publicados/consumidos enquanto se espera.
        if (threaded_) {
            ++stats_.ringFull;
            publishPending();
            waitProcessed(nextSeq_ - cap, HostProfile::RingFull);
        } else {
            drain();
        }
    }
    item.seq = nextSeq_++;
    noteHorizon(item);
    fillSlot(slot(item.seq), item);
    if (!threaded_) return;
    // Publica por lote: os itens que disparam trabalho vão de imediato; UNPACK e MPG (muitos e
    // baratos) esperam o próximo disparo ou kPublishBatch itens acumulados.
    const bool lazy = item.kind == Item::Kind::Vif &&
                      (item.vif.kind == VifItem::Kind::Unpack || item.vif.kind == VifItem::Kind::Mpg);
    if (!lazy || item.seq - pubSeq_.load(std::memory_order_relaxed) >= kPublishBatch) publishPending();
    if (forceFence_) fence();
}

void Vu1Stream::pushVif(const VifItem& v) {
    if (threaded_ && (v.kind == VifItem::Kind::Mscal || v.kind == VifItem::Kind::Mscnt)) {
        HostProfile::noteMscalEnqueued();  // o lote do VIF1 (medição) é do EE
    }
    Item it(Item::Kind::Vif);
    it.vif = v;
    it.eePc = v.pc;
    it.stamp = rt_.timing().now();
    push(it);
}

void Vu1Stream::pushPath3(const std::uint8_t* data, std::size_t qwords, std::uint32_t pc) {
    Item it(Item::Kind::Path3);
    it.data = data;
    it.qwords = qwords;
    it.eePc = pc;
    it.stamp = rt_.timing().now();
    push(it);
}

void Vu1Stream::pushGifWrite(std::uint32_t addr, std::uint32_t value, std::uint32_t pc) {
    Item it(Item::Kind::GifWrite);
    it.addr = addr;
    it.value = value;
    it.eePc = pc;
    it.stamp = rt_.timing().now();
    push(it);
}

void Vu1Stream::pushGifReset() {
    Item it(Item::Kind::GifReset);
    it.stamp = rt_.timing().now();
    push(it);
}

void Vu1Stream::writeGsPrivileged(std::uint32_t addr, std::uint64_t value, std::uint32_t pc) {
    const std::uint32_t off = addr - 0x12000000u;
    if (off >= 0xF0) {
        // CSR (sem RESET), IMR e SIGLBLID são estado do EE: vão direto ao espelho, no ponto do EE,
        // depois das respostas dos itens anteriores (senão um SIGNAL antigo reapareceria depois
        // da limpeza, ou o IMR novo decidiria um evento antigo): o espelho vale como em `agora`.
        syncMirror();
        switch (off & ~0xFu) {
            case 0x1000:
                if (!(value & gs::kCsrResetMask)) {
                    eeState().writeCsr(value, rt_.timing().now());
                    return;
                }
                break;
            case 0x1010:
                eeState().setImr(value);
                return;
            case 0x1080:
                eeState().setSigLblId(value);
                return;
            default: break;
        }
    }
    Item it(Item::Kind::GsPriv);
    it.addr = addr;
    it.value = value;
    it.eePc = pc;
    it.stamp = rt_.timing().now();
    push(it);
}

void Vu1Stream::setCrt(std::uint32_t interlace, std::uint32_t mode, std::uint32_t field) {
    // Os mesmos valores que Gs::setCrt grava em SMODE1 (0x1200_0010) e SMODE2 (0x1200_0020).
    writeGsPrivileged(0x12000010u, std::uint64_t{mode} << 8, 0);
    writeGsPrivileged(0x12000020u, (interlace & 1u) | ((field & 1u) << 1), 0);
}

void Vu1Stream::pushVblank(std::uint64_t stamp) {
    // VSINT e a inversão do campo são do EE (o VBlank é do Timing): entram no espelho agora, sem o
    // horizonte W (senão um programa que espera o VSINT por polling do CSR perderia W por quadro).
    // Antes, as respostas devidas dos itens anteriores, que no baseline já valiam.
    syncMirror();
    gs::GsEvent v;
    v.kind = gs::GsEvent::Kind::Vblank;
    eeState().apply(v);
    Item it(Item::Kind::Vblank);
    it.stamp = stamp;
    push(it);
}

void Vu1Stream::pushVu1Start(std::uint32_t startBytes, std::uint32_t eePc) {
    Item it(Item::Kind::Vu1Start);
    it.start = startBytes;
    it.eePc = eePc;
    it.vblanks = rt_.vblanks();  // o rastreador do VU conta por VBlank: vem no item
    it.stamp = rt_.timing().now();
    push(it);
}

void Vu1Stream::pushVu1Reset() {
    Item it(Item::Kind::Vu1Reset);
    it.stamp = rt_.timing().now();
    push(it);
}

void Vu1Stream::pushFence() {
    Item it(Item::Kind::Fence);
    it.stamp = rt_.timing().now();
    push(it);
}

// Copia para o slot os dados que o chamador só empresta durante o push.
void Vu1Stream::fillSlot(Queued& q, const Item& item) {
    q.item = item;
    switch (item.kind) {
        case Item::Kind::Vif:
            switch (item.vif.kind) {
                case VifItem::Kind::Unpack:
                    q.writes.assign(item.vif.writes, item.vif.writes + item.vif.count);
                    q.item.vif.writes = q.writes.data();
                    break;
                case VifItem::Kind::Mpg:
                    q.words.assign(item.vif.words, item.vif.words + item.vif.count);
                    q.item.vif.words = q.words.data();
                    break;
                case VifItem::Kind::Direct:
                    q.bytes.assign(item.vif.data, item.vif.data + 16);
                    q.item.vif.data = q.bytes.data();
                    break;
                default: break;
            }
            break;
        case Item::Kind::Path3:
            q.bytes.assign(item.data, item.data + item.qwords * 16);
            q.item.data = q.bytes.data();
            break;
        default: break;
    }
}

void Vu1Stream::trimSlot(Queued& q) {
    if (q.bytes.capacity() > kKeepBytes) std::vector<std::uint8_t>().swap(q.bytes);
    if (q.writes.capacity() > kKeepWrites) std::vector<VifQwWrite>().swap(q.writes);
    if (q.words.capacity() > kKeepWords) std::vector<std::uint32_t>().swap(q.words);
}

void Vu1Stream::fenceSlow() {
    if (tlOnConsumer) return;
    drain();
    applyResponses(rt_.timing().now());
    rethrowPending();
}

bool Vu1Stream::responseDue() const {
    return !due_.empty() && due_.front().at <= rt_.timing().now();
}

bool Vu1Stream::needsMirrorSync(bool wait) const {
    if (!virtual_) {
        // Relógio real: sem horizonte. Safepoint (!wait) só aplica o que o consumidor já publicou.
        return wait ? needsFence() : (respCount_.load(std::memory_order_acquire) != 0 ||
                                      failed_.load(std::memory_order_acquire) || responseDue());
    }
    if (w_ == 0) return needsFence();  // W = 0: todo item já venceu, o fence completo
    const std::uint64_t now = rt_.timing().now();
    return (!horizonQ_.empty() && horizonQ_.front().stamp + w_ <= now) ||
           respCount_.load(std::memory_order_acquire) != 0 || (!due_.empty() && due_.front().at <= now) ||
           failed_.load(std::memory_order_acquire);
}

void Vu1Stream::syncSlow(bool wait) {
    if (tlOnConsumer) return;
    const std::uint64_t now = rt_.timing().now();
    if (!virtual_) {
        if (wait) drain();
    } else if (w_ == 0) {
        drain();
    } else {
        waitHorizon(now);
    }
    applyResponses(now);
    rethrowPending();
}

void Vu1Stream::noteHorizon(const Item& item) {
    if (w_ == 0) return;
    // Os carimbos não decrescem (Timing::now()); se um item vier com carimbo menor, vale o maior,
    // o mesmo que o consumidor usa para o `at` das respostas (consStamp_).
    if (!horizonQ_.empty() && horizonQ_.back().stamp >= item.stamp) {
        horizonQ_.back().seq = item.seq;
        return;
    }
    horizonQ_.push_back({item.stamp, item.seq});
}

void Vu1Stream::waitHorizon(std::uint64_t now) {
    std::uint64_t target = 0;
    while (!horizonQ_.empty() && horizonQ_.front().stamp + w_ <= now) {
        target = horizonQ_.front().seq;
        horizonQ_.pop_front();
    }
    if (target != 0) drainTo(target, HostProfile::HorizonWait);
}

void Vu1Stream::rethrowPending() {
    if (!failed_.load(std::memory_order_acquire)) return;
    if (threaded_) {
        // Recomeço determinista: o consumidor termina de pular o que já estava no anel antes de
        // o EE limpar o estado de erro.
        publishPending();
        waitProcessed(nextSeq_ - 1, HostProfile::VuWait);
    }
    failed_.store(false, std::memory_order_release);
    std::exception_ptr e = std::move(error_);
    error_ = nullptr;
    std::rethrow_exception(e);
}

std::uint64_t Vu1Stream::readGsPrivileged(std::uint32_t addr, std::uint32_t pc) {
    const std::uint32_t off = addr - 0x12000000u;
    if (off >= 0xF0) {
        // CSR, IMR e SIGLBLID são do espelho do EE: basta o horizonte.
        switch (off & ~0xFu) {
            case 0x1000:
                syncMirror();
                HostProfile::count(&SyncStats::gsCsrReads);
                return eeState().csr(rt_.timing().now());
            case 0x1010:
                syncMirror();
                HostProfile::count(&SyncStats::gsOtherPrivReads);
                return eeState().imr();
            case 0x1080:
                syncMirror();
                HostProfile::count(&SyncStats::gsSiglblidReads);
                return eeState().siglblid();
            default: break;
        }
    }
    // Registradores de vídeo e BUSDIR (estado do consumidor) e o erro de endereço inexistente.
    fence();
    return gs_.readPrivileged(addr, pc);
}

std::uint64_t Vu1Stream::csr(std::uint64_t now) {
    syncMirror();
    return eeState().csr(now);
}

std::uint64_t Vu1Stream::imr() {
    syncMirror();
    return eeState().imr();
}

std::uint32_t Vu1Stream::readGif(std::uint32_t addr, std::uint32_t pc) {
    fence();
    return gif_.readRegister(addr, pc);
}

bool Vu1Stream::path3Masked() {
    return path3MaskedEe_;
}

gs::GsEeState& Vu1Stream::eeState() {
    return gs_.eeState();
}

std::uint64_t Vu1Stream::nextEventTime() {
    poll();
    std::uint64_t next = eeState().nextEventTime();
    // O orçamento do safepoint não cruza o horizonte do item mais antigo ainda não vencido. Depois
    // do poll, carimbo + W > agora; a lista é do EE e igual em todos os modos, então os
    // safepoints caem nos mesmos ciclos com ou sem a thread.
    if (w_ != 0 && !horizonQ_.empty()) next = std::min(next, horizonQ_.front().stamp + w_);
    return next;
}

void Vu1Stream::processEvents(std::uint64_t now) {
    poll();
    eeState().processEvents(now);
}

// ---------------------------------------------------------------------------
// Consumidor
// ---------------------------------------------------------------------------

void Vu1Stream::drain() {
    drainTo(nextSeq_ - 1, HostProfile::VuWait);
}

void Vu1Stream::drainTo(std::uint64_t target, unsigned part) {
    if (threaded_) {
        if (processedSeq_.load(std::memory_order_acquire) < target) {
            if (part == HostProfile::HorizonWait) ++stats_.horizonWaits;
            else ++stats_.fenceWaits;
            publishPending();
            waitProcessed(target, part);
        }
        return;
    }
    // Adiado: o consumidor é a própria thread do EE. Inline: nada a esperar (processou no push).
    while (processedSeq_.load(std::memory_order_relaxed) < target) {
        Queued& q = slot(processedSeq_.load(std::memory_order_relaxed) + 1);
        consume(q.item, &q);
    }
}

void Vu1Stream::consume(const Item& item, Queued* q) {
    // Carimbo das respostas deste item (máximo até ele, como na lista do horizonte do EE).
    consStamp_ = std::max(consStamp_, item.stamp);
    // Depois de um erro o EE o recebe e para: o resto não roda (até o relançamento).
    if (!failed_.load(std::memory_order_acquire)) {
        gs_.setItemClock(item.stamp);
        try {
            apply(item);
        } catch (...) {
            // Guarda a exceção original (mesma classe, mensagem e pc); o EE a relança no fence ou
            // no próximo enfileiramento (inline: logo depois do item, no ponto onde ela já era
            // lançada).
            error_ = std::current_exception();
            errorSeq_ = item.seq;
            failed_.store(true, std::memory_order_release);
        }
    }
    // O slot volta ao EE com o seq publicado: apara antes.
    if (q) trimSlot(*q);
    processedSeq_.store(item.seq, std::memory_order_seq_cst);
    if (threaded_) notifyEe(item.seq);
}

void Vu1Stream::apply(const Item& item) {
    switch (item.kind) {
        case Item::Kind::Vif:
            vif1_.apply(item.vif);
            break;
        case Item::Kind::Path3:
            gif_.transfer(3, item.data, item.qwords, item.eePc);
            break;
        case Item::Kind::GifWrite:
            gif_.writeRegister(item.addr, static_cast<std::uint32_t>(item.value), item.eePc);
            break;
        case Item::Kind::GifReset:
            gif_.reset();
            break;
        case Item::Kind::GsPriv:
            gs_.writePrivileged(item.addr, item.value, item.eePc);
            break;
        case Item::Kind::Vblank:
            gs_.vblankStart();
            break;
        case Item::Kind::Vu1Start:
            rt_.vu1().startAt(item.start, item.eePc, item.vblanks);
            break;
        case Item::Kind::Vu1Reset:
            rt_.vu1().reset();
            break;
        case Item::Kind::Fence:
            break;
    }
}

void Vu1Stream::publish(const gs::GsEvent& e) {
    gs::GsEvent c = e;
    c.seq = respSeq_++;
    // A resposta vale para o EE em carimbo + W (o EE a aplica quando o relógio chega lá); o FINISH
    // dispara em max(fim do trabalho do GS, carimbo + W). Só o seq, o carimbo e W entram na conta.
    c.at = consStamp_ + w_;
    c.due = c.kind == gs::GsEvent::Kind::FinishDue ? std::max(c.due, c.at) : c.at;
    const std::lock_guard<std::mutex> lock(respMutex_);
    responses_.push_back(c);
    respCount_.store(responses_.size(), std::memory_order_release);
}

void Vu1Stream::applyResponses(std::uint64_t now) {
    if (respCount_.load(std::memory_order_acquire) != 0) {
        const std::lock_guard<std::mutex> lock(respMutex_);
        for (const gs::GsEvent& e : responses_) due_.push_back(e);
        responses_.clear();
        respCount_.store(0, std::memory_order_release);
    }
    // Na ordem da fila, só as devidas: as de itens posteriores (at > agora) ficam para depois,
    // mesmo que o consumidor já as tenha publicado.
    while (!due_.empty() && due_.front().at <= now) {
        const gs::GsEvent e = due_.front();
        due_.pop_front();
        gs_.eeState().apply(e);
    }
}

// ---------------------------------------------------------------------------
// Thread
// ---------------------------------------------------------------------------

void Vu1Stream::setThreaded(bool on) {
    if (on == threaded_) return;
    if (on) {
        if (rt_.options().traceGs) {
            // ANYPS2_TRACE=gs imprime do EE (DMA, VIF) e do consumidor (GIF, VU): as linhas se
            // misturariam em ordem não determinista. O diagnóstico vale só no inline.
            std::fprintf(stderr,
                         "anyps2: aviso: ANYPS2_VU1_THREAD=1 não vale com ANYPS2_TRACE=gs (as linhas do EE e do "
                         "consumidor se misturariam); usando o modo inline\n");
            return;
        }
        drain();
        applyResponses(rt_.timing().now());
        deferred_ = false;
        ensureRing();
        pubSeq_.store(processedSeq_.load(std::memory_order_acquire), std::memory_order_release);
        startThread();
    } else {
        drain();
        applyResponses(rt_.timing().now());
        stopThread();
    }
}

void Vu1Stream::startThread() {
    stop_.store(false, std::memory_order_relaxed);
    threaded_ = true;
    HostProfile::setThreaded(true);
    thread_ = std::make_unique<HostThread>([this] { consumerMain(); }, kConsumerStack);
}

void Vu1Stream::stopThread() {
    if (!thread_) {
        threaded_ = false;
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(mtx_);
        stop_.store(true, std::memory_order_seq_cst);
    }
    consumerCv_.notify_all();
    thread_->join();
    thread_.reset();
    threaded_ = false;
}

void Vu1Stream::shutdown() {
    if (!thread_) return;
    stopThread();
    // O que sobrou no anel foi descartado: conta como consumido, para nenhum fence tentar
    // reexecutá-lo (o stream volta ao inline).
    processedSeq_.store(nextSeq_ - 1, std::memory_order_seq_cst);
    pubSeq_.store(nextSeq_ - 1, std::memory_order_relaxed);
}

void Vu1Stream::publishPending() {
    const std::uint64_t last = nextSeq_ - 1;
    if (pubSeq_.load(std::memory_order_relaxed) == last) return;
    pubSeq_.store(last, std::memory_order_seq_cst);
    ++stats_.publishes;
    if (consumerSleeping_.load(std::memory_order_seq_cst)) {
        const std::lock_guard<std::mutex> lock(mtx_);
        consumerCv_.notify_one();
    }
}

void Vu1Stream::waitProcessed(std::uint64_t target, unsigned part) {
    if (processedSeq_.load(std::memory_order_acquire) >= target) return;
    const HostProfile::Scope prof(static_cast<HostProfile::Part>(part));
    for (int i = 0; i < kSpinYields; ++i) {
        if (processedSeq_.load(std::memory_order_acquire) >= target) return;
        std::this_thread::yield();
    }
    std::unique_lock<std::mutex> lock(mtx_);
    eeTarget_.store(target, std::memory_order_seq_cst);
    while (processedSeq_.load(std::memory_order_seq_cst) < target) eeCv_.wait(lock);
    eeTarget_.store(0, std::memory_order_relaxed);
}

void Vu1Stream::notifyEe(std::uint64_t seq) {
    const std::uint64_t t = eeTarget_.load(std::memory_order_seq_cst);
    if (t != 0 && seq >= t) {
        const std::lock_guard<std::mutex> lock(mtx_);
        eeCv_.notify_all();
    }
}

void Vu1Stream::idleWait(std::uint64_t next) {
    for (int i = 0; i < kIdleSpins; ++i) {
        if (pubSeq_.load(std::memory_order_acquire) >= next || stop_.load(std::memory_order_acquire)) return;
        std::this_thread::yield();
    }
    std::unique_lock<std::mutex> lock(mtx_);
    consumerSleeping_.store(true, std::memory_order_seq_cst);
    while (pubSeq_.load(std::memory_order_seq_cst) < next && !stop_.load(std::memory_order_seq_cst)) {
        consumerCv_.wait(lock);
    }
    consumerSleeping_.store(false, std::memory_order_relaxed);
}

void Vu1Stream::consumerMain() {
    tlOnConsumer = true;
    HostProfile::bindConsumerThread();
    std::uint64_t next = processedSeq_.load(std::memory_order_acquire) + 1;
    std::uint64_t lastSeed = 0, rng = 0;
    for (;;) {
        while (pubSeq_.load(std::memory_order_acquire) < next) {
            if (stop_.load(std::memory_order_acquire)) return;
            idleWait(next);
        }
        if (stop_.load(std::memory_order_acquire)) return;
        // Jitter de teste: cede a CPU ou dorme um tempo sorteado a cada item.
        if (const std::uint64_t seed = jitterSeed_.load(std::memory_order_relaxed)) {
            if (seed != lastSeed) {
                lastSeed = seed;
                rng = seed * 0x9E3779B97F4A7C15ull | 1;
            }
            rng ^= rng << 13;
            rng ^= rng >> 7;
            rng ^= rng << 17;
            const std::uint64_t r = rng >> 8;
            switch (rng & 15) {
                case 9: case 10: case 11: case 12: case 13: std::this_thread::yield(); break;
                case 14: std::this_thread::sleep_for(std::chrono::microseconds(r % 400)); break;  // >= 1 ms no Windows
                case 15:
                    for (std::uint64_t k = r % 40; k > 0; --k) std::this_thread::yield();
                    break;
                default: break;
            }
        }
        Queued& q = slot(next);
        {
            const HostProfile::Scope prof(HostProfile::Item);
            consume(q.item, &q);
        }
        ++next;
    }
}

}  // namespace anyps2::rt
