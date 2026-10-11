// Cache de texels crus do GS (gs_texcache.h, ANYPS2_GS_TEXCACHE=1).
//
// A regra de todo teste de cena: a VRAM inteira com o cache ligado tem de ser idêntica
// à do mesmo programa com o cache desligado, em 1 e em 4 faixas, e o modo VERIFY (que
// compara cada leitura do cache com a VRAM) não pode achar diferença. As cenas atacam
// cada jeito de a textura mudar debaixo do cache: HOST→LOCAL, LOCAL→LOCAL, render para
// textura, FRAME e ZBUF na mesma página, recarga de CLUT, CSA/TEXA sem recarga,
// transferência parcial com desenho no meio e textura que dá a volta no fim da VRAM.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
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
    std::uint64_t word() {
        return (std::uint64_t{next(1u << 16)} << 48) | (std::uint64_t{next(1u << 16)} << 32) |
               (std::uint64_t{next(1u << 16)} << 16) | next(1u << 16);
    }
};

void setEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void resetEnv() {
    setEnv("ANYPS2_GS_THREAD", "");
    setEnv("ANYPS2_GS_THREADS", "");
    setEnv("ANYPS2_GS_TEXCACHE", "");
    setEnv("ANYPS2_GS_TEXCACHE_VERIFY", "");
    setEnv("ANYPS2_GS_TEXCACHE_MB", "");
}

constexpr std::uint32_t kFrameFbp = 128;  // página do FRAME principal (bloco 4096)
constexpr std::uint32_t kTexA = 1024;     // páginas 32-33
constexpr std::uint32_t kTexB = 1280;     // páginas 40-41
constexpr std::uint32_t kTexMip1 = 1792;  // páginas 56-57
constexpr std::uint32_t kTexT8 = 2048;    // página 64
constexpr std::uint32_t kTexT4 = 2304;    // página 72
constexpr std::uint32_t kClutBp = 3072;   // página 96
constexpr std::uint32_t kWrapBp = 16368;  // 16 blocos antes do fim da VRAM: a textura dá a volta

struct Tex {
    std::uint32_t bp = 0, bw = 1, psm = PSMCT32, tw = 6, th = 6;
    std::uint64_t cbp = 0, cpsm = 0, csa = 0;
    std::uint64_t reg() const {
        return std::uint64_t{bp} | (std::uint64_t{bw} << 14) | (std::uint64_t{psm} << 20) | (std::uint64_t{tw} << 26) |
               (std::uint64_t{th} << 30) | (cbp << 37) | (cpsm << 51) | (csa << 56);
    }
};

std::uint64_t xy(std::uint64_t x, std::uint64_t y, std::uint64_t z = 0) {
    return (x * 16) | ((y * 16) << 16) | (z << 32);
}
std::uint64_t uvReg(unsigned u, unsigned v) {
    return std::uint64_t{u * 16u} | (std::uint64_t{v * 16u} << 16);
}
std::uint64_t rgbaq(std::uint32_t c) {
    return std::uint64_t{c} | (std::uint64_t{0x3F800000u} << 32);
}

// Um programa de GS escrito sobre uma Gs recém-criada (com o ambiente já posto).
struct Scene {
    Gs g{nullptr};
    Lcg r;

    explicit Scene(std::uint64_t seed) : r{seed} {
        g.setTexVerifyAbort(false);
        g.writeRegister(PRMODECONT, 1, 0);
        g.writeRegister(XYOFFSET_1, 0, 0);
        g.writeRegister(TEST_1, 0, 0);
        g.writeRegister(ZBUF_1, std::uint64_t{1} << 32, 0);  // ZMSK
        g.writeRegister(TEXA, 0x80 | (0x80ull << 32), 0);
        mainTarget();
    }

