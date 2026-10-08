/* Fase 6: carregamento de módulos do IOP (loadfile em HLE).
 *   (sem argumento) rom0: e IRX embutido (padman.irx do ps2sdk) com HLE
 *   unknown         IRX sem HLE (usbd.irx): tem de parar com erro claro
 *   rom             rom0:FOOBAR inexistente: idem */
#include <iopheap.h>
#include <kernel.h>
#include <libpad.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>

extern unsigned char padman_irx[], padman_irx_end[];
extern unsigned char usbd_irx[], usbd_irx_end[];

static int exec(const char *name, unsigned char *start, unsigned char *end)
{
    int res = -1;
    int id = SifExecModuleBuffer(start, (u32)(end - start), 0, NULL, &res);
    printf("SifExecModuleBuffer(%s): %s\n", name, id >= 0 ? "ok" : "erro");
    return id;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "";

    sceSifInitRpc(0);
    SifInitIopHeap();
    SifLoadFileInit();
    printf("rom0:XSIO2MAN: %s\n", SifLoadModule("rom0:XSIO2MAN", 0, NULL) >= 0 ? "ok" : "erro");
    if (strcmp(mode, "unknown") == 0) {
        printf("carregando usbd.irx (sem HLE)...\n");
        exec("usbd.irx", usbd_irx, usbd_irx_end);
        printf("não deveria chegar aqui\n");
        return 1;
    }
    if (strcmp(mode, "rom") == 0) {
        printf("carregando rom0:FOOBAR...\n");
        SifLoadModule("rom0:FOOBAR", 0, NULL);
        printf("não deveria chegar aqui\n");
        return 1;
    }
    printf("padman já carregado? %s\n", SifSearchModuleByName("padman") >= 0 ? "sim" : "não");
    exec("padman.irx", padman_irx, padman_irx_end);
    exec("padman.irx de novo", padman_irx, padman_irx_end);
    printf("padman carregado? %s\n", SifSearchModuleByName("padman") >= 0 ? "sim" : "não");
    printf("padInit: %d\n", padInit(0));
    printf("rom0:XSIO2MAN de novo: %s\n", SifLoadModule("rom0:XSIO2MAN", 0, NULL) >= 0 ? "ok" : "erro");
    printf("fim\n");
    return 0;
}
