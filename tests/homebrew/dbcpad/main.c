/* dbcpad: a pilha de controles do SDK 3.0 (dbcman/libdbc/libpad2) em HLE.
 *
 * Fala o protocolo do dbcman direto pelo SIF (escrito a partir do que o
 * protocolo faz — sem código da Sony). O "módulo" carregado é um IRX mínimo
 * montado aqui, só com o nome Dbc_Manager no cabeçalho: o IOP em HLE
 * identifica módulos pelo nome. Com o roteiro de controle do teste, os
 * quadros que o IOP escreve no EE mostram os botões apertados.
 *
 * Também: sceMcGetSlotMax (mcserv, função 0x15) e devctl("dev9x:") pelo
 * fileio do SDK 3.0 num console sem adaptador de rede (-ENODEV).
 */
#include <delaythread.h>
#include <kernel.h>
#include <loadfile.h>
#include <sifcmd.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>

static SifRpcClientData_t dbc __attribute__((aligned(64)));
static u32 buf[64] __attribute__((aligned(64)));      /* 0x90 bytes de ida e volta */
static u32 work[32] __attribute__((aligned(64)));     /* área de trabalho: estado por socket */
static u8 frames[2][256] __attribute__((aligned(64))); /* por socket: 2 quadros de 128 bytes */

static void put16(u8 *p, u16 v) { p[0] = v; p[1] = v >> 8; }
static void put32(u8 *p, u32 v) { put16(p, v); put16(p + 2, v >> 16); }

/* IRX mínimo: cabeçalho ELF + PT_SCE_IOPMOD + .iopmod com o nome. */
static int load_named_irx(const char *name)
{
    static u8 irx[256] __attribute__((aligned(64)));
    memset(irx, 0, sizeof(irx));
    irx[0] = 0x7F; irx[1] = 'E'; irx[2] = 'L'; irx[3] = 'F';
    irx[4] = 1; irx[5] = 1; irx[6] = 1;
    put16(irx + 16, 0xFF80); /* ET_SCE_IOPRELEXEC */
    put16(irx + 18, 8);      /* MIPS */
    put32(irx + 28, 52);     /* phoff */
    put16(irx + 42, 32);
    put16(irx + 44, 1);
    put32(irx + 52, 0x70000080); /* PT_SCE_IOPMOD */
    put32(irx + 56, 96);         /* offset do .iopmod */
    put16(irx + 96 + 24, 0x0316);
    strcpy((char *)irx + 96 + 26, name);
    int res = 0;
    return SifExecModuleBuffer(irx, sizeof(irx), 0, NULL, &res);
}

static u32 dbc_call(u32 fn)
{
    SifCallRpc(&dbc, fn, 0, buf, 0x90, buf, 0x90, NULL, NULL);
    return buf[0];
}

/* Quadro mais recente de um socket (contador em 124). */
static const u8 *latest(int s)
{
    const u8 *a = frames[s], *b = frames[s] + 128;
    return *(const u32 *)(b + 124) > *(const u32 *)(a + 124) ? b : a;
}

static void wait_vblanks(int n)
{
    while (n-- > 0)
        DelayThread(16700);
}

static void show(const char *when, int s)
{
    const u8 *f = latest(s);
    printf("%s: estado %u, %u bytes, perfil %u bytes, status %u, botoes %02x %02x, "
           "analogicos %u %u %u %u, pressao X %u\n",
           when, f[0], f[2], f[3], f[4], f[28], f[29], f[30], f[31], f[32], f[33], f[28 + 6 + 6]);
}

int main(void)
{
    SifInitRpc(0);
    printf("Dbc_Manager carregado: %s\n", load_named_irx("Dbc_Manager") >= 0 ? "sim" : "nao");
    while (SifBindRpc(&dbc, 0x80001300, 0) >= 0 && !dbc.server)
        ;

    printf("versao: 0x%x\n", (unsigned)dbc_call(0x80001363));
    buf[1] = (u32)work;
    dbc_call(0x80001304);

    int socks[2];
    for (int port = 0; port < 2; ++port) {
        memset(buf, 0, sizeof(buf));
        buf[0] = 3; /* tipo | 1 */
        buf[1] = 1;
        buf[2] = port;
        buf[10] = (u32)frames[port];
        buf[11] = (u32)frames[port] + 128;
        dbc_call(0x80001301);
        socks[port] = buf[9];
        printf("socket da porta %d: %d, estado na area de trabalho: %u\n", port, socks[port],
               (unsigned)work[socks[port]]);
    }

    buf[0] = socks[0];
    dbc_call(0x80001303);
    show("logo depois de iniciar", 0);

    memset(buf, 0, sizeof(buf));
    buf[0] = socks[0];
    buf[1] = 0x0101800C; /* consulta: estado */
    dbc_call(0x8000131A);
    printf("consulta de estado: %u byte(s), valor %u, resultado %d\n", (unsigned)buf[2],
           ((u8 *)buf)[12], (int)buf[35]);

    wait_vblanks(40); /* o roteiro aperta CROSS+START no VBlank 30 */
    show("CROSS+START", 0);
    wait_vblanks(30); /* e solta no 60 */
    show("solto", 0);

    buf[0] = socks[1];
    dbc_call(0x80001302);
    printf("socket da porta 1 apagado: %d, estado agora %u\n", (int)buf[1], (unsigned)work[socks[1]]);

    /* sceMcGetSlotMax: mcserv, função 0x15 */
    SifLoadModule("rom0:XSIO2MAN", 0, NULL);
    SifLoadModule("rom0:XMCMAN", 0, NULL);
    SifLoadModule("rom0:XMCSERV", 0, NULL);
    static SifRpcClientData_t mc __attribute__((aligned(64)));
    while (SifBindRpc(&mc, 0x80000400, 0) >= 0 && !mc.server)
        ;
    static u32 mcbuf[12] __attribute__((aligned(64)));
    static u32 mcres[4] __attribute__((aligned(64)));
    mcbuf[1] = 0; /* porta */
    SifCallRpc(&mc, 0x15, 0, mcbuf, 48, mcres, 16, NULL, NULL);
    printf("slots na porta 0 do memory card: %d\n", (int)mcres[0]);
    return 0;
}