    void setTarget(std::uint32_t fbp, std::uint32_t fbw, unsigned w, unsigned h) {
        g.writeRegister(FRAME_1, std::uint64_t{fbp} | (std::uint64_t{fbw} << 16) | (std::uint64_t{PSMCT32} << 24), 0);
        g.writeRegister(SCISSOR_1, 0ull | (std::uint64_t{w - 1} << 16) | (0ull << 32) | (std::uint64_t{h - 1} << 48), 0);
    }
    void mainTarget() { setTarget(kFrameFbp, 2, 128, 128); }
    void bilinear(bool on) { g.writeRegister(TEX1_1, on ? ((1ull << 5) | (1ull << 6)) : 0ull, 0); }
    void clamp(unsigned wms, unsigned wmt, unsigned minu, unsigned maxu, unsigned minv, unsigned maxv) {
        g.writeRegister(CLAMP_1,
                        wms | (std::uint64_t{wmt} << 2) | (std::uint64_t{minu} << 4) | (std::uint64_t{maxu} << 14) |
                            (std::uint64_t{minv} << 24) | (std::uint64_t{maxv} << 34),
                        0);
    }
    void texa(unsigned ta0, unsigned ta1, bool aem) {
        g.writeRegister(TEXA, ta0 | (aem ? 0x8000u : 0u) | (std::uint64_t{ta1} << 32), 0);
    }
    // ZBUF no formato Z32; ZTE ligado com ZTST = ALWAYS escreve Z em ZBP.
    void zbuf(std::uint32_t zbp, bool zte) {
        g.writeRegister(ZBUF_1, zbp, 0);
        g.writeRegister(TEST_1, zte ? ((1ull << 16) | (1ull << 17)) : 0ull, 0);
    }

    void hostBegin(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, unsigned x, unsigned y, unsigned w,
                   unsigned h) {
        g.writeRegister(BITBLTBUF, (std::uint64_t{bp} << 32) | (std::uint64_t{bw} << 48) | (std::uint64_t{psm} << 56), 0);
        g.writeRegister(TRXPOS, (std::uint64_t{x} << 32) | (std::uint64_t{y} << 48), 0);
        g.writeRegister(TRXREG, w | (std::uint64_t{h} << 32), 0);
        g.writeRegister(TRXDIR, 0, 0);
    }
    void hostWords(unsigned n) {
        for (unsigned i = 0; i < n; ++i) g.writeRegister(HWREG, r.word(), 0);
    }
    static unsigned wordsFor(std::uint32_t psm, unsigned w, unsigned h) {
        return static_cast<unsigned>((std::uint64_t{w} * h * psmTransferBits(psm) + 63) / 64);
    }
    // HOST→LOCAL completa de um retângulo, com dados novos aleatórios.
    void upload(std::uint32_t bp, std::uint32_t bw, std::uint32_t psm, unsigned x, unsigned y, unsigned w, unsigned h) {
        hostBegin(bp, bw, psm, x, y, w, h);
        hostWords(wordsFor(psm, w, h));
    }
    void uploadTex(const Tex& t) { upload(t.bp, t.bw, t.psm, 0, 0, 1u << t.tw, 1u << t.th); }
    void copy(std::uint32_t sbp, std::uint32_t sbw, std::uint32_t dbp, std::uint32_t dbw, unsigned sx, unsigned sy,
              unsigned dx, unsigned dy, unsigned w, unsigned h) {
        g.writeRegister(BITBLTBUF,
                        std::uint64_t{sbp} | (std::uint64_t{sbw} << 16) | (std::uint64_t{PSMCT32} << 24) |
                            (std::uint64_t{dbp} << 32) | (std::uint64_t{dbw} << 48) | (std::uint64_t{PSMCT32} << 56),
                        0);
        g.writeRegister(TRXPOS, sx | (std::uint64_t{sy} << 16) | (std::uint64_t{dx} << 32) | (std::uint64_t{dy} << 48), 0);
        g.writeRegister(TRXREG, w | (std::uint64_t{h} << 32), 0);
        g.writeRegister(TRXDIR, 2, 0);
    }
    // Carrega a CLUT da textura (TEX0 com CLD = 1).
    void loadClut(const Tex& t) { g.writeRegister(TEX0_1, t.reg() | (1ull << 61), 0); }

