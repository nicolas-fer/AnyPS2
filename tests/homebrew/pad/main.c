/* Fase 6: controles via libpad + padman do IOP (HLE).
 *
 * argv[1] == "rom": módulos antigos da ROM (rom0:SIO2MAN/PADMAN, protocolo
 * 0x8000010F); senão os novos (rom0:XSIO2MAN/XPADMAN). A entrada vem do
 * roteiro ANYPS2_PAD_SCRIPT (script.txt), então a saída é determinística. */
#include <kernel.h>
#include <libpad.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>

static char padBuf[2][256] __attribute__((aligned(64)));
static int vsema;
static volatile int frame;

static int vblank_handler(int cause)
{
    (void)cause;
    frame++;
    iSignalSema(vsema);
    return 0;
}

static void wait_vblank(void)
{
    WaitSema(vsema);
}

static int wait_ready(int port)
{
    int i, state = 0;
    for (i = 0; i < 300; i++) {
        state = padGetState(port, 0);
        if (state == PAD_STATE_STABLE || state == PAD_STATE_FINDCTP1) return state;
        wait_vblank();
    }
    return state;
}

static void wait_req(int port)
{
    int i;
    for (i = 0; i < 300 && padGetReqState(port, 0) == PAD_RSTAT_BUSY; i++) wait_vblank();
}

int main(int argc, char **argv)
{
    const int rom = argc > 1 && strcmp(argv[1], "rom") == 0;
    struct padButtonStatus b;
    ee_sema_t s;
    int port, i, modes;
    u16 last[2] = {0, 0};
    unsigned char lastAxis[2][4];

    sceSifInitRpc(0);
    printf("pad: módulos %s\n", rom ? "da ROM (SIO2MAN/PADMAN)" : "X (XSIO2MAN/XPADMAN)");
    if (SifLoadModule(rom ? "rom0:SIO2MAN" : "rom0:XSIO2MAN", 0, NULL) < 0) printf("erro sio2man\n");
    if (SifLoadModule(rom ? "rom0:PADMAN" : "rom0:XPADMAN", 0, NULL) < 0) printf("erro padman\n");

    s.init_count = 0;
    s.max_count = 1;
    s.option = 0;
    vsema = CreateSema(&s);
    AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0);
    EnableIntc(INTC_VBLANK_S);

    printf("padInit: %d\n", padInit(0));
    printf("portas: %d, slots: %d\n", padGetPortMax(), padGetSlotMax(0));
    for (port = 0; port < 2; port++) {
        printf("porta %d: padPortOpen = %d\n", port, padPortOpen(port, 0, padBuf[port]));
        printf("porta %d: estado %d\n", port, wait_ready(port));
    }

    modes = padInfoMode(0, 0, PAD_MODETABLE, -1);
    printf("modos: %d (", modes);
    for (i = 0; i < modes; i++) printf(" 0x%x", padInfoMode(0, 0, PAD_MODETABLE, i));
    printf(" ), atual id 0x%x\n", padInfoMode(0, 0, PAD_MODECURID, 0));
    printf("padSetMainMode(analógico): %d\n", padSetMainMode(0, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK));
    wait_req(0);
    wait_ready(0);
    printf("modo atual: 0x%x (ext 0x%x)\n", padInfoMode(0, 0, PAD_MODECURID, 0),
           padInfoMode(0, 0, PAD_MODECUREXID, 0));
    printf("pressão: info %d, enter %d\n", padInfoPressMode(0, 0), padEnterPressMode(0, 0));
    wait_req(0);
    wait_ready(0);
    printf("atuadores: %d\n", padInfoAct(0, 0, -1, 0));

    memset(lastAxis, 0x80, sizeof(lastAxis));
    frame = 0;
    for (i = 0; i < 150; i++) {
        wait_vblank();
        for (port = 0; port < 2; port++) {
            int state = padGetState(port, 0);
            u16 btns;
            if (state != PAD_STATE_STABLE && state != PAD_STATE_FINDCTP1) {
                if (last[port] != 0xFFFF) printf("quadro %3d porta %d: desconectado (estado %d)\n", i, port, state);
                last[port] = 0xFFFF;
                continue;
            }
            memset(&b, 0, sizeof(b));
            if (padRead(port, 0, &b) == 0) continue;
            btns = 0xFFFF ^ b.btns;
            if ((b.mode >> 4) != 7) b.ljoy_h = b.ljoy_v = b.rjoy_h = b.rjoy_v = 0x80; /* digital */
            if (btns != last[port] || b.ljoy_h != lastAxis[port][0] || b.ljoy_v != lastAxis[port][1] ||
                b.rjoy_h != lastAxis[port][2] || b.rjoy_v != lastAxis[port][3]) {
                printf("quadro %3d porta %d: modo 0x%02x botões 0x%04x eixos %3d %3d %3d %3d", i, port, b.mode,
                       btns, b.ljoy_h, b.ljoy_v, b.rjoy_h, b.rjoy_v);
                if (port == 0 && b.mode == 0x79)
                    printf(" pressão ✕=%d ○=%d", b.cross_p, b.circle_p);
                printf("\n");
                last[port] = btns;
                lastAxis[port][0] = b.ljoy_h;
                lastAxis[port][1] = b.ljoy_v;
                lastAxis[port][2] = b.rjoy_h;
                lastAxis[port][3] = b.rjoy_v;
            }
        }
    }
    printf("fim\n");
    return 0;
}
