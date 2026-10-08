/*
 * gfx2d: primitivas 2D do GS pelo caminho clássico do ps2sdk (libgraph +
 * libdraw + libdma + libpacket): limpar a tela, sprites, triângulos com
 * Gouraud, linhas, pontos, blending e um sprite com textura PSMT8 + CLUT,
 * enviados pelo DMA do GIF em modo normal e chain. O teste compara a saída
 * de texto e o hash da imagem exibida no fim.
 */
#include <dma.h>
#include <dma_tags.h>
#include <draw.h>
#include <gif_tags.h>
#include <graph.h>
#include <gs_gp.h>
#include <gs_psm.h>
#include <kernel.h>
#include <packet.h>
#include <stdio.h>
#include <tamtypes.h>

#define W 640
#define H 448
#define OFS 2048

static u8 texture[64 * 64] __attribute__((aligned(16)));
static u32 clut[256] __attribute__((aligned(16)));

static u64 xy(int x, int y) {
    return GS_SET_XYZ((x + OFS) << 4, (y + OFS) << 4, 0);
}

static void send(packet_t *p, qword_t *q) {
    q = draw_finish(q);
    dma_channel_send_normal(DMA_CHANNEL_GIF, p->data, q - p->data, 0, 0);
    dma_wait_fast();
    draw_wait_finish();
}