    // Sprite texturizado (UV em texels).
    void sprite(const Tex& t, unsigned x0, unsigned y0, unsigned x1, unsigned y1, unsigned u0, unsigned v0, unsigned u1,
                unsigned v1) {
        g.writeRegister(TEX0_1, t.reg(), 0);
        g.writeRegister(PRIM, 6u | 16u | 256u, 0);
        g.writeRegister(RGBAQ, rgbaq(0x80808080u), 0);
        g.writeRegister(UV, uvReg(u0, v0), 0);
        g.writeRegister(XYZ2, xy(x0, y0), 0);
        g.writeRegister(UV, uvReg(u1, v1), 0);
        g.writeRegister(XYZ2, xy(x1, y1), 0);
    }
    // Triângulo texturizado (UV em texels).
    void tri(const Tex& t, unsigned x0, unsigned y0, unsigned u0, unsigned v0, unsigned x1, unsigned y1, unsigned u1,
             unsigned v1, unsigned x2, unsigned y2, unsigned u2, unsigned v2) {
        g.writeRegister(TEX0_1, t.reg(), 0);
        g.writeRegister(PRIM, 3u | 16u | 256u, 0);
        g.writeRegister(RGBAQ, rgbaq(0x80808080u), 0);
        g.writeRegister(UV, uvReg(u0, v0), 0);
        g.writeRegister(XYZ2, xy(x0, y0), 0);
        g.writeRegister(UV, uvReg(u1, v1), 0);
        g.writeRegister(XYZ2, xy(x1, y1), 0);
        g.writeRegister(UV, uvReg(u2, v2), 0);
        g.writeRegister(XYZ2, xy(x2, y2), 0);
    }
    // Sprite sem textura (cor chapada), com Z.
    void flat(unsigned x0, unsigned y0, unsigned x1, unsigned y1, std::uint32_t color, std::uint32_t z = 0x1234) {
        g.writeRegister(PRIM, 6u, 0);
        g.writeRegister(RGBAQ, rgbaq(color), 0);
        g.writeRegister(XYZ2, xy(x0, y0, z), 0);
        g.writeRegister(XYZ2, xy(x1, y1, z), 0);
    }
};

using Scenario = void (*)(Scene&);

struct Out {
    std::vector<std::uint8_t> vram;
    TexCacheReport rep;
    std::uint64_t mismatches = 0;
};

// mode: 0 = cache desligado, 1 = ligado, 2 = ligado com VERIFY.
Out render(Scenario fn, std::uint64_t seed, unsigned lanes, int mode) {
    setEnv("ANYPS2_GS_THREAD", "1");
    setEnv("ANYPS2_GS_THREADS", std::to_string(lanes).c_str());
    setEnv("ANYPS2_GS_TEXCACHE", mode >= 1 ? "1" : "");
    setEnv("ANYPS2_GS_TEXCACHE_VERIFY", mode == 2 ? "1" : "");
    Out o;
    {
        Scene s(seed);
        CHECK_EQ(s.g.texCacheEnabled(), mode >= 1);
        fn(s);
        const std::uint8_t* d = s.g.vram().data();  // espera as faixas
        o.vram.assign(d, d + Vram::kSize);
        o.rep = s.g.texCacheReport();
        o.mismatches = s.g.texVerifyMismatches();
    }
    resetEnv();
    return o;
}

// Roda a cena nos três modos, em 1 e em 4 faixas. minVersions: quantas versões de
// textura o cache tem de ter criado (prova que a cena invalidou o que devia).
void runScene(const char* name, Scenario fn, std::uint64_t minVersions) {
    for (unsigned lanes : {1u, 4u}) {
        const Out off = render(fn, 0xC0FFEEull, lanes, 0);
        for (int mode : {1, 2}) {
            const Out on = render(fn, 0xC0FFEEull, lanes, mode);
            bool same = on.vram.size() == off.vram.size();
            std::size_t at = 0;
            if (same) {
                for (; at < on.vram.size(); ++at) {
                    if (on.vram[at] != off.vram[at]) {
                        same = false;
                        break;
                    }
                }
            }
            if (!same) {
                std::printf("cena '%s': VRAM difere no byte %zu com cache (modo %d, %u faixas)\n", name, at, mode, lanes);
            }
            CHECK(same);
            CHECK_EQ(on.mismatches, 0u);
            CHECK(on.rep.versions >= minVersions);
        }
    }
}

// ---- Cenas ----

Tex texA() { return Tex{kTexA, 1, PSMCT32, 6, 6}; }
Tex texB() { return Tex{kTexB, 1, PSMCT32, 6, 6}; }

// Desenhos que leem a textura inteira, ampliada, reduzida e deslocada.
void drawAround(Scene& s, const Tex& t, unsigned row) {
    s.sprite(t, 0, row, 64, row + 32, 0, 0, 64, 64);
    s.sprite(t, 64, row, 128, row + 32, 0, 0, 32, 16);
}

