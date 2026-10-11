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
    if (const char* v = std::getenv("ANYPS2_VU1_JITTER"); v && *v) {
        jitterSeed_.store(std::strtoull(v, nullptr, 0), std::memory_order_relaxed);
    }
    if (requestedThread_) setThreaded(true);
}

Vu1Stream::~Vu1Stream() {
    shutdown();
}

void Vu1Stream::setDeferred(bool on) {
    if (on == deferred_) return;
    if (on) {
        if (threaded_) setThreaded(false);
        ensureRing();
    } else {
        drain();
        applyResponses();
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
    consume(item);
    applyResponses();
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
        // da limpeza, ou o IMR novo decidiria um evento antigo).
        fence();
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
    applyResponses();
    rethrowPending();
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
    fence();
    const std::uint32_t off = addr - 0x12000000u;
    if (off >= 0xF0) {
        switch (off & ~0xFu) {
            case 0x1000:
                HostProfile::count(&SyncStats::gsCsrReads);
                return eeState().csr(rt_.timing().now());
            case 0x1010:
                HostProfile::count(&SyncStats::gsOtherPrivReads);
                return eeState().imr();
            case 0x1080:
                HostProfile::count(&SyncStats::gsSiglblidReads);
                return eeState().siglblid();
            default: break;
        }
    }
    // Registradores de vídeo e BUSDIR (estado do consumidor) e o erro de endereço inexistente.
    return gs_.readPrivileged(addr, pc);
}

std::uint64_t Vu1Stream::csr(std::uint64_t now) {
    fence();
    return eeState().csr(now);
}

std::uint64_t Vu1Stream::imr() {
    fence();
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
    fence();
    return eeState().nextEventTime();
}

void Vu1Stream::processEvents(std::uint64_t now) {
    fence();
    eeState().processEvents(now);
}

// ---------------------------------------------------------------------------
// Consumidor
// ---------------------------------------------------------------------------

void Vu1Stream::drain() {
    if (threaded_) {
        const std::uint64_t target = nextSeq_ - 1;
        if (processedSeq_.load(std::memory_order_acquire) < target) {
            ++stats_.fenceWaits;
            publishPending();
            waitProcessed(target, HostProfile::VuWait);
        }
        return;
    }
    // Adiado: o consumidor é a própria thread do EE.
    while (processedSeq_.load(std::memory_order_relaxed) + 1 < nextSeq_) {
        Queued& q = slot(processedSeq_.load(std::memory_order_relaxed) + 1);
        consume(q.item, &q);
    }
}

void Vu1Stream::consume(const Item& item, Queued* q) {
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
    const std::lock_guard<std::mutex> lock(respMutex_);
    responses_.push_back(c);
    respCount_.store(responses_.size(), std::memory_order_release);
}

void Vu1Stream::applyResponses() {
    if (respCount_.load(std::memory_order_acquire) == 0) return;
    std::deque<gs::GsEvent> local;
    {
        const std::lock_guard<std::mutex> lock(respMutex_);
        local.swap(responses_);
        respCount_.store(0, std::memory_order_release);
    }
    for (const gs::GsEvent& e : local) gs_.eeState().apply(e);
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
        applyResponses();
        deferred_ = false;
        ensureRing();
        pubSeq_.store(processedSeq_.load(std::memory_order_acquire), std::memory_order_release);
        startThread();
    } else {
        drain();
        applyResponses();
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
