/*
 * vu0math: o VU0 pelo EE.
 *  1. libmath3d (macroinstruções COP2: vmula/vmadd, vopmula/vopmsub,
 *     vrsqrt/vdiv com vwaitq...) com resultados conferidos à mão;
 *  2. instruções que a libmath3d não usa, em asm (conversões, max/mini,
 *     clip + cfc2, flags de status, inteiros, vmr32, vcallms);
 *  3. um microprograma do VU0 (micro.vsm) chamado com VCALLMS;
 *  4. um cubo girando, transformado por calculate_vertices (VU0) e
 *     desenhado com a libdraw.
 */
#include <dma.h>
#include <draw.h>
#include <draw3d.h>
#include <graph.h>
#include <gs_psm.h>
#include <kernel.h>
#include <math3d.h>
#include <packet.h>
#include <stdio.h>
#include <string.h>
#include <tamtypes.h>

extern u32 vu0_micro_start __attribute__((section(".vudata")));
extern u32 vu0_micro_end __attribute__((section(".vudata")));

static int r3(float x) { return (int)(x * 1000.0f + (x < 0 ? -0.5f : 0.5f)); }

static void pv(const char *name, VECTOR v) {
    printf("%s = (%d, %d, %d, %d)\n", name, r3(v[0]), r3(v[1]), r3(v[2]), r3(v[3]));
}

static void math3d_checks(void) {
    VECTOR a = {1, 0, 0, 1}, b = {0, 1, 0, 1}, out;
    VECTOR v345 = {3, 4, 0, 1};
    VECTOR p = {1, 2, 3, 1};
    MATRIX m, t;
    VECTOR move = {10, 20, 30, 1};
    vector_cross_product(out, a, b);
    pv("cross", out);
    vector_normalize(out, v345);
    pv("normalize", out);
    matrix_unit(m);
    matrix_translate(m, m, move);
    vector_apply(out, p, m);
    pv("translate", out);
    matrix_unit(t);
    t[0] = 2.0f;  /* escala x por 2 */
    t[5] = 0.5f;  /* escala y por 0,5 */
    matrix_multiply(m, t, m);
    vector_apply(out, p, m);
    pv("scale+translate", out);
}

static void asm_checks(void) {
    VECTOR in = {1.5f, -2.7f, 100.25f, -0.03f};
    VECTOR other = {-1.0f, -3.0f, 200.0f, 0.0f};
    int ints[4] __attribute__((aligned(16)));
    VECTOR f, mx, mn, rot;
    int clip, status, vi, q;
    __asm__ __volatile__(
        "lqc2      $vf1, 0(%[in])        \n"
        "lqc2      $vf2, 0(%[other])     \n"
        "vftoi4.xyzw $vf3, $vf1          \n"
        "sqc2      $vf3, 0(%[ints])      \n"
        "vitof4.xyzw $vf4, $vf3          \n"
        "sqc2      $vf4, 0(%[f])         \n"
        "vmax.xyzw $vf5, $vf1, $vf2      \n"
        "sqc2      $vf5, 0(%[mx])        \n"
        "vmini.xyzw $vf6, $vf1, $vf2     \n"
        "sqc2      $vf6, 0(%[mn])        \n"
        "vmr32.xyzw $vf7, $vf1           \n"
        "sqc2      $vf7, 0(%[rot])       \n"
        "ctc2      $zero, $vi18          \n"
        "vclipw.xyz $vf1, $vf0w          \n"
        "vnop                            \n"
        "cfc2      %[clip], $vi18        \n"
        "vdiv      $Q, $vf0w, $vf0x      \n" /* 1/0: flag D */
        "vwaitq                          \n"
        "cfc2      %[status], $vi16      \n"
        "viaddi    $vi1, $vi0, 7         \n"
        "viaddi    $vi2, $vi0, -3        \n"
        "viadd     $vi3, $vi1, $vi2      \n"
        "vmfir.xyzw $vf8, $vi3           \n"
        "vmtir     $vi4, $vf8x           \n"
        "cfc2      %[vi], $vi4           \n"
        "vdiv      $Q, $vf0w, $vf1x      \n" /* 1/1.5 */
        "vwaitq                          \n"
        "cfc2      %[q], $vi22           \n"
        : [clip] "=r"(clip), [status] "=r"(status), [vi] "=r"(vi), [q] "=r"(q)
        : [in] "r"(in), [other] "r"(other), [ints] "r"(ints), [f] "r"(f), [mx] "r"(mx), [mn] "r"(mn),
          [rot] "r"(rot)
        : "memory");
    printf("ftoi4 = (%d, %d, %d, %d)\n", ints[0], ints[1], ints[2], ints[3]);
    pv("itof4", f);
    pv("max", mx);
    pv("mini", mn);
    pv("mr32", rot);
    printf("clip = 0x%06x\n", clip);
    printf("status(1/0) D=%d DS=%d\n", (status >> 5) & 1, (status >> 11) & 1);
    printf("vi4 = %d\n", vi);
    printf("q(1/1.5) = 0x%08x\n", q);
}