void sceneHostOverTexture(Scene& s) {
    const Tex a = texA();
    s.uploadTex(a);
    drawAround(s, a, 0);
    drawAround(s, a, 0);  // mesma versão
    s.uploadTex(a);       // HOST inteira sobre a textura
    drawAround(s, a, 32);
    s.upload(a.bp, a.bw, a.psm, 16, 16, 16, 16);  // HOST num retângulo
    drawAround(s, a, 64);
    s.bilinear(true);
    s.upload(a.bp, a.bw, a.psm, 40, 0, 24, 8);
    drawAround(s, a, 96);
}

void sceneLocalToLocalOverTexture(Scene& s) {
    const Tex a = texA();
    s.uploadTex(a);
    drawAround(s, a, 0);
    // Do FRAME principal (já desenhado) para a textura.
    s.copy(kFrameFbp * 32, 2, a.bp, a.bw, 0, 0, 0, 0, 32, 32);
    drawAround(s, a, 32);
    s.copy(kFrameFbp * 32, 2, a.bp, a.bw, 0, 16, 32, 32, 32, 32);
    drawAround(s, a, 64);
    // Cópia da textura sobre ela mesma.
    s.copy(a.bp, a.bw, a.bp, a.bw, 0, 0, 16, 16, 24, 24);
    drawAround(s, a, 96);
}

void sceneRenderToTexture(Scene& s) {
    const Tex p{kTexA, 1, PSMCT32, 6, 6};
    const Tex b = texB();
    s.uploadTex(b);
    s.setTarget(kTexA / 32, 1, 64, 64);  // o destino é a textura P
    s.flat(0, 0, 64, 64, 0x20408060u);
    s.sprite(b, 8, 8, 56, 56, 0, 0, 64, 64);
    s.mainTarget();
    drawAround(s, p, 0);  // P, primeira versão
    drawAround(s, p, 0);
    s.setTarget(kTexA / 32, 1, 64, 64);
    s.flat(16, 16, 48, 48, 0x80FF4020u);
    s.mainTarget();
    drawAround(s, p, 32);  // P, segunda versão
    s.setTarget(kTexA / 32, 1, 64, 64);
    s.sprite(b, 0, 0, 64, 32, 0, 0, 64, 32);
    s.mainTarget();
    s.bilinear(true);
    drawAround(s, p, 64);  // terceira
    drawAround(s, p, 96);
}

void sceneTextureOnOwnFrame(Scene& s) {
    const Tex p{kTexA, 1, PSMCT32, 6, 6};
    s.upload(p.bp, p.bw, p.psm, 0, 0, 64, 64);
    drawAround(s, p, 0);
    s.setTarget(kTexA / 32, 1, 64, 64);  // lê P enquanto escreve em P (selfTexture)
    s.sprite(p, 8, 8, 56, 56, 0, 0, 48, 48);
    s.sprite(p, 0, 0, 32, 32, 16, 16, 48, 48);
    s.mainTarget();
    drawAround(s, p, 32);
    drawAround(s, p, 64);
}

void sceneZbufOnTexturePage(Scene& s) {
    const Tex a = texA();
    s.uploadTex(a);
    drawAround(s, a, 0);
    // Escreve Z em cima das páginas da textura (FRAME em outro lugar).
    s.zbuf(kTexA / 32, true);
    s.flat(0, 0, 64, 64, 0x40404040u, 0x11111111u);
    s.zbuf(kTexA / 32, false);
    drawAround(s, a, 32);
    drawAround(s, a, 64);
    // FRAME e ZBUF na mesma página (selfFrameZ), e uma textura lida no meio.
    s.zbuf(kFrameFbp, true);
    s.flat(0, 96, 32, 128, 0x30303030u);
    s.sprite(a, 32, 96, 64, 128, 0, 0, 32, 32);
    s.zbuf(kTexA / 32, false);
    drawAround(s, a, 96);
}

