/*
 * cube3d: cubo 3D girando, transformado na CPU (FPU do EE), com Z-buffer
 * (PSMZ24, GEQUAL), textura PSMCT32 com correção de perspectiva (STQ),
 * filtro bilinear, iluminação por face (Gouraud + MODULATE) e double
 * buffering. A matemática é toda em C: libmath3d usa macroinstruções do
 * VU0, que chegam na Fase 5.
 */
#include <dma.h>
#include <dma_tags.h>
#include <draw.h>
#include <gif_tags.h>
#include <graph.h>
#include <gs_gp.h>
#include <gs_psm.h>
#include <kernel.h>
#include <math.h>
#include <packet.h>
#include <stdio.h>
#include <tamtypes.h>

#define W 640
#define H 448
#define OFS 2048
#define FRAMES 4

static u32 texture[32 * 32] __attribute__((aligned(16)));

static const float verts[8][3] = {
    {-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
    {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1},
};
/* Faces em sentido horário vistas de fora */
static const int faces[6][4] = {
    {0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {3, 7, 6, 2}, {0, 4, 7, 3}, {1, 2, 6, 5},
};
static const float normals[6][3] = {
    {0, 0, -1}, {0, 0, 1}, {0, -1, 0}, {0, 1, 0}, {-1, 0, 0}, {1, 0, 0},
};
static const u8 faceColor[6][3] = {
    {255, 255, 255}, {255, 160, 160}, {160, 255, 160}, {160, 160, 255}, {255, 255, 140}, {140, 255, 255},
};

typedef union {
    float f;
    u32 u;
} fu;

static void make_texture(void) {
    int x, y;
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            u32 c;
            if (x < 2 || y < 2 || x > 29 || y > 29) c = 0x00202020;
            else if (((x / 4) + (y / 4)) & 1) c = 0x00F0F0F0;
            else c = 0x00B04010 + (u32)(x * 4) * 0x100;
            texture[y * 32 + x] = c | 0x80000000u;
        }
    }
}

static void rotate(const float *in, float ax, float ay, float *out) {
    float cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay);
    float x = in[0], y = in[1], z = in[2];
    /* Y depois X */
    float x1 = x * cy + z * sy;
    float z1 = -x * sy + z * cy;
    float y2 = y * cx - z1 * sx;
    float z2 = y * sx + z1 * cx;
    out[0] = x1;
    out[1] = y2;
    out[2] = z2;
}

static qword_t *ad(qword_t *q, int n) {
    PACK_GIFTAG(q, GIF_SET_TAG(n, 0, 0, 0, GIF_FLG_PACKED, 1), GIF_REG_AD);
    return q + 1;
}
static qword_t *reg(qword_t *q, u64 value, int r) {
    PACK_GIFTAG(q, value, r);
    return q + 1;
}

static void send(packet_t *p, qword_t *q) {
    q = draw_finish(q);
    dma_channel_send_normal(DMA_CHANNEL_GIF, p->data, q - p->data, 0, 0);
    dma_wait_fast();
    draw_wait_finish();
}

