// Busca de texel do GS (fetchTexel = texelRaw + expandTexel, paleta memorizada).
// O teste guarda uma cópia congelada do fetchTexel antigo (um bloco só, com o wrap,
// o readPixel cru e o switch de formato/CLUT/TEXA) e exige igualdade bit a bit com a
// busca atual, para todos os formatos de textura, tamanhos, modos de wrap, CLUT e TEXA.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <vector>

#include "anyps2/runtime/gs/gs.h"
#include "minitest.h"

#ifdef _WIN32
#include <stdlib.h>
#endif

using namespace anyps2::rt;
using namespace anyps2::rt::gs;

namespace {

struct Lcg {
    std::uint64_t s;
    std::uint32_t next(std::uint32_t n) {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>((s >> 33) % n);
    }
};

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

// ---- Cópia congelada do fetchTexel anterior à separação em texelRaw/expandTexel ----

std::uint32_t frozenExpand16(std::uint32_t v, std::uint32_t ta0, std::uint32_t ta1, bool aem) {
    const std::uint32_t rgb = ((v & 0x1F) << 3) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 19);
    std::uint32_t a;
    if (v & 0x8000) a = ta1;
    else a = (aem && (v & 0x7FFF) == 0) ? 0 : ta0;
    return rgb | (a << 24);
}

std::uint32_t frozenFetch(const Vram& vram, const Gs::TexelProbe& e, unsigned level, int u, int v) {
    const int w = std::max(1, (1 << e.tw) >> level);
    const int h = std::max(1, (1 << e.th) >> level);
    auto wrap = [](int c, int size, unsigned mode, unsigned mn, unsigned mx, unsigned lvl) {
        switch (mode) {
            case 0: return c & (size - 1);
            case 1: return std::clamp(c, 0, size - 1);
            case 2: return std::clamp(c, static_cast<int>(mn >> lvl), static_cast<int>(mx >> lvl));
            default: return (c & static_cast<int>(mn)) | static_cast<int>(mx);
        }
    };
    u = wrap(u, w, e.wms, e.minu, e.maxu, level);
    v = wrap(v, h, e.wmt, e.minv, e.maxv, level);
    const auto x = static_cast<std::uint32_t>(u) & 2047, y = static_cast<std::uint32_t>(v) & 2047;
    const std::uint32_t bp = e.tbp[level], bw = e.tbw[level];
    const std::uint32_t raw = vram.readPixel(e.tpsm, bp, bw, x, y);
    switch (e.tpsm) {
        case PSMCT32: case PSMZ32: return raw;
        case PSMCT24: case PSMZ24: {
            const std::uint32_t a = (e.aem && raw == 0) ? 0 : e.ta0;
            return raw | (a << 24);
        }
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S:
            return frozenExpand16(raw, e.ta0, e.ta1, e.aem);
        default: {
            const unsigned idx = e.csa * 16 + raw;
            if (e.cpsm == PSMCT32) {
                const unsigned i = idx & 255;
                return static_cast<std::uint32_t>(e.clut->v[i]) | (static_cast<std::uint32_t>(e.clut->v[i + 256]) << 16);
            }
            return frozenExpand16(e.clut->v[idx & 511], e.ta0, e.ta1, e.aem);
        }
    }
}

const std::uint32_t kPsms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8,  PSMT4,   PSMT8H,
                               PSMT4HL, PSMT4HH, PSMZ32,  PSMZ24,   PSMZ16, PSMZ16S};
const std::uint32_t kCpsms[] = {PSMCT32, PSMCT16, PSMCT16S};

std::shared_ptr<const ClutBuffer> randomClut(Lcg& r) {
    auto c = std::make_shared<ClutBuffer>();
    for (auto& x : c->v) x = static_cast<std::uint16_t>(r.next(65536));
    c->done.store(true);
    return c;
}