void sceneClutAndTexa(Scene& s) {
    Tex t8{kTexT8, 2, PSMT8, 6, 6, kClutBp, PSMCT32, 0};
    Tex t4{kTexT4, 2, PSMT4, 6, 6, kClutBp, PSMCT32, 0};
    s.upload(t8.bp, t8.bw, t8.psm, 0, 0, 64, 64);
    s.upload(t4.bp, t4.bw, t4.psm, 0, 0, 64, 64);
    s.upload(kClutBp, 1, PSMCT32, 0, 0, 16, 16);
    s.loadClut(t8);
    drawAround(s, t8, 0);
    drawAround(s, t4, 0);
    s.loadClut(t8);  // recarga com o mesmo conteúdo
    drawAround(s, t8, 32);
    s.upload(kClutBp, 1, PSMCT32, 0, 0, 16, 16);  // conteúdo novo na VRAM, ainda sem recarga
    drawAround(s, t8, 32);
    s.loadClut(t8);  // recarga com conteúdo diferente
    drawAround(s, t8, 64);
    drawAround(s, t4, 64);
    t8.csa = 3;  // CSA diferente sem recarga
    t4.csa = 5;
    drawAround(s, t8, 96);
    drawAround(s, t4, 96);
    // CLUT de 16 bits: o TEXA e o AEM entram na cor.
    t8.cpsm = PSMCT16;
    t4.cpsm = PSMCT16;
    t8.csa = 0;
    t4.csa = 0;
    s.loadClut(t8);
    drawAround(s, t8, 0);
    drawAround(s, t4, 0);
    s.texa(0x20, 0xC0, false);
    drawAround(s, t8, 32);
    drawAround(s, t4, 32);
    s.texa(0x20, 0xC0, true);
    drawAround(s, t8, 64);
    t8.csa = 2;
    drawAround(s, t8, 96);
    s.bilinear(true);
    s.texa(0x70, 0x10, true);
    drawAround(s, t4, 96);
}

void scenePartialHost(Scene& s) {
    const Tex a = texA();
    s.uploadTex(a);
    drawAround(s, a, 0);
    const unsigned total = Scene::wordsFor(a.psm, 64, 64);
    s.hostBegin(a.bp, a.bw, a.psm, 0, 0, 64, 64);
    s.hostWords(total / 4);
    drawAround(s, a, 32);  // lê o conteúdo parcial
    s.hostWords(total / 4);
    drawAround(s, a, 32);  // e de novo, com mais palavras
    s.hostWords(total / 4);
    s.tri(a, 0, 64, 0, 0, 64, 64, 64, 0, 0, 128, 0, 64);
    s.hostWords(total - 3 * (total / 4));
    drawAround(s, a, 64);
    drawAround(s, a, 96);
    // Transferência aberta em outro lugar não invalida a textura.
    s.hostBegin(kTexB, 1, PSMCT32, 0, 0, 64, 64);
    s.hostWords(100);
    drawAround(s, a, 0);
    s.hostWords(100);
    drawAround(s, a, 32);
}

void sceneWrapAroundVramEnd(Scene& s) {
    const Tex w{kWrapBp, 1, PSMCT32, 6, 6};
    s.uploadTex(w);
    drawAround(s, w, 0);
    drawAround(s, w, 32);
    s.upload(w.bp, w.bw, w.psm, 0, 40, 64, 16);  // linhas que caem depois da volta
    drawAround(s, w, 64);
    s.upload(w.bp, w.bw, w.psm, 0, 0, 64, 8);  // linhas antes da volta
    s.bilinear(true);
    drawAround(s, w, 96);
    // Cópia cujo destino cai na parte que deu a volta.
    s.copy(kFrameFbp * 32, 2, w.bp, w.bw, 0, 0, 0, 48, 16, 16);
    drawAround(s, w, 0);
    // T8 também dá a volta.
    const Tex w8{16380, 2, PSMT8, 6, 6, kClutBp, PSMCT32, 0};
    s.upload(kClutBp, 1, PSMCT32, 0, 0, 16, 16);
    s.upload(w8.bp, w8.bw, w8.psm, 0, 0, 64, 64);
    s.loadClut(w8);
    drawAround(s, w8, 32);
    s.upload(w8.bp, w8.bw, w8.psm, 0, 32, 64, 32);
    drawAround(s, w8, 64);
}

void sceneRegionModes(Scene& s) {
    const Tex a = texA();
    s.uploadTex(a);
    // REGION_CLAMP menor que a textura: o desenho alcança só parte dela.
    s.clamp(2, 2, 8, 40, 4, 30);
    drawAround(s, a, 0);
    // Escrita fora do que o desenho alcança, mas dentro da textura.
    s.upload(a.bp, a.bw, a.psm, 48, 40, 16, 16);
    drawAround(s, a, 32);
    // REGION_REPEAT cujas coordenadas passam do tamanho da textura (fallback).
    s.clamp(3, 3, 0x3F, 0x40, 0x1F, 0x20);
    drawAround(s, a, 64);
    s.clamp(0, 1, 0, 0, 0, 0);  // REPEAT em U, CLAMP em V
    s.sprite(a, 0, 96, 128, 128, 0, 0, 200, 90);
    s.clamp(1, 0, 0, 0, 0, 0);
    s.bilinear(true);
    s.sprite(a, 0, 96, 128, 128, 0, 0, 90, 200);
}

