#include "anyps2/runtime/gs/gs_texcache.h"

#include <algorithm>
#include <thread>
#include <utility>

#include "anyps2/runtime/gs/vram.h"

namespace anyps2::rt::gs {

TexGeom texGeom(std::uint32_t psm) {
    switch (psm) {
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return TexGeom{4, 3, 2};  // 16×8
        case PSMT8: return TexGeom{4, 4, 1};                                              // 16×16
        case PSMT4: return TexGeom{5, 4, 1};                                              // 32×16
        // Os índices dos bytes altos de uma palavra de 32 bits ocupam o bloco 8×8 dela.
        case PSMT8H: case PSMT4HL: case PSMT4HH: return TexGeom{3, 3, 1};
        default: return TexGeom{3, 3, 4};  // CT32, CT24, Z32, Z24: 8×8
    }
}

TexLevel::TexLevel(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, unsigned w, unsigned h,
                   std::shared_ptr<TexLiveCounters> counters)
    : psm_(psm), bp_(bp), bw_(bw), w_(w), h_(h), counters_(std::move(counters)) {
    const TexGeom g = texGeom(psm);
    bwShift_ = g.bwShift;
    bhShift_ = g.bhShift;
    elemBytes_ = g.elemBytes;
    gridW_ = (w + (1u << bwShift_) - 1) >> bwShift_;
    const unsigned gridH = (h + (1u << bhShift_) - 1) >> bhShift_;
    const std::size_t blocks = std::size_t{gridW_} * gridH;
    bytes_ = (blocks << (bwShift_ + bhShift_)) * elemBytes_ + blocks;
    state_ = std::make_unique<std::atomic<std::uint8_t>[]>(blocks);
    for (std::size_t i = 0; i < blocks; ++i) state_[i].store(Empty, std::memory_order_relaxed);
    arena_.reset(new std::uint8_t[(blocks << (bwShift_ + bhShift_)) * elemBytes_]);
    if (counters_) {
        const std::uint64_t live = counters_->liveBytes.fetch_add(bytes_, std::memory_order_relaxed) + bytes_;
        std::uint64_t peak = counters_->peakLiveBytes.load(std::memory_order_relaxed);
        while (live > peak &&
               !counters_->peakLiveBytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
        }
    }
}

TexLevel::~TexLevel() {
    if (counters_) counters_->liveBytes.fetch_sub(bytes_, std::memory_order_relaxed);
}

bool TexLevel::ensure(const Vram& vram, unsigned bi) const {
    std::uint8_t expected = Empty;
    if (state_[bi].compare_exchange_strong(expected, Busy, std::memory_order_acq_rel, std::memory_order_acquire)) {
        fill(vram, bi);
        state_[bi].store(Ready, std::memory_order_release);
        return true;
    }
    // Outra faixa preenche este bloco (ou já o preencheu): espera. O bloco é pequeno (256
    // bytes de VRAM) e quem o preenche não depende de ninguém, então a espera é curta.
    while (state_[bi].load(std::memory_order_acquire) != Ready) std::this_thread::yield();
    return false;
}

// Preenche o bloco com readPixel, texel a texel (idêntico ao caminho sem cache). Só os
// texels dentro de w×h: os de fora da grade nunca são servidos (get devolve Outside),
// e a leitura deles poderia tocar páginas que o desenho não declarou como lidas.
void TexLevel::fill(const Vram& vram, unsigned bi) const {
    const unsigned bx = bi % gridW_, by = bi / gridW_;
    const unsigned bwPx = 1u << bwShift_, bhPx = 1u << bhShift_;
    const unsigned x0 = bx << bwShift_, y0 = by << bhShift_;
    const unsigned x1 = std::min(x0 + bwPx, w_), y1 = std::min(y0 + bhPx, h_);
    std::uint8_t* base = arena_.get() + (static_cast<std::size_t>(bi) << (bwShift_ + bhShift_)) * elemBytes_;
    for (unsigned y = y0; y < y1; ++y) {
        std::uint8_t* row = base + (static_cast<std::size_t>(y - y0) << bwShift_) * elemBytes_;
        for (unsigned x = x0; x < x1; ++x) {
            const std::uint32_t v = vram.readPixel(psm_, bp_, bw_, x, y);
            std::uint8_t* p = row + static_cast<std::size_t>(x - x0) * elemBytes_;
            switch (elemBytes_) {
                case 1:
                    *p = static_cast<std::uint8_t>(v);
                    break;
                case 2: {
                    const auto s = static_cast<std::uint16_t>(v);
                    std::memcpy(p, &s, 2);
                    break;
                }
                default:
                    std::memcpy(p, &v, 4);
                    break;
            }
        }
    }
}

TexCache::Entry* TexCache::find(const TexKey& k) {
    const auto it = map_.find(k);
    if (it == map_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second.lru);
    return &it->second;
}

TexCache::Entry& TexCache::create(const TexKey& k, const PageSet& pages, std::vector<std::uint32_t> gens,
                                  std::uint64_t epoch) {
    auto it = map_.find(k);
    if (it == map_.end()) {
        lru_.push_front(k);
        it = map_.emplace(k, Entry{}).first;
        it->second.lru = lru_.begin();
    } else {
        stats_.bytes -= it->second.level->bytes();
        lru_.splice(lru_.begin(), lru_, it->second.lru);
    }
    Entry& e = it->second;
    // A versão anterior (se houver) só é solta daqui: os desenhos em voo que a fixaram
    // continuam com ela.
    e.level = std::make_shared<TexLevel>(k.psm, k.bp, k.bw, k.w, k.h, counters_);
    e.pages = pages;
    e.gens = std::move(gens);
    e.validEpoch = epoch;
    ++stats_.versions;
    stats_.bytes += e.level->bytes();
    stats_.peakBytes = std::max(stats_.peakBytes, stats_.bytes);
    evictToBudget();
    stats_.entries = map_.size();
    return e;
}

void TexCache::evictToBudget() {
    // Nunca despeja a mais recente (a que acabou de ser criada ou pedida).
    while (stats_.bytes > budget_ && lru_.size() > 1) {
        const auto it = map_.find(lru_.back());
        stats_.bytes -= it->second.level->bytes();
        map_.erase(it);
        lru_.pop_back();
        ++stats_.evictions;
    }
}

}  // namespace anyps2::rt::gs