int main(void) {
    framebuffer_t fb[2];
    zbuffer_t z;
    packet_t *packet = packet_init(600, PACKET_NORMAL);
    packet_t *chain = packet_init(64, PACKET_NORMAL);
    qword_t *q;
    int i, f, frame, tex, drawn = 0;
    const float light[3] = {0.3f, 0.5f, -0.81f};

    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);

    for (i = 0; i < 2; i++) {
        fb[i].width = W;
        fb[i].height = H;
        fb[i].mask = 0;
        fb[i].psm = GS_PSM_32;
        fb[i].address = graph_vram_allocate(W, H, GS_PSM_32, GRAPH_ALIGN_PAGE);
    }
    z.enable = DRAW_ENABLE;
    z.mask = 0;
    z.method = ZTEST_METHOD_GREATER_EQUAL;
    z.zsm = GS_ZBUF_24;
    z.address = graph_vram_allocate(W, H, GS_ZBUF_24, GRAPH_ALIGN_PAGE);
    graph_initialize(fb[0].address, W, H, GS_PSM_32, 0, 0);

    make_texture();
    tex = graph_vram_allocate(32, 32, GS_PSM_32, GRAPH_ALIGN_BLOCK);
    q = draw_texture_transfer(chain->data, texture, 32, 32, GS_PSM_32, tex, 64);
    q = draw_texture_flush(q);
    dma_channel_send_chain(DMA_CHANNEL_GIF, chain->data, q - chain->data, 0, 0);
    dma_wait_fast();

    for (frame = 0; frame < FRAMES; frame++) {
        framebuffer_t *back = &fb[(frame + 1) & 1];
        float ax = 0.5f + 0.15f * (float)frame, ay = 0.7f + 0.2f * (float)frame;
        float pv[8][3], sv[8][3];

        q = draw_setup_environment(packet->data, 0, back, &z);
        /* Limpa cor e Z (Z=0 com teste ALWAYS) */
        q = ad(q, 1);
        q = reg(q, GS_SET_TEST(1, 7, 0, 0, 0, 0, 1, 1), GS_REG_TEST);
        q = draw_clear(q, 0, 0, 0, W, H, 30, 30, 50);
        q = ad(q, 3);
        q = reg(q, GS_SET_TEST(1, 7, 0, 0, 0, 0, 1, 2), GS_REG_TEST);
        q = reg(q, GS_SET_TEX0(tex >> 6, 1, GS_PSM_32, 5, 5, 0, 0, 0, 0, 0, 0, 0), GS_REG_TEX0);
        q = reg(q, GS_SET_TEX1(0, 0, 1, 1, 0, 0, 0), GS_REG_TEX1);

        for (i = 0; i < 8; i++) {
            rotate(verts[i], ax, ay, pv[i]);
            pv[i][2] += 4.0f;
            sv[i][2] = 1.0f / pv[i][2];
            sv[i][0] = 320.0f + pv[i][0] * sv[i][2] * 420.0f;
            sv[i][1] = 224.0f - pv[i][1] * sv[i][2] * 420.0f;
        }
        for (f = 0; f < 6; f++) {
            static const float uv[4][2] = {{0, 0}, {0, 1}, {1, 1}, {1, 0}};
            static const int tri[6] = {0, 1, 2, 0, 2, 3};
            const int *v = faces[f];
            float n[3], lum;
            float ex = sv[v[1]][0] - sv[v[0]][0], ey = sv[v[1]][1] - sv[v[0]][1];
            float fx = sv[v[2]][0] - sv[v[0]][0], fy = sv[v[2]][1] - sv[v[0]][1];
            int k;
            if (ex * fy - ey * fx <= 0.0f) continue; /* face de costas (Y da tela cresce para baixo) */
            rotate(normals[f], ax, ay, n);
            lum = n[0] * light[0] + n[1] * light[1] + n[2] * light[2];
            if (lum < 0.15f) lum = 0.15f;
            q = ad(q, 1 + 6 * 3);
            q = reg(q, GS_SET_PRIM(GS_PRIM_TRIANGLE, 1, 1, 0, 0, 0, 0, 0, 0), GS_REG_PRIM);
            for (k = 0; k < 6; k++) {
                int vi = v[tri[k]];
                fu qq, s, t;
                u32 r = (u32)(faceColor[f][0] * lum) >> 1, g = (u32)(faceColor[f][1] * lum) >> 1,
                    b = (u32)(faceColor[f][2] * lum) >> 1;
                qq.f = sv[vi][2];
                s.f = uv[tri[k]][0] * qq.f;
                t.f = uv[tri[k]][1] * qq.f;
                q = reg(q, GS_SET_ST(s.u, t.u), GS_REG_ST);
                q = reg(q, GS_SET_RGBAQ(r, g, b, 0x80, qq.u), GS_REG_RGBAQ);
                q = reg(q,
                        GS_SET_XYZ((int)((sv[vi][0] + OFS) * 16.0f), (int)((sv[vi][1] + OFS) * 16.0f),
                                   (u32)(sv[vi][2] * 2.0f * 16777215.0f)),
                        GS_REG_XYZ2);
            }
            drawn++;
        }
        send(packet, q);
        graph_wait_vsync();
        graph_set_framebuffer_filtered(back->address, W, GS_PSM_32, 0, 0);
    }
    graph_wait_vsync();
    printf("cube3d: %d quadros, %d faces desenhadas\n", FRAMES, drawn);
    return 0;
}