void sceneMipmap(Scene& s) {
    Tex lv0{kTexA, 1, PSMCT32, 6, 6};
    const Tex lv1{kTexMip1, 1, PSMCT32, 5, 5};
    s.uploadTex(lv0);
    s.uploadTex(lv1);
    s.g.writeRegister(MIPTBP1_1, std::uint64_t{kTexMip1} | (std::uint64_t{1} << 14), 0);
    // LCM = 1 (LOD fixo = K), MXL = 1, MMIN = NEAREST_MIPMAP_NEAREST.
    auto tex1 = [&](int k) {
        s.g.writeRegister(TEX1_1, 1ull | (1ull << 2) | (2ull << 6) | (static_cast<std::uint64_t>(k & 0xFFF) << 32), 0);
    };
    tex1(0);
    drawAround(s, lv0, 0);
    tex1(16);  // nível 1
    drawAround(s, lv0, 32);
    s.uploadTex(lv1);  // só o nível 1 muda
    drawAround(s, lv0, 64);
    tex1(0);
    drawAround(s, lv0, 96);  // o nível 0 continua o mesmo
    s.uploadTex(lv0);
    tex1(16);
    drawAround(s, lv0, 0);
}

// Muitos desenhos pequenos sobre a mesma textura: faixas diferentes preenchem e leem os
// mesmos blocos ao mesmo tempo.
void sceneManySmallDraws(Scene& s) {
    Tex a{kTexA, 2, PSMCT32, 7, 7};
    Tex t8{kTexT8, 2, PSMT8, 7, 7, kClutBp, PSMCT32, 0};
    s.upload(a.bp, a.bw, a.psm, 0, 0, 128, 128);
    s.upload(t8.bp, t8.bw, t8.psm, 0, 0, 128, 128);
    s.upload(kClutBp, 1, PSMCT32, 0, 0, 16, 16);
    s.loadClut(t8);
    for (unsigned i = 0; i < 300; ++i) {
        const unsigned x = s.r.next(120), y = s.r.next(120);
        const unsigned w = 2 + s.r.next(9), h = 2 + s.r.next(9);
        const unsigned u = s.r.next(110), v = s.r.next(110);
        s.bilinear(i % 3 == 0);
        s.sprite(i % 4 == 3 ? t8 : a, x, y, x + w, y + h, u, v, u + 1 + s.r.next(24), v + 1 + s.r.next(24));
        if (i % 50 == 49) s.upload(a.bp, a.bw, a.psm, s.r.next(64), s.r.next(64), 32, 32);
    }
    for (unsigned i = 0; i < 40; ++i) {
        const unsigned x = s.r.next(100), y = s.r.next(100);
        s.tri(a, x, y, s.r.next(100), s.r.next(100), x + 24, y + 4, s.r.next(100), s.r.next(100), x + 8, y + 28,
              s.r.next(100), s.r.next(100));
    }
}

}  // namespace

TEST_CASE(gs_texcache, off_by_default_and_with_zero) {
    resetEnv();
    {
        Gs g(nullptr);
        CHECK(!g.texCacheEnabled());
        CHECK(!g.texCacheReport().enabled);
    }
    setEnv("ANYPS2_GS_TEXCACHE", "0");
    {
        Gs g(nullptr);
        CHECK(!g.texCacheEnabled());
    }
    setEnv("ANYPS2_GS_TEXCACHE", "1");
    {
        Gs g(nullptr);
        CHECK(g.texCacheEnabled());
        CHECK_EQ(g.texCacheReport().budget, std::uint64_t{256} << 20);
    }
    setEnv("ANYPS2_GS_TEXCACHE", "");
    setEnv("ANYPS2_GS_TEXCACHE_VERIFY", "1");  // VERIFY liga o cache
    {
        Gs g(nullptr);
        CHECK(g.texCacheEnabled());
    }
    resetEnv();
}

