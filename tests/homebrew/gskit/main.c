/*
 * gskit: o mesmo tipo de cena do gfx2d, mas pela gsKit (a biblioteca
 * gráfica mais usada em homebrews de PS2): fila de desenho com dmaKit em
 * chain, textura PSMCT32 enviada pela gsKit, primitivas com Gouraud,
 * blending e troca de buffers com gsKit_sync_flip.
 */
#include <dmaKit.h>
#include <gsKit.h>
#include <gsToolkit.h>
#include <malloc.h>
#include <stdio.h>

int main(void) {
    GSGLOBAL *gs = gsKit_init_global();
    GSTEXTURE tex;
    u32 *pixels;
    int x, y, frame;

    gs->PSM = GS_PSM_CT32;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->PrimAlphaEnable = GS_SETTING_ON;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8,
                1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    printf("gskit: tela %dx%d, modo %d, entrelaçado %d\n", gs->Width, gs->Height, gs->Mode, gs->Interlace);

    tex.Width = 64;
    tex.Height = 64;
    tex.PSM = GS_PSM_CT32;
    tex.Filter = GS_FILTER_NEAREST;
    tex.Clut = NULL;
    tex.Mem = memalign(128, gsKit_texture_size_ee(64, 64, GS_PSM_CT32));
    pixels = (u32 *)tex.Mem;
    for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
            pixels[y * 64 + x] = (((x ^ y) & 8) ? 0xFF2080F0u : 0xFFF0E040u) ^ (u32)((x * 3) << 8);
    tex.Vram = gsKit_vram_alloc(gs, gsKit_texture_size(64, 64, GS_PSM_CT32), GSKIT_ALLOC_USERBUFFER);
    gsKit_texture_upload(gs, &tex);

    for (frame = 0; frame < 3; frame++) {
        float shift = (float)(frame * 20);
        gsKit_clear(gs, GS_SETREG_RGBAQ(0x10, 0x30, 0x20, 0x80, 0x00));
        gsKit_prim_sprite(gs, 40.0f + shift, 40.0f, 200.0f + shift, 120.0f, 1, GS_SETREG_RGBAQ(0xFF, 0x40, 0x40, 0x80, 0));
        gsKit_prim_triangle_gouraud(gs, 320.0f, 60.0f, 560.0f, 380.0f, 80.0f, 380.0f, 1,
                                    GS_SETREG_RGBAQ(0xFF, 0xFF, 0x00, 0x80, 0), GS_SETREG_RGBAQ(0x00, 0xFF, 0xFF, 0x80, 0),
                                    GS_SETREG_RGBAQ(0xFF, 0x00, 0xFF, 0x80, 0));
        gsKit_prim_line(gs, 20.0f, 420.0f, 620.0f, 300.0f, 1, GS_SETREG_RGBAQ(0xFF, 0xFF, 0xFF, 0x80, 0));
        gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);
        gsKit_prim_sprite(gs, 100.0f, 200.0f, 540.0f, 260.0f, 1, GS_SETREG_RGBAQ(0x00, 0x00, 0xFF, 0x40, 0));
        gsKit_prim_sprite_texture(gs, &tex, 440.0f, 40.0f, 0.0f, 0.0f, 568.0f, 168.0f, 64.0f, 64.0f, 1,
                                  GS_SETREG_RGBAQ(0x80, 0x80, 0x80, 0x80, 0));
        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
    }
    printf("gskit: %d quadros, ok\n", frame);
    return 0;
}