static void micro_checks(void) {
    VECTOR a = {1, 2, 3, 4};
    VECTOR b = {0.5f, 0.5f, 0.5f, 0.5f};
    VECTOR s, sq, qv;
    int vi2;
    const u32 bytes = (u32)((u8 *)&vu0_micro_end - (u8 *)&vu0_micro_start);
    memcpy((void *)0x11000000, &vu0_micro_start, bytes); /* micro memória do VU0 */
    __asm__ __volatile__(
        "lqc2      $vf1, 0(%[a])     \n"
        "lqc2      $vf2, 0(%[b])     \n"
        "vcallms   0                 \n"
        "vnop                        \n"
        "sqc2      $vf3, 0(%[s])     \n"
        "sqc2      $vf4, 0(%[sq])    \n"
        "sqc2      $vf5, 0(%[qv])    \n"
        "cfc2      %[vi2], $vi2      \n"
        : [vi2] "=r"(vi2)
        : [a] "r"(a), [b] "r"(b), [s] "r"(s), [sq] "r"(sq), [qv] "r"(qv)
        : "memory");
    printf("micro: %u bytes, vi2 = %d\n", bytes, vi2);
    pv("micro add", s);
    pv("micro mul", sq);
    pv("micro q", qv);
}

/* Cubo: 8 vértices, 12 triângulos, cor por vértice */
static VECTOR cube_vertices[8] = {
    {-10, -10, -10, 1}, {10, -10, -10, 1}, {10, 10, -10, 1}, {-10, 10, -10, 1},
    {-10, -10, 10, 1},  {10, -10, 10, 1},  {10, 10, 10, 1},  {-10, 10, 10, 1},
};
static VECTOR cube_colors[8] = {
    {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 0, 1},
    {1, 0, 1, 1}, {0, 1, 1, 1}, {1, 1, 1, 1}, {0.3f, 0.3f, 0.3f, 1},
};
static const int cube_points[36] = {
    0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
    3, 2, 6, 3, 6, 7, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2,
};

static void render_cube(void) {
    framebuffer_t frame;
    zbuffer_t z;
    packet_t *packet = packet_init(200, PACKET_NORMAL);
    VECTOR object_position = {0, 0, 0, 1}, object_rotation = {0.4f, 0.6f, 0, 1};
    VECTOR camera_position = {0, 0, 60, 1}, camera_rotation = {0, 0, 0, 1};
    MATRIX local_world, world_view, view_screen, local_screen;
    VECTOR temp[8] __attribute__((aligned(16)));
    xyz_t verts[8] __attribute__((aligned(16)));
    color_t colors[8] __attribute__((aligned(16)));
    prim_t prim;
    color_t color;
    qword_t *q;
    int i, f;

    frame.width = 640;
    frame.height = 448;
    frame.mask = 0;
    frame.psm = GS_PSM_32;
    frame.address = graph_vram_allocate(640, 448, GS_PSM_32, GRAPH_ALIGN_PAGE);
    z.enable = DRAW_ENABLE;
    z.mask = 0;
    z.method = ZTEST_METHOD_GREATER_EQUAL;
    z.zsm = GS_ZBUF_32;
    z.address = graph_vram_allocate(640, 448, GS_ZBUF_32, GRAPH_ALIGN_PAGE);
    graph_initialize(frame.address, 640, 448, GS_PSM_32, 0, 0);

    q = draw_setup_environment(packet->data, 0, &frame, &z);
    q = draw_primitive_xyoffset(q, 0, 2048 - 320, 2048 - 224);
    q = draw_finish(q);
    dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
    dma_wait_fast();
    draw_wait_finish();

    prim.type = PRIM_TRIANGLE;
    prim.shading = PRIM_SHADE_GOURAUD;
    prim.mapping = DRAW_DISABLE;
    prim.fogging = DRAW_DISABLE;
    prim.blending = DRAW_DISABLE;
    prim.antialiasing = DRAW_DISABLE;
    prim.mapping_type = PRIM_MAP_ST;
    prim.colorfix = PRIM_UNFIXED;
    color.r = color.g = color.b = color.a = 0x80;
    color.q = 1.0f;
    create_view_screen(view_screen, graph_aspect_ratio(), -3.0f, 3.0f, -3.0f, 3.0f, 1.0f, 2000.0f);

    for (f = 0; f < 3; f++) {
        object_rotation[0] += 0.1f;
        object_rotation[1] += 0.15f;
        create_local_world(local_world, object_position, object_rotation);
        create_world_view(world_view, camera_position, camera_rotation);
        create_local_screen(local_screen, local_world, world_view, view_screen);
        calculate_vertices(temp, 8, cube_vertices, local_screen);
        draw_convert_xyz(verts, 2048, 2048, 32, 8, (vertex_f_t *)temp);
        draw_convert_rgbq(colors, 8, (vertex_f_t *)temp, (color_f_t *)cube_colors, 0x80);

        q = packet->data;
        q = draw_disable_tests(q, 0, &z);
        q = draw_clear(q, 0, 2048.0f - 320.0f, 2048.0f - 224.0f, 640, 448, 0x10, 0x10, 0x20);
        q = draw_enable_tests(q, 0, &z);
        q = draw_prim_start(q, 0, &prim, &color);
        for (i = 0; i < 36; i++) {
            q->dw[0] = colors[cube_points[i]].rgbaq;
            q->dw[1] = verts[cube_points[i]].xyz;
            q++;
        }
        q = draw_prim_end(q, 2, DRAW_RGBAQ_REGLIST);
        q = draw_finish(q);
        dma_channel_send_normal(DMA_CHANNEL_GIF, packet->data, q - packet->data, 0, 0);
        dma_wait_fast();
        draw_wait_finish();
        graph_wait_vsync();
    }
    printf("cubo: %d quadros\n", f);
}

int main(void) {
    dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
    dma_channel_fast_waits(DMA_CHANNEL_GIF);
    math3d_checks();
    asm_checks();
    micro_checks();
    render_cube();
    return 0;
}