void fillRandom(Gs& g, Lcg& r) {
    std::uint8_t* d = g.vram().data();
    for (std::uint32_t i = 0; i < Vram::kSize; ++i) d[i] = static_cast<std::uint8_t>(r.next(256));
}

Gs::TexelProbe randomProbe(Lcg& r, std::uint32_t psm) {
    Gs::TexelProbe p;
    p.tpsm = psm;
    p.tw = r.next(11);
    p.th = r.next(11);
    p.wms = r.next(4);
    p.wmt = r.next(4);
    p.minu = r.next(1024);
    p.maxu = r.next(1024);
    p.minv = r.next(1024);
    p.maxv = r.next(1024);
    p.csa = r.next(32);
    p.cpsm = kCpsms[r.next(3)];
    p.ta0 = r.next(256);
    p.ta1 = r.next(256);
    p.aem = r.next(2) != 0;
    for (unsigned l = 0; l < 7; ++l) {
        p.tbp[l] = r.next(Vram::kBlocks);
        p.tbw[l] = 1 + r.next(16);
    }
    p.clut = randomClut(r);
    return p;
}

ClutBuffer* mutableClut(const Gs::TexelProbe& p) {
    return const_cast<ClutBuffer*>(p.clut.get());
}

}  // namespace

// As duas comparações com a cópia congelada rodam sem e com o cache de texels crus
// (ANYPS2_GS_TEXCACHE): o resultado tem de ser idêntico bit a bit.
static void checkFetchAllFormats(bool cache) {
    setEnv("ANYPS2_GS_THREAD", "0");
    setEnv("ANYPS2_GS_TEXCACHE", cache ? "1" : "");
    setEnv("ANYPS2_GS_TEXCACHE_VERIFY", "");  // o VERIFY liga o cache por conta própria
    Gs g(nullptr);
    CHECK_EQ(g.texCacheEnabled(), cache);
    Lcg r{0x7E8E1ull};
    fillRandom(g, r);
    const Vram& vram = g.vram();
    std::uint64_t checked = 0;
    for (std::uint32_t psm : kPsms) {
        for (unsigned tw = 0; tw <= 10; ++tw) {
            for (unsigned th = 0; th <= 10; ++th) {
                for (unsigned wms = 0; wms < 4; ++wms) {
                    for (unsigned wmt = 0; wmt < 4; ++wmt) {
                        Gs::TexelProbe p = randomProbe(r, psm);
                        p.tw = tw;
                        p.th = th;
                        p.wms = wms;
                        p.wmt = wmt;
                        for (unsigned k = 0; k < 6; ++k) {
                            const unsigned level = r.next(7);
                            const int u = static_cast<int>(r.next(6000)) - 3000;
                            const int v = static_cast<int>(r.next(6000)) - 3000;
                            const std::uint32_t want = frozenFetch(vram, p, level, u, v);
                            const std::uint32_t got = g.probeTexel(p, level, u, v);
                            if (got != want) {
                                CHECK_EQ(got, want);
                                CHECK_EQ(psm, 0u);  // identifica o formato
                                return;
                            }
                            ++checked;
                        }
                    }
                }
            }
        }
    }
    CHECK(checked > 100000);
    setEnv("ANYPS2_GS_THREAD", "");
    setEnv("ANYPS2_GS_TEXCACHE", "");
}

