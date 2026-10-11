#include "anyps2/runtime/vu1_stream.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/host_profile.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/timing.h"
#include "anyps2/runtime/vif.h"
#include "anyps2/runtime/vu/vu.h"

namespace anyps2::rt {

Vu1Stream::Vu1Stream(Runtime& rt, VuMemory& vu, Gif& gif, gs::Gs& gs)
    : rt_(rt), gif_(gif), gs_(gs), vif1_(&rt, 1, vu, &gif) {
    // ANYPS2_VU1_THREAD=0 (padrão): o consumidor roda inline em cada push. =1 (VU1/GIF/GS em outra
    // thread) ainda não existe: avisa e cai no inline, que é o único modo desta fatia.
    if (const char* v = std::getenv("ANYPS2_VU1_THREAD"); v && *v && std::strcmp(v, "0") != 0) {
        requestedThread_ = 1;
        std::fprintf(stderr,
                     "anyps2: aviso: ANYPS2_VU1_THREAD=%s ainda não existe (o VU1 em outra thread é uma fatia "
                     "seguinte do plano); usando o modo inline\n",
                     v);
    }
}

void Vu1Stream::setDeferred(bool on) {
    if (deferred_ && !on) {
        drain();
        applyResponses();
    }
    deferred_ = on;
}

// ---------------------------------------------------------------------------
// Lado do EE
// ---------------------------------------------------------------------------

void Vu1Stream::push(Item& item) {
    rethrowPending();  // erro de um item anterior (modo adiado): relançado no enfileiramento
    item.seq = nextSeq_++;
    if (deferred_) {
        enqueueCopy(item);
        return;
    }
    consume(item);
    applyResponses();
    rethrowPending();
}

void Vu1Stream::pushVif(const VifItem& v) {
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

void Vu1Stream::enqueueCopy(const Item& item) {
    queue_.emplace_back(item.kind);
    Queued& q = queue_.back();
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

void Vu1Stream::fenceSlow() {
    drain();
    applyResponses();
    rethrowPending();
}

void Vu1Stream::rethrowPending() {
    if (!failed_) return;
    failed_ = false;
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
    fence();
    return gif_.path3Masked();
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
    while (!queue_.empty()) {
        consume(queue_.front().item);
        queue_.pop_front();
    }
}

void Vu1Stream::consume(const Item& item) {
    processedSeq_ = item.seq;
    if (failed_) return;  // depois de um erro o EE o recebe e para: o resto não roda
    gs_.setItemClock(item.stamp);
    try {
        apply(item);
    } catch (const GuestError&) {
        // Guarda a exceção original (mesma classe, mensagem e pc); o EE a relança no fence ou no
        // próximo enfileiramento (inline: logo depois do item, no ponto onde ela já era lançada).
        failed_ = true;
        error_ = std::current_exception();
        errorSeq_ = item.seq;
    }
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
    responses_.push_back(c);
}

void Vu1Stream::applyResponses() {
    while (!responses_.empty()) {
        const gs::GsEvent e = responses_.front();
        responses_.pop_front();
        gs_.eeState().apply(e);
    }
}

}  // namespace anyps2::rt
