/* Fase 6: som via audsrv.irx (ps2sdk, embutido e carregado com
 * SifExecModuleBuffer) + SPU2 em software. Toca 0,3 s de PCM (onda
 * triangular de 441 Hz, 22050 Hz mono) e depois um sample ADPCM (onda
 * quadrada) numa voz do SPU2. O teste compara o WAV gerado. */
#include <audsrv.h>
#include <iopheap.h>
#include <kernel.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>

extern unsigned char freesd_irx[], freesd_irx_end[];
extern unsigned char audsrv_irx[], audsrv_irx_end[];

static short pcm[1024];
static unsigned char adpcm[16 + 40 * 16] __attribute__((aligned(64)));
static int vsema;

static int vblank_handler(int cause)
{
    (void)cause;
    iSignalSema(vsema);
    return 0;
}

static int load(const char *name, unsigned char *start, unsigned char *end)
{
    int res = -1;
    int id = SifExecModuleBuffer(start, (u32)(end - start), 0, NULL, &res);
    printf("%s: %s\n", name, id >= 0 ? "carregado" : "erro");
    return id;
}

int main(void)
{
    struct audsrv_fmt_t fmt;
    audsrv_adpcm_t sample;
    ee_sema_t s;
    int i, phase = 0, sent = 0, frames, ch;

    sceSifInitRpc(0);
    SifInitIopHeap();
    SifLoadFileInit();
    load("freesd.irx", freesd_irx, freesd_irx_end);
    load("audsrv.irx", audsrv_irx, audsrv_irx_end);

    s.init_count = 0;
    s.max_count = 1;
    s.option = 0;
    vsema = CreateSema(&s);
    AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0);
    EnableIntc(INTC_VBLANK_S);

    printf("audsrv_init: %d\n", audsrv_init());
    fmt.freq = 96000;
    fmt.bits = 16;
    fmt.channels = 2;
    printf("formato 96 kHz: %d\n", audsrv_set_format(&fmt));
    fmt.freq = 22050;
    fmt.bits = 16;
    fmt.channels = 1;
    printf("formato 22050 Hz mono: %d\n", audsrv_set_format(&fmt));
    printf("volume: %d\n", audsrv_set_volume(MAX_VOLUME));
    printf("livre no início: %d\n", audsrv_available());

    /* 0,3 s de PCM: 6615 amostras, em blocos de 1024 */
    while (sent < 6615) {
        int n = 6615 - sent < 1024 ? 6615 - sent : 1024;
        for (i = 0; i < n; i++, phase = (phase + 1) % 50)
            pcm[i] = (short)((phase < 25 ? phase * 2 - 25 : 75 - phase * 2) * 1000);
        audsrv_wait_audio(n * 2);
        sent += audsrv_play_audio((char *)pcm, n * 2) / 2;
    }
    printf("PCM enviado: %d amostras, na fila: %s\n", sent, audsrv_queued() > 0 ? "sim" : "não");
    for (frames = 0; audsrv_queued() > 0 && frames < 120; frames++) WaitSema(vsema);
    printf("fila vazia depois de ~%d quadros\n", frames);

    /* ADPCM: cabeçalho do audsrv + 40 blocos (filtro 0, shift 4): quadrada */
    memset(adpcm, 0, sizeof(adpcm));
    memcpy(adpcm, "APCM", 4);
    adpcm[4] = 1;    /* versão */
    adpcm[5] = 1;    /* canais */
    adpcm[6] = 0;    /* sem loop */
    adpcm[8] = 0x00; /* pitch 0x800 = 24 kHz */
    adpcm[9] = 0x08;
    for (i = 0; i < 40; i++) {
        unsigned char *b = adpcm + 16 + i * 16;
        int j;
        b[0] = 0x02;
        b[1] = i == 39 ? 1 : 0; /* fim do sample no último bloco */
        for (j = 0; j < 14; j++) b[2 + j] = (i & 1) ? 0x99 : 0x77; /* -7 ou +7 */
    }
    printf("adpcm_init: %d\n", audsrv_adpcm_init());
    printf("load_adpcm: %d", audsrv_load_adpcm(&sample, adpcm, sizeof(adpcm)));
    printf(" pitch 0x%x canais %d loop %d\n", sample.pitch, sample.channels, sample.loop);
    ch = audsrv_ch_play_adpcm(-1, &sample);
    printf("tocando no canal %d\n", ch);
    printf("volume/pan: %d\n", audsrv_adpcm_set_volume_and_pan(ch, 80, -50));
    WaitSema(vsema);
    printf("tocando? %d\n", audsrv_is_adpcm_playing(ch, &sample));
    for (frames = 0; audsrv_is_adpcm_playing(ch, &sample) == 1 && frames < 120; frames++) WaitSema(vsema);
    printf("sample terminou depois de ~%d quadros\n", frames);
    for (i = 0; i < 6; i++) WaitSema(vsema);
    printf("fim\n");
    return 0;
}