TEST_CASE(gs_texcache, scene_host_over_texture) { runScene("host", sceneHostOverTexture, 4); }
TEST_CASE(gs_texcache, scene_local_to_local_over_texture) { runScene("local", sceneLocalToLocalOverTexture, 4); }
TEST_CASE(gs_texcache, scene_render_to_texture) { runScene("rtt", sceneRenderToTexture, 4); }
TEST_CASE(gs_texcache, scene_texture_on_own_frame) { runScene("selftex", sceneTextureOnOwnFrame, 2); }
TEST_CASE(gs_texcache, scene_zbuf_on_texture_page) { runScene("zbuf", sceneZbufOnTexturePage, 2); }
TEST_CASE(gs_texcache, scene_clut_and_texa_changes) { runScene("clut", sceneClutAndTexa, 2); }
TEST_CASE(gs_texcache, scene_partial_host_transfer_with_draws_between_words) { runScene("partial", scenePartialHost, 5); }
TEST_CASE(gs_texcache, scene_texture_wrapping_around_vram_end) { runScene("wrap", sceneWrapAroundVramEnd, 4); }
TEST_CASE(gs_texcache, scene_region_clamp_and_repeat_reach) { runScene("region", sceneRegionModes, 2); }
TEST_CASE(gs_texcache, scene_mipmap_level_invalidation) { runScene("mip", sceneMipmap, 4); }

TEST_CASE(gs_texcache, stress_many_small_draws_four_lanes_repeated) {
    // O mesmo programa várias vezes: o agendamento das faixas muda de uma execução para a
    // outra, e qualquer corrida no preenchimento dos blocos dá uma diferença.
    const Out off = render(sceneManySmallDraws, 0x5EEDull, 4, 0);
    for (unsigned rep = 0; rep < 12; ++rep) {
        const Out on = render(sceneManySmallDraws, 0x5EEDull, 4, rep % 2 ? 1 : 2);
        CHECK_EQ(on.mismatches, 0u);
        CHECK(on.vram == off.vram);
        CHECK(on.rep.versions >= 2);
    }
    const Out solo = render(sceneManySmallDraws, 0x5EEDull, 1, 2);
    CHECK_EQ(solo.mismatches, 0u);
    CHECK(solo.vram == off.vram);
}

TEST_CASE(gs_texcache, verify_detects_a_write_behind_the_caches_back) {
    // Quebra de propósito o invariante (escrita na VRAM sem passar pelas gerações): o
    // VERIFY tem de pegar a leitura velha. Síncrono, para ser determinístico.
    setEnv("ANYPS2_GS_THREAD", "0");
    setEnv("ANYPS2_GS_TEXCACHE_VERIFY", "1");
    {
        Scene s(7);
        const Tex a = texA();
        s.uploadTex(a);
        drawAround(s, a, 0);
        CHECK_EQ(s.g.texVerifyMismatches(), 0u);
        auto* raw = const_cast<std::uint8_t*>(static_cast<const Gs&>(s.g).vram().data());
        for (std::uint32_t i = 0; i < 64 * 256; ++i) raw[std::size_t{kTexA} * 256 + i] ^= 0xFF;
        drawAround(s, a, 32);
        CHECK(s.g.texVerifyMismatches() > 0u);
    }
    resetEnv();
    // Pela API normal (Gs::vram() não const), a escrita conta e a versão é refeita.
    setEnv("ANYPS2_GS_THREAD", "0");
    setEnv("ANYPS2_GS_TEXCACHE_VERIFY", "1");
    {
        Scene s(7);
        const Tex a = texA();
        s.uploadTex(a);
        drawAround(s, a, 0);
        std::uint8_t* raw = s.g.vram().data();
        for (std::uint32_t i = 0; i < 64 * 256; ++i) raw[std::size_t{kTexA} * 256 + i] ^= 0xFF;
        s.g.vram();  // o acesso é que conta; este só repete
        drawAround(s, a, 32);
        CHECK_EQ(s.g.texVerifyMismatches(), 0u);
    }
    resetEnv();
}

namespace {

void fillRandom(Vram& v, Lcg& r) {
    std::uint8_t* d = v.data();
    for (std::uint32_t i = 0; i < Vram::kSize; ++i) d[i] = static_cast<std::uint8_t>(r.next(256));
}

}  // namespace