/* GIFtag A+D com n registradores */
static qword_t *ad(qword_t *q, int n) {
    PACK_GIFTAG(q, GIF_SET_TAG(n, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    return q + 1;
}

static qword_t *reg(qword_t *q, u64 value, int r) {
    PACK_GIFTAG(q, value, r);
    return q + 1;
}

static void make_texture(void) {
    int x, y, i;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            texture[y * 64 + x] = (u8)(((x / 8 + y / 8) & 1) ? (x * 4) : (255 - y * 4));
    /* Paleta 256 cores; na VRAM (CSM1) a entrada e fica na posição com os
     * bits 3 e 4 trocados. */
    for (i = 0; i < 256; i++) {
        int e = (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
        u32 r = (u32)e, g = (u32)((e * 3) & 0xFF), b = (u32)(255 - e);
        clut[i] = r | (g << 8) | (b << 16) | (0x80u << 24);
    }
}

int main(void) {
    framebuffer_t frame;
    zbuffer_t z;
    packet_t *packet = packet_init(200, PACKET_NORMAL);
    packet_t *chain = packet_init(64, PACKET_NORMAL);
    qword_t *q;
    int i, tex, cl;

    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    frame.width = W;
    frame.height = H;
    frame.mask = 0;
    frame.psm = GS_PSM_32;
    frame.address = graph_vram_allocate(W, H, GS_PSM_32, GRAPH_ALIGN_PAGE);
    z.enable = 0;
    z.address = 0;
    z.mask = 1;
    z.zsm = 0;
    z.method = ZTEST_METHOD_ALLPASS;
    graph_initialize(frame.address, W, H, GS_PSM_32, 0, 0);
    printf("gfx2d: framebuffer em %d, regiao %s\n", frame.address,
           graph_get_region() == GRAPH_MODE_NTSC ? "NTSC" : "PAL");

    q = draw_setup_environment(packet->data, 0, &frame, &z);
    send(packet, q);

    /* Fundo */
    q = draw_clear(packet->data, 0, 0, 0, W, H, 16, 24, 64);
    send(packet, q);

    /* 8 sprites de cores sólidas */
    q = packet->data;
    q = ad(q, 1 + 8 * 3);
    q = reg(q, GS_SET_PRIM(GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0), GS_REG_PRIM);
    for (i = 0; i < 8; i++) {
        q = reg(q, GS_SET_RGBAQ((i & 1) ? 255 : 40, (i & 2) ? 255 : 40, (i & 4) ? 255 : 40, 0x80, 0x3F800000),
                GS_REG_RGBAQ);
        q = reg(q, xy(20 + i * 75, 20), GS_REG_XYZ2);
        q = reg(q, xy(20 + i * 75 + 60, 80), GS_REG_XYZ2);
    }
    send(packet, q);

    /* Triângulo com Gouraud */
    q = packet->data;
    q = ad(q, 7);
    q = reg(q, GS_SET_PRIM(GS_PRIM_TRIANGLE, 1, 0, 0, 0, 0, 0, 0, 0), GS_REG_PRIM);
    q = reg(q, GS_SET_RGBAQ(255, 0, 0, 0x80, 0x3F800000), GS_REG_RGBAQ);
    q = reg(q, xy(60, 400), GS_REG_XYZ2);
    q = reg(q, GS_SET_RGBAQ(0, 255, 0, 0x80, 0x3F800000), GS_REG_RGBAQ);
    q = reg(q, xy(180, 110), GS_REG_XYZ2);
    q = reg(q, GS_SET_RGBAQ(0, 0, 255, 0x80, 0x3F800000), GS_REG_RGBAQ);
    q = reg(q, xy(300, 400), GS_REG_XYZ2);
    send(packet, q);

    /* Leque (triangle fan) formando um hexágono, cores alternadas */
    q = packet->data;
    q = ad(q, 1 + 2 * 8);
    q = reg(q, GS_SET_PRIM(GS_PRIM_TRIANGLE_FAN, 1, 0, 0, 0, 0, 0, 0, 0), GS_REG_PRIM);
    {
        static const int hx[8] = {0, 60, 30, -30, -60, -30, 30, 60};
        static const int hy[8] = {0, 0, 52, 52, 0, -52, -52, 0};
        for (i = 0; i < 8; i++) {
            q = reg(q, GS_SET_RGBAQ(i == 0 ? 255 : 0, 255, i & 1 ? 255 : 64, 0x80, 0x3F800000), GS_REG_RGBAQ);
            q = reg(q, xy(540 + hx[i], 330 + hy[i]), GS_REG_XYZ2);
        }
    }
    send(packet, q);

    /* Linhas (line strip) e pontos */
    q = packet->data;
    q = ad(q, 2 + 6 + 1 + 10);
    q = reg(q, GS_SET_PRIM(GS_PRIM_LINE_STRIP, 0, 0, 0, 0, 0, 0, 0, 0), GS_REG_PRIM);
    q = reg(q, GS_SET_RGBAQ(255, 255, 0, 0x80, 0x3F800000), GS_REG_RGBAQ);
    q = reg(q, xy(320, 120), GS_REG_XYZ2);
    q = reg(q, xy(620, 120), GS_REG_XYZ2);
    q = reg(q, xy(620, 230), GS_REG_XYZ2);
    q = reg(q, xy(320, 230), GS_REG_XYZ2);
    q = reg(q, xy(320, 120), GS_REG_XYZ2);
    q = reg(q, xy(620, 230), GS_REG_XYZ2);
    q = reg(q, GS_SET_PRIM(GS_PRIM_POINT, 0, 0, 0, 0, 0, 0, 0, 0), GS_REG_PRIM);
    for (i = 0; i < 10; i++) q = reg(q, xy(330 + i * 4, 240), GS_REG_XYZ2);
    send(packet, q);

    /* Sprite translúcido: (Cs - Cd) * As + Cd com As = 0x40 (50%) */
    q = packet->data;
    q = ad(q, 5);
    q = reg(q, GS_SET_ALPHA(0, 1, 0, 1, 0), GS_REG_ALPHA);
    q = reg(q, GS_SET_PRIM(GS_PRIM_SPRITE, 0, 0, 0, 1, 0, 0, 0, 0), GS_REG_PRIM);
    q = reg(q, GS_SET_RGBAQ(255, 255, 255, 0x40, 0x3F800000), GS_REG_RGBAQ);
    q = reg(q, xy(100, 50), GS_REG_XYZ2);
    q = reg(q, xy(400, 160), GS_REG_XYZ2);
    send(packet, q);

    /* Textura PSMT8 64x64 + CLUT de 256 cores, enviadas em chain */
    make_texture();
    tex = graph_vram_allocate(64, 64, GS_PSM_8, GRAPH_ALIGN_BLOCK);
    cl = graph_vram_allocate(16, 16, GS_PSM_32, GRAPH_ALIGN_BLOCK);
    q = chain->data;
    q = draw_texture_transfer(q, texture, 64, 64, GS_PSM_8, tex, 64);
    q = draw_texture_transfer(q, clut, 16, 16, GS_PSM_32, cl, 64);
    q = draw_texture_flush(q);
    dma_channel_send_chain(DMA_CHANNEL_GIF, chain->data, q - chain->data, 0, 0);
    dma_wait_fast();

    q = packet->data;
    q = ad(q, 7);
    q = reg(q, GS_SET_TEX0(tex >> 6, 1, GS_PSM_8, 6, 6, 1, 1, cl >> 6, GS_PSM_32, 0, 0, 1), GS_REG_TEX0);
    q = reg(q, GS_SET_TEX1(0, 0, 0, 0, 0, 0, 0), GS_REG_TEX1);
    q = reg(q, GS_SET_PRIM(GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0), GS_REG_PRIM);
    q = reg(q, GS_SET_UV(8, 8), GS_REG_UV);
    q = reg(q, xy(360, 260), GS_REG_XYZ2);
    q = reg(q, GS_SET_UV(64 * 16 + 8, 64 * 16 + 8), GS_REG_UV);
    q = reg(q, xy(360 + 128, 260 + 128), GS_REG_XYZ2);
    send(packet, q);

    graph_wait_vsync();
    graph_wait_vsync();
    printf("gfx2d: %d quadros, ok\n", 2);
    return 0;
}