static void checkFetchCsaCpsmAndClutChange(bool cache) {
    setEnv("ANYPS2_GS_THREAD", "0");
    setEnv("ANYPS2_GS_TEXCACHE", cache ? "1" : "");
    setEnv("ANYPS2_GS_TEXCACHE_VERIFY", "");  // o VERIFY liga o cache por conta própria
    Gs g(nullptr);
    CHECK_EQ(g.texCacheEnabled(), cache);
    Lcg r{0xC1A7ull};
    fillRandom(g, r);
    const Vram& vram = g.vram();
    for (std::uint32_t psm : {static_cast<std::uint32_t>(PSMT8), static_cast<std::uint32_t>(PSMT4),
                              static_cast<std::uint32_t>(PSMT8H), static_cast<std::uint32_t>(PSMT4HL),
                              static_cast<std::uint32_t>(PSMT4HH)}) {
        for (std::uint32_t cpsm : kCpsms) {
            for (unsigned csa = 0; csa < 32; ++csa) {
                for (unsigned aem = 0; aem < 2; ++aem) {
                    Gs::TexelProbe p = randomProbe(r, psm);
                    p.cpsm = cpsm;
                    p.csa = csa;
                    p.aem = aem != 0;
                    p.tw = 6;
                    p.th = 6;
                    p.wms = 0;
                    p.wmt = 0;
                    // Várias versões de CLUT seguidas, com a paleta do desenho mudando.
                    for (unsigned c = 0; c < 2; ++c) {
                        if (c) p.clut = randomClut(r);
                        for (int y = 0; y < 64; y += 3) {
                            for (int x = 0; x < 64; x += 5) {
                                const std::uint32_t want = frozenFetch(vram, p, 0, x, y);
                                const std::uint32_t got = g.probeTexel(p, 0, x, y);
                                if (got != want) {
                                    CHECK_EQ(got, want);
                                    return;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    setEnv("ANYPS2_GS_THREAD", "");
}

TEST_CASE(gs_texel, fetch_matches_frozen_copy_all_formats_sizes_and_wraps) { checkFetchAllFormats(false); }
TEST_CASE(gs_texel, fetch_matches_frozen_copy_for_every_csa_cpsm_and_clut_change) { checkFetchCsaCpsmAndClutChange(false); }
TEST_CASE(gs_texel, cached_fetch_matches_frozen_copy_all_formats_sizes_and_wraps) { checkFetchAllFormats(true); }
TEST_CASE(gs_texel, cached_fetch_matches_frozen_copy_for_every_csa_cpsm_and_clut_change) {
    checkFetchCsaCpsmAndClutChange(true);
}

namespace {

// Cor de um índice da CLUT pela regra antiga (sem memo).
std::uint32_t frozenIndexed(const Gs::TexelProbe& e, std::uint32_t raw) {
    const unsigned idx = e.csa * 16 + raw;
    if (e.cpsm == PSMCT32) {
        const unsigned i = idx & 255;
        return static_cast<std::uint32_t>(e.clut->v[i]) | (static_cast<std::uint32_t>(e.clut->v[i + 256]) << 16);
    }
    return frozenExpand16(e.clut->v[idx & 511], e.ta0, e.ta1, e.aem);
}

}  // namespace

TEST_CASE(gs_texel, palette_matches_expansion_for_every_raw_csa_cpsm_and_texa) {
    setEnv("ANYPS2_GS_THREAD", "0");
    Gs g(nullptr);
    Lcg r{0x9A1E77Eull};
    std::uint32_t pal[256];
    for (std::uint32_t cpsm : kCpsms) {
        for (unsigned csa = 0; csa < 32; ++csa) {
            for (unsigned aem = 0; aem < 2; ++aem) {
                for (unsigned n : {16u, 256u}) {
                    Gs::TexelProbe p = randomProbe(r, PSMT8);
                    p.cpsm = cpsm;
                    p.csa = csa;
                    p.aem = aem != 0;
                    // Alguns TEXA extremos além dos aleatórios; entradas com cor 0 e com bit 15.
                    if (csa % 8 == 0) p.ta0 = 0;
                    if (csa % 8 == 1) p.ta1 = 0x80;
                    for (auto& x : mutableClut(p)->v) {
                        if (r.next(8) == 0) x = r.next(2) ? std::uint16_t{0} : std::uint16_t{0x8000};
                    }
                    g.probePalette(p, n, pal);
                    for (unsigned raw = 0; raw < n; ++raw) {
                        const std::uint32_t want = frozenIndexed(p, raw);
                        if (pal[raw] != want) {
                            CHECK_EQ(pal[raw], want);
                            return;
                        }
                    }
                }
            }
        }
    }
    setEnv("ANYPS2_GS_THREAD", "");
}

TEST_CASE(gs_texel, palette_is_rebuilt_only_when_the_key_changes) {
    setEnv("ANYPS2_GS_THREAD", "0");
    Gs g(nullptr);
    Lcg r{0x51ull};
    std::uint32_t pal[256];
    Gs::TexelProbe p = randomProbe(r, PSMT8);
    p.cpsm = PSMCT16;
    p.csa = 3;
    g.probePalette(p, 256, pal);
    const std::uint64_t b0 = g.paletteBuilds();
    g.probePalette(p, 256, pal);
    g.probePalette(p, 16, pal);  // T4 cabe na tabela de T8
    CHECK_EQ(g.paletteBuilds(), b0);
    p.csa = 4;
    g.probePalette(p, 256, pal);
    CHECK_EQ(g.paletteBuilds(), b0 + 1);
    p.ta0 ^= 0x40;
    g.probePalette(p, 256, pal);
    CHECK_EQ(g.paletteBuilds(), b0 + 2);
    p.ta1 ^= 0x40;
    g.probePalette(p, 256, pal);
    CHECK_EQ(g.paletteBuilds(), b0 + 3);
    p.aem = !p.aem;
    g.probePalette(p, 256, pal);
    CHECK_EQ(g.paletteBuilds(), b0 + 4);
    p.cpsm = PSMCT32;
    g.probePalette(p, 256, pal);
    CHECK_EQ(g.paletteBuilds(), b0 + 5);
    // Com CPSM=32 o TEXA não conta.
    p.ta0 ^= 0x10;
    p.aem = !p.aem;
    g.probePalette(p, 256, pal);
    CHECK_EQ(g.paletteBuilds(), b0 + 5);
    // Outra versão da CLUT (outro ponteiro) reconstrói.
    p.clut = randomClut(r);
    g.probePalette(p, 256, pal);
    CHECK_EQ(g.paletteBuilds(), b0 + 6);
    // Tabela de 16 entradas, depois pedido de 256: reconstrói.
    p.csa = 9;
    g.probePalette(p, 16, pal);
    const std::uint64_t b1 = g.paletteBuilds();
    g.probePalette(p, 256, pal);
    CHECK_EQ(g.paletteBuilds(), b1 + 1);
    setEnv("ANYPS2_GS_THREAD", "");
}

// ---- Gerações de página e simulação do cache (fatia 3) ----
//
// Textura CT32 64x64 em TBP 1024 (páginas 32 e 33). Cada texDraw é um sprite pequeno
// no FRAME 0 (fbw 2) lendo essa textura; a simulação conta acertos e o motivo de cada
// miss. As escritas intermediárias são de FRAME, de HOST→LOCAL (inteira e parcial) e
// de LOCAL→LOCAL.

namespace {

constexpr std::uint32_t kTexBp = 1024;
constexpr unsigned kTexWords = 64 * 64 / 2;  // CT32: dois pixels por palavra

std::uint64_t frameRegT(std::uint64_t fbp, std::uint64_t fbw) {
    return fbp | (fbw << 16) | (std::uint64_t{PSMCT32} << 24);
}
std::uint64_t xyT(std::uint64_t x, std::uint64_t y) { return (x * 16) | ((y * 16) << 16); }

struct SimScene {
    Gs g{nullptr};
    std::vector<std::uint64_t> words;

    SimScene() {
        g.forceTexSim(true);
        g.writeRegister(FRAME_1, frameRegT(0, 2), 0);
        g.writeRegister(SCISSOR_1, 0ull | (127ull << 16) | (127ull << 48), 0);
        g.writeRegister(XYOFFSET_1, 0, 0);
        g.writeRegister(PRMODECONT, 1, 0);
        for (unsigned i = 0; i < kTexWords; ++i) words.push_back(0x0102030405060708ull * (i + 1));
    }
    void hostBegin(std::uint32_t bp = kTexBp) {
        g.writeRegister(BITBLTBUF, (std::uint64_t{bp} << 32) | (1ull << 48) | (std::uint64_t{PSMCT32} << 56), 0);
        g.writeRegister(TRXPOS, 0, 0);
        g.writeRegister(TRXREG, 64 | (64ull << 32), 0);
        g.writeRegister(TRXDIR, 0, 0);
    }
    void hostWords(unsigned from, unsigned to) {
        for (unsigned i = from; i < to; ++i) g.writeRegister(HWREG, words[i], 0);
    }
    void hostAll() {
        hostBegin();
        hostWords(0, kTexWords);
    }
    // Sprite 8x8 no FRAME corrente lendo o canto da textura.
    void texDraw() {
        const std::uint64_t tex0v = std::uint64_t{kTexBp} | (1ull << 14) | (std::uint64_t{PSMCT32} << 20) | (6ull << 26) |
                                    (6ull << 30);
        g.writeRegister(TEX0_1, tex0v, 0);
        g.writeRegister(PRIM, 6u | 16u | 256u, 0);
        g.writeRegister(RGBAQ, 0x80808080ull | (0x3F800000ull << 32), 0);
        g.writeRegister(UV, 0, 0);
        g.writeRegister(XYZ2, xyT(16, 16), 0);
        g.writeRegister(UV, (8ull * 16) | ((8ull * 16) << 16), 0);
        g.writeRegister(XYZ2, xyT(24, 24), 0);
    }
    // Sprite sem textura escrito num FRAME qualquer (por exemplo, sobre a textura).
    void frameDraw(std::uint32_t fbp, std::uint32_t fbw) {
        g.writeRegister(FRAME_1, frameRegT(fbp, fbw), 0);
        g.writeRegister(PRIM, 6u, 0);
        g.writeRegister(RGBAQ, 0x40404040ull | (0x3F800000ull << 32), 0);
        g.writeRegister(XYZ2, xyT(0, 0), 0);
        g.writeRegister(XYZ2, xyT(8, 8), 0);
        g.writeRegister(FRAME_1, frameRegT(0, 2), 0);
    }
    void copyIntoTexture() {
        // Origem: bloco 16x16 do FRAME 0; destino: o canto da textura (página 32).
        g.writeRegister(BITBLTBUF,
                        0ull | (2ull << 16) | (std::uint64_t{PSMCT32} << 24) | (std::uint64_t{kTexBp} << 32) |
                            (1ull << 48) | (std::uint64_t{PSMCT32} << 56),
                        0);
        g.writeRegister(TRXPOS, 0, 0);
        g.writeRegister(TRXREG, 16 | (16ull << 32), 0);
        g.writeRegister(TRXDIR, 2, 0);
    }
};

struct Snap {
    std::uint64_t hits, miss[TexSimReport::MissCount];
    explicit Snap(const Gs& g) {
        const TexSimReport& r = g.texSim();
        hits = r.hits;
        for (unsigned i = 0; i < TexSimReport::MissCount; ++i) miss[i] = r.miss[i];
    }
};

}  // namespace

TEST_CASE(gs_texel, texcache_sim_hits_and_miss_reasons) {
    for (const char* threaded : {"0", "1"}) {
        setEnv("ANYPS2_GS_THREAD", threaded);
        setEnv("ANYPS2_GS_THREADS", "2");
        SimScene s;
        s.hostAll();
        s.texDraw();
        CHECK_EQ(s.g.texSim().miss[TexSimReport::MissFirst], 1u);
        CHECK_EQ(s.g.texSim().hits, 0u);
        s.texDraw();  // sem escrita no meio: acerto
        CHECK_EQ(s.g.texSim().hits, 1u);

        Snap a(s.g);
        s.frameDraw(kTexBp / 32, 1);  // FRAME em cima da página 32 da textura
        s.texDraw();
        CHECK_EQ(s.g.texSim().miss[TexSimReport::MissFrame], a.miss[TexSimReport::MissFrame] + 1);
        CHECK_EQ(s.g.texSim().hits, a.hits);
        s.texDraw();
        CHECK_EQ(s.g.texSim().hits, a.hits + 1);

        // FRAME numa página que não é da textura: continua acertando.
        s.frameDraw(0, 2);
        s.texDraw();
        CHECK_EQ(s.g.texSim().hits, a.hits + 2);

        Snap b(s.g);
        s.hostAll();  // HOST→LOCAL inteira sobre a textura
        s.texDraw();
        CHECK_EQ(s.g.texSim().miss[TexSimReport::MissHost], b.miss[TexSimReport::MissHost] + 1);
        s.texDraw();
        CHECK_EQ(s.g.texSim().hits, b.hits + 1);

        Snap c(s.g);
        s.copyIntoTexture();  // LOCAL→LOCAL sobre a textura
        s.texDraw();
        CHECK_EQ(s.g.texSim().miss[TexSimReport::MissCopy], c.miss[TexSimReport::MissCopy] + 1);
        s.texDraw();
        CHECK_EQ(s.g.texSim().hits, c.hits + 1);

        // Uma textura só: nunca houve outra primeira vez.
        CHECK_EQ(s.g.texSim().miss[TexSimReport::MissFirst], 1u);
    }
    setEnv("ANYPS2_GS_THREAD", "");
    setEnv("ANYPS2_GS_THREADS", "");
}

TEST_CASE(gs_texel, texcache_sim_partial_host_transfer_misses_after_more_words) {
    for (const char* threaded : {"0", "1"}) {
        setEnv("ANYPS2_GS_THREAD", threaded);
        setEnv("ANYPS2_GS_THREADS", "2");
        SimScene s;
        s.hostAll();
        s.texDraw();  // miss de primeira vez
        s.hostBegin();
        s.hostWords(0, 1000);  // transferência aberta: faltam 1048 palavras
        s.texDraw();           // fixa uma versão com o conteúdo parcial (a transferência já bumpou)
        Snap a(s.g);
        s.texDraw();  // sem escrita no meio
        CHECK_EQ(s.g.texSim().hits, a.hits + 1);
        s.hostWords(1000, 1500);  // mais palavras sobre a textura fixada
        s.texDraw();              // tem de dar miss por HOST, não acerto
        CHECK_EQ(s.g.texSim().hits, a.hits + 1);
        CHECK_EQ(s.g.texSim().miss[TexSimReport::MissHost], a.miss[TexSimReport::MissHost] + 1);
        s.texDraw();  // a versão nova vale até a próxima palavra
        CHECK_EQ(s.g.texSim().hits, a.hits + 2);
        s.hostWords(1500, kTexWords);  // fim da transferência
        s.texDraw();
        CHECK_EQ(s.g.texSim().miss[TexSimReport::MissHost], a.miss[TexSimReport::MissHost] + 2);
        s.texDraw();
        CHECK_EQ(s.g.texSim().hits, a.hits + 3);

        // Transferência aberta em outro lugar da VRAM: as palavras não invalidam a textura.
        s.hostBegin(4096);
        s.hostWords(0, 100);
        s.texDraw();
        Snap b(s.g);
        s.hostWords(100, 200);
        s.texDraw();
        CHECK_EQ(s.g.texSim().hits, b.hits + 1);
    }
    setEnv("ANYPS2_GS_THREAD", "");
    setEnv("ANYPS2_GS_THREADS", "");
}

TEST_CASE(gs_texel, texcache_sim_texture_on_own_frame_is_off_cache) {
    setEnv("ANYPS2_GS_THREAD", "0");
    SimScene s;
    s.hostAll();
    s.g.writeRegister(FRAME_1, frameRegT(kTexBp / 32, 1), 0);
    // A geração é lida depois do HOST→LOCAL (que já bumpou a página 32): só o desenho
    // sozinho pode fazê-la subir daqui em diante.
    const std::uint32_t g0 = s.g.pageGeneration(32);
    s.texDraw();  // textura sobre o próprio FRAME
    CHECK_EQ(s.g.texSim().off[TexSimReport::OffTexture], 1u);
    CHECK_EQ(s.g.texSim().hits, 0u);
    CHECK_EQ(s.g.texSim().miss[TexSimReport::MissFirst], 0u);
    // O desenho sozinho também escreveu: a geração da página mudou.
    CHECK_EQ(s.g.pageGeneration(32), g0 + 1);
    setEnv("ANYPS2_GS_THREAD", "");
}

TEST_CASE(gs_texel, texcache_sim_frame_and_zbuf_on_same_page_bumps_both) {
    setEnv("ANYPS2_GS_THREAD", "0");
    SimScene s;
    // Desenho sem textura com FRAME e ZBUF na mesma página (selfFrameZ): roda sozinho,
    // mas escreve as duas superfícies, e a geração da página tem de subir.
    const std::uint32_t page = 40;
    const std::uint32_t g0 = s.g.pageGeneration(page);
    s.g.writeRegister(FRAME_1, frameRegT(page, 1), 0);
    s.g.writeRegister(ZBUF_1, page, 0);
    s.g.writeRegister(TEST_1, (1ull << 16) | (1ull << 17), 0);  // ZTE, ZTST = ALWAYS
    s.g.writeRegister(PRIM, 6u, 0);
    s.g.writeRegister(RGBAQ, 0x40404040ull | (0x3F800000ull << 32), 0);
    s.g.writeRegister(XYZ2, xyT(0, 0), 0);
    s.g.writeRegister(XYZ2, xyT(8, 8), 0);
    CHECK_EQ(s.g.bandStats().self[BandStats::SelfFrameZ], 1u);
    CHECK(s.g.pageGeneration(page) >= g0 + 1);
    setEnv("ANYPS2_GS_THREAD", "");
}

TEST_CASE(gs_texel, texcache_sim_zbuf_write_on_texture_page_misses_by_zbuf) {
    for (const char* threaded : {"0", "1"}) {
        setEnv("ANYPS2_GS_THREAD", threaded);
        setEnv("ANYPS2_GS_THREADS", "2");
        SimScene s;
        s.hostAll();
        s.texDraw();
        s.texDraw();
        CHECK_EQ(s.g.texSim().hits, 1u);
        Snap a(s.g);
        // Desenho sem textura: FRAME em outro lugar, ZBUF em cima da página 32 da textura.
        s.g.writeRegister(ZBUF_1, kTexBp / 32, 0);
        s.g.writeRegister(TEST_1, (1ull << 16) | (1ull << 17), 0);  // ZTE, ZTST = ALWAYS
        s.g.writeRegister(PRIM, 6u, 0);
        s.g.writeRegister(RGBAQ, 0x40404040ull | (0x3F800000ull << 32), 0);
        s.g.writeRegister(XYZ2, xyT(0, 0), 0);
        s.g.writeRegister(XYZ2, xyT(8, 8), 0);
        s.g.writeRegister(TEST_1, 0, 0);
        s.texDraw();
        CHECK_EQ(s.g.texSim().miss[TexSimReport::MissZbuf], a.miss[TexSimReport::MissZbuf] + 1);
        CHECK_EQ(s.g.texSim().hits, a.hits);
        s.texDraw();
        CHECK_EQ(s.g.texSim().hits, a.hits + 1);
    }
    setEnv("ANYPS2_GS_THREAD", "");
    setEnv("ANYPS2_GS_THREADS", "");
}
