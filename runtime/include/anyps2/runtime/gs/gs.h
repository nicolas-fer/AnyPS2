#pragma once

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "anyps2/runtime/gs/vram.h"

namespace anyps2::rt {
class Runtime;
}

namespace anyps2::rt::gs {

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

// Graphics Synthesizer em software: estado completo dos registradores,
// memória local, transferências e um rasterizador de referência que segue as
// regras do hardware (cobertura top-left, Z de 32 bits inteiro, blending
// (A−B)·C/128+D, texturas com CLUT/TEXA etc.).
class Gs {
public:
    explicit Gs(Runtime* rt);

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
    std::uint64_t reg(std::uint8_t r) const { return regs_[r]; }

    // ---- Eventos de vídeo ----------------------------------------------------
    void vblankStart();
    void vblankEnd();
    // Linha HSYNC (o HSINT é calculado sob demanda a partir do relógio).
    void setHsyncSource(std::uint64_t (*now)(void*), void* ctx, std::uint64_t cyclesPerLine);
    // Eventos de desenho atrasados (FINISH): o GS processa os dados na hora,
    // mas o evento só aparece depois do tempo estimado de trabalho do GS
    // (transferência pelo GIF + preenchimento de pixels). Sem relógio
    // (testes do GS isolado) o evento é imediato.
    std::uint64_t nextEventTime() const;  // ciclo do EE, ~0 se nenhum
    void processEvents(std::uint64_t now);

    // Compõe a imagem exibida (circuitos de leitura 1 e 2, PMODE, BGCOLOR).
    Frame display() const;
    bool displayEnabled() const;

    Vram& vram() { return vram_; }
    const Vram& vram() const { return vram_; }
    std::uint64_t drawCount() const { return drawCount_; }

private:
    struct Context {
        std::uint64_t tex0 = 0, tex1 = 0, clamp = 0, xyoffset = 0, miptbp1 = 0, miptbp2 = 0;
        std::uint64_t scissor = 0, alpha = 0, test = 0, fba = 0, frame = 0, zbuf = 0;
    };

    void raiseEvent(unsigned bit);
    void addWork(std::uint64_t cycles);
    void vertexKick(bool draw, std::uint32_t pc);
    void draw(std::uint32_t pc);
    void writeTex0(unsigned ctx, std::uint64_t value, std::uint32_t pc);
    void loadClut(std::uint64_t tex0, std::uint32_t pc);
    void startTransfer(std::uint32_t pc);
    void localToLocal(std::uint32_t pc);
    [[noreturn]] void unsupported(const std::string& what, std::uint32_t pc) const;
    void warnOnce(const std::string& what);

    // Rasterização (gs_draw.cpp)
    struct DrawEnv;
    void setupEnv(DrawEnv& e, std::uint32_t pc);
    void drawPoint(const DrawEnv& e, const Vertex& v);
    void drawLine(const DrawEnv& e, const Vertex& a, const Vertex& b);
    void drawTriangle(const DrawEnv& e, const Vertex& a, const Vertex& b, const Vertex& c);
    void drawSprite(const DrawEnv& e, const Vertex& a, const Vertex& b);
    struct Fragment;
    void shadePixel(const DrawEnv& e, int x, int y, Fragment& f);
    std::uint32_t sampleTexture(const DrawEnv& e, float u, float v, float lod) const;
    std::uint32_t fetchTexel(const DrawEnv& e, unsigned level, int u, int v) const;

    Runtime* rt_;
    Vram vram_;
    std::array<std::uint64_t, 256> regs_{};
    Context ctx_[2];
    std::array<std::uint16_t, 512> clut_{};  // buffer de CLUT interno (1 KB)
    std::uint32_t cbp0_ = 0, cbp1_ = 0;

    // Fila de vértices
    Vertex current_;
    Vertex queue_[3];
    unsigned queued_ = 0;
    unsigned fanFirst_ = 0;

    // Transferência HOST→LOCAL em andamento
    struct Transfer {
        bool active = false;
        std::uint32_t dbp = 0, dbw = 0, psm = 0;
        std::uint32_t x0 = 0, y0 = 0, w = 0, h = 0;
        std::uint32_t x = 0, y = 0;
        std::uint64_t bits = 0;  // dados de 24 bits/4 bits pendentes
        unsigned nbits = 0;
    } xfer_;

    // Registradores privilegiados
    std::array<std::uint64_t, 16> priv_{};  // 0x1200_0000 + i*0x10 (PMODE..BGCOLOR)
    std::uint64_t csr_ = 0;
    std::uint64_t imr_ = 0x7F00;
    std::uint64_t siglblid_ = 0;
    std::uint64_t (*hsyncNow_)(void*) = nullptr;
    void* hsyncCtx_ = nullptr;
    std::uint64_t cyclesPerLine_ = 0;
    std::uint64_t hsyncClearedAt_ = 0;

    // Modelo de tempo do GS (em ciclos do EE)
    std::uint64_t work_ = 0;        // trabalho acumulado desde o último evento
    std::uint64_t pixels_ = 0;      // pixels desenhados desde o último evento
    std::uint64_t busyUntil_ = 0;   // fim do trabalho já agendado
    std::uint64_t workStart_ = 0;   // chegada do primeiro dado do lote atual
    std::vector<std::uint64_t> finishDue_;  // FINISH pendentes (ordem crescente)

    std::uint64_t drawCount_ = 0;
    std::set<std::string> warned_;
};

}  // namespace anyps2::rt::gs