TEST_CASE(gs_texcache, level_blocks_filled_concurrently_match_the_vram_for_every_format) {
    Vram vram;
    Lcg rnd{0xB10C5ull};
    fillRandom(vram, rnd);
    const std::uint32_t psms[] = {PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8,   PSMT4,  PSMT8H,
                                  PSMT4HL, PSMT4HH, PSMZ32,  PSMZ24,   PSMZ16, PSMZ16S};
    for (std::uint32_t psm : psms) {
        for (unsigned rep = 0; rep < 4; ++rep) {
            const unsigned w = 37 + rnd.next(60), h = 29 + rnd.next(70);
            const std::uint32_t bp = rnd.next(Vram::kBlocks);
            TexLevel lv(psm, bp, 2, w, h, nullptr);
            std::atomic<unsigned> bad{0}, filled{0};
            std::vector<std::thread> threads;
            for (unsigned t = 0; t < 4; ++t) {
                threads.emplace_back([&, t] {
                    // Cada thread varre a grade (e um pouco além dela) começando num ponto e numa
                    // direção diferentes; todas passam por todas as coordenadas.
                    const unsigned total = (w + 5) * (h + 5);
                    for (unsigned n = 0; n < total; ++n) {
                        const unsigned m = (t & 1) ? total - 1 - n : n;
                        const unsigned k = (m + (t >> 1) * (total / 3)) % total;
                        const unsigned x = k % (w + 5), y = k / (w + 5);
                        std::uint32_t raw = 0xDEADBEEFu;
                        const TexLevel::Got got = lv.get(vram, x, y, raw);
                        if (x >= w || y >= h) {
                            if (got != TexLevel::Got::Outside || raw != 0xDEADBEEFu) ++bad;
                            continue;
                        }
                        if (got == TexLevel::Got::Outside || raw != vram.readPixel(psm, bp, 2, x, y)) ++bad;
                        if (got == TexLevel::Got::Filled) ++filled;
                    }
                });
            }
            for (auto& t : threads) t.join();
            CHECK_EQ(bad.load(), 0u);
            // Cada bloco é preenchido por exatamente uma thread.
            const TexGeom g = texGeom(psm);
            const unsigned blocks = ((w + (1u << g.bwShift) - 1) >> g.bwShift) * ((h + (1u << g.bhShift) - 1) >> g.bhShift);
            CHECK_EQ(filled.load(), blocks);
        }
    }
}

TEST_CASE(gs_texcache, index_respects_budget_lru_and_keeps_versions_in_flight_alive) {
    PageSet none;
    TexLevel probe(PSMCT32, 0, 1, 64, 64, nullptr);
    const std::size_t each = probe.bytes();
    TexCache c(each * 3 + each / 2);  // cabem 3
    auto key = [](std::uint32_t bp) { return TexKey{PSMCT32, bp, 1, 64, 64}; };
    std::shared_ptr<TexLevel> held;
    for (std::uint32_t i = 0; i < 6; ++i) {
        TexCache::Entry& e = c.create(key(i * 64), none, {}, 1);
        if (i == 0) held = e.level;  // um desenho em voo fixou a versão mais velha
        CHECK(c.stats().bytes <= each * 3);
    }
    CHECK_EQ(c.stats().versions, 6u);
    CHECK_EQ(c.stats().evictions, 3u);
    CHECK_EQ(c.stats().entries, 3u);
    CHECK(c.find(key(0)) == nullptr);  // despejada do índice
    CHECK(c.find(key(5 * 64)) != nullptr);
    // A versão fixada continua inteira e contada como viva.
    CHECK_EQ(c.liveBytes(), static_cast<std::uint64_t>(each * 4));
    Vram vram;
    std::uint32_t raw = 0;
    CHECK(held->get(vram, 3, 3, raw) != TexLevel::Got::Outside);
    held.reset();
    CHECK_EQ(c.liveBytes(), static_cast<std::uint64_t>(each * 3));
    CHECK(c.peakLiveBytes() >= static_cast<std::uint64_t>(each * 4));
    // LRU: pedir a mais antiga a salva do próximo despejo.
    CHECK(c.find(key(3 * 64)) != nullptr);
    c.create(key(9 * 64), none, {}, 2);
    CHECK(c.find(key(3 * 64)) != nullptr);
    CHECK(c.find(key(4 * 64)) == nullptr);
    // Uma versão maior que o orçamento inteiro fica (a mais recente nunca é despejada).
    TexCache tiny(16);
    tiny.create(key(0), none, {}, 1);
    CHECK_EQ(tiny.stats().entries, 1u);
    CHECK(tiny.find(key(0)) != nullptr);
}
