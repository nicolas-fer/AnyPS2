/*
 * vu1draw: geometria processada pelo VU1.
 * O EE envia pelo VIF1 (DMA): o microprograma (MPG), as configurações do
 * double buffering (BASE/OFFSET) e, a cada quadro, matriz + vértices
 * (UNPACK com TOPS) seguidos de MSCAL. O microprograma (draw.vsm)
 * transforma, divide pela perspectiva, converte para ponto fixo e manda os
 * triângulos ao GS com XGKICK (PATH1).
 */
#include <dma.h>
#include <draw.h>
#include <graph.h>
#include <gs_psm.h>
#include <kernel.h>
#include <math.h>
#include <packet.h>
#include <stdio.h>
#include <string.h>
#include <tamtypes.h>

extern u32 vu1_draw_start __attribute__((section(".vudata")));
extern u32 vu1_draw_end __attribute__((section(".vudata")));

#define NVERT 36

static u32 vifbuf[4 * 1024] __attribute__((aligned(16)));
static int nw;

static void w32(u32 v) { vifbuf[nw++] = v; }
static void wf(float f) {
    union { float f; u32 u; } x;
    x.f = f;
    w32(x.u);
}
static void align_qw(void) {
    while (nw & 3) w32(0); /* VIF NOP */
}
static void send_vif(void) {
    align_qw();
    dma_channel_send_normal(DMA_CHANNEL_VIF1, vifbuf, nw / 4, 0, 0);
    dma_channel_wait(DMA_CHANNEL_VIF1, 0);
    nw = 0;
}

static const float cube[8][3] = {
    {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1},
};
static const int tris[36] = {
    0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1, 3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2,
};
static const float face_color[6][3] = {
    {255, 64, 64}, {64, 255, 64}, {64, 64, 255}, {255, 255, 64}, {64, 255, 255}, {255, 64, 255},
};

static void upload_program(void) {
    const u32 *code = &vu1_draw_start;
    const int words = (int)(&vu1_draw_end - &vu1_draw_start);
    int i;
    nw = 0;
    w32(0);
    w32(0);
    w32(0);
    w32(0x4A000000u | ((u32)(words / 2) << 16) | 0); /* MPG num, endereço 0 */
    for (i = 0; i < words; i++) w32(code[i]);
    w32(0x01000101u); /* STCYCL 1,1 */
    w32(0x03000000u); /* BASE 0 */
    w32(0x02000000u | 400); /* OFFSET 400 */
    send_vif();
    printf("vu1draw: microprograma de %d instruções\n", words / 2);
}

static void frame_packet(float ax, float ay) {
    float cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay);
    /* R = Ry * Rx (vetor-linha), câmera em z = +6, f = 1,6 */
    float r[3][3] = {
        {cy, sx * sy, -cx * sy},
        {0, cx, sx},
        {sy, -sx * cy, cx * cy},
    };
    const float f = 1.6f, dist = 6.0f, K = 16777215.0f * 4.0f;
    int i, row;
    nw = 0;
    w32(0x01000101u);
    /* UNPACK V4-32, FLG (+TOPS), endereço 0, 7 + 2*NVERT quadwords */
    w32(0x6C000000u | ((u32)(7 + 2 * NVERT) << 16) | 0x8000u);
    for (row = 0; row < 3; row++) {
        wf(r[row][0] * f);
        wf(r[row][1] * f);
        wf(0);
        wf(r[row][2]);
    }
    wf(0);
    wf(0);
    wf(K);
    wf(dist);
    wf(320.0f); /* escala */
    wf(-320.0f);
    wf(1.0f);
    w32(NVERT);
    wf(2048.0f + 320.0f); /* deslocamento: XYOFFSET da libdraw é 2048; centro da tela */
    wf(2048.0f + 224.0f);
    wf(0.0f);
    wf(0.0f);
    /* GIFtag: NLOOP=NVERT EOP PRE PRIM=triângulo Gouraud, PACKED, NREG=2 (RGBAQ, XYZ2) */
    w32((u32)NVERT | 0x8000u);
    w32((1u << 14) | ((3u | 8u) << 15) | (2u << 28));
    w32(0x51);
    w32(0);
    for (i = 0; i < NVERT; i++) {
        const float *p = cube[tris[i]];
        const float *c = face_color[i / 6];
        wf(p[0]);
        wf(p[1]);
        wf(p[2]);
        wf(1.0f);
        wf(c[0]);
        wf(c[1]);
        wf(c[2]);
        wf(128.0f);
    }
    w32(0x14000000u); /* MSCAL 0 */
    w32(0x11000000u); /* FLUSH */
    send_vif();
}

int main(void) {
    framebuffer_t frame;
    zbuffer_t z;
    packet_t *packet = packet_init(64, PACKET_NORMAL);
    qword_t *q;
    int f;
    u32 *vu1mem = (u32 *)0x1100C000;

    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_initialize(DMA_CHANNEL_VIF1, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    frame.width = 640;
    frame.height = 448;
    frame.mask = 0;
    frame.psm = GS_PSM_32;
    frame.address = graph_vram_allocate(640, 448, GS_PSM_32, GRAPH_ALIGN_PAGE);
    z.enable = DRAW_ENABLE;
    z.mask = 0;
    z.method = ZTEST_METHOD_GREATER_EQUAL;
    z.zsm = GS_ZBUF_24;
    z.address = graph_vram_allocate(640, 448, GS_ZBUF_24, GRAPH_ALIGN_PAGE);
    graph_initialize(frame.address, 640, 448, GS_PSM_32, 0, 0);

    upload_program();
    for (f = 0; f < 3; f++) {
        q = draw_setup_environment(packet->data, 0, &frame, &z);
        q = draw_disable_tests(q, 0, &z);
        q = draw_clear(q, 0, 0, 0, 640, 448, 0x20, 0x18, 0x10);
        q = draw_enable_tests(q, 0, &z);
        q = draw_finish(q);
        dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
        dma_wait_fast();
        draw_wait_finish();
        frame_packet(0.5f + 0.2f * f, 0.6f + 0.3f * f);
        graph_wait_vsync();
    }
    /* Primeiro vértice de saída do último quadro: buffer em TOP = 400 (3º MSCAL), saída em +7+72 */
    {
        const u32 *out = vu1mem + (400 + 7 + 2 * NVERT) * 4;
        printf("giftag nloop=%u eop=%u\n", out[0] & 0x7FFF, (out[0] >> 15) & 1);
        printf("v0 rgba=(%u,%u,%u,%u) xy=(%d,%d)\n", out[4], out[5], out[6], out[7],
               ((int)out[8] - 0x8000) / 16, ((int)out[9] - 0x8000) / 16);
    }
    printf("vu1draw: %d quadros\n", f);
    return 0;
}
