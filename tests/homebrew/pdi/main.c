/* pdi: os drivers da Polyphony Digital (Gran Turismo 4) e o SIF RPC
 * multi-thread da Sony em HLE.
 *
 * Fala os protocolos direto pelo SIF, como o lado do EE do jogo os usa
 * (levantados do que o jogo faz — sem código da Sony nem da Polyphony):
 *   - pdicdvd ("PCDV"): leitura de setores para a memória do EE;
 *   - pdistr ("STRP"): stream de um trecho do disco por LSN (abrir, ler em
 *     pedaços, fechar);
 *   - lgdev (0x046D046D): versão do módulo e enumeração sem volante;
 *   - msifrpc: handshake pelo SREG 1 e bind/call pelos comandos 0x80000019 e
 *     0x8000001A, com as respostas no comando 0x80000018 (aqui chamando o
 *     STRP, como faria um servidor registrado pelo sceSifMRegisterRpc).
 * Os "módulos" são IRX mínimos só com o nome no cabeçalho: o IOP em HLE
 * identifica módulos pelo nome. Os dados vêm de disc.iso (teste cdvd).
 */
#include <kernel.h>
#include <loadfile.h>
#include <sifcmd.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>

static SifRpcClientData_t cd __attribute__((aligned(64)));
static u32 buf[144] __attribute__((aligned(64))); /* até 576 bytes (lgdev) */
static u8 data[256] __attribute__((aligned(64)));

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
    put16(irx + 96 + 24, 0x0101);
    strcpy((char *)irx + 96 + 26, name);
    int res = 0;
    return SifExecModuleBuffer(irx, sizeof(irx), 0, NULL, &res);
}

static void bind(u32 sid)
{
    while (SifBindRpc(&cd, sid, 0) >= 0 && !cd.server)
        ;
}

static u32 call(u32 fn, int size, void *recv, int rsize)
{
    SifCallRpc(&cd, fn, 0, buf, size, recv, rsize, NULL, NULL);
    return buf[0];
}

/* BIG.BIN de disc.iso: byte i = (i * 7 + (i >> 8)) & 0xFF. */
static int big_ok(const u8 *p, int offset, int n)
{
    for (int i = 0; i < n; ++i) {
        int j = offset + i;
        if (p[i] != (u8)(j * 7 + (j >> 8)))
            return 0;
    }
    return 1;
}

/* --- msifrpc --------------------------------------------------------- */

static u32 pkt[16] __attribute__((aligned(64)));
static volatile u32 msif_cmd, msif_client, msif_server, msif_buffer;

static void msif_end(void *p, void *arg)
{
    const u32 *r = p;
    (void)arg;
    msif_client = r[7];
    msif_server = r[9];
    msif_buffer = r[10];
    msif_cmd = r[8]; /* por último: é o que o laço espera */
}

static void msif_wait(void)
{
    while (!msif_cmd)
        ;
}

int main(void)
{
    SifInitRpc(0);

    /* pdicdvd: lê HELLO.TXT (setor 27, 23 bytes) para a memória do EE. */
    printf("PDI_CDVD_Manager: %s\n", load_named_irx("PDI_CDVD_Manager") >= 0 ? "carregado" : "falhou");
    bind(0x50434456);
    memset(data, 0, sizeof(data));
    buf[0] = 27;
    buf[1] = 23;
    buf[2] = (u32)data;
    buf[3] = 0;
    FlushCache(0);
    printf("PCDV leitura: %d, \"%.22s\"\n", (int)call(3, 16, buf, 64), data);

    /* pdistr: stream de BIG.BIN (setor 24, 5000 bytes), lido em pedaços. */
    printf("PDI_Streaming_service: %s\n", load_named_irx("PDI_Streaming_service") >= 0 ? "carregado" : "falhou");
    bind(0x53545250);
    memset(buf, 0, 128);
    buf[0] = 24;
    buf[1] = 5000;
    buf[2] = 0x8000;
    u32 h = call(3, 128, buf, 64);
    printf("STRP abrir: handle %u\n", (unsigned)h);
    int ok = 1;
    for (int off = 0; off < 1024; off += 256) {
        memset(buf, 0, 128);
        buf[0] = h;
        buf[1] = (u32)data;
        buf[2] = 256;
        FlushCache(0);
        call(4, 128, NULL, 0);
        ok &= big_ok(data, off, 256);
    }
    printf("STRP 4 leituras de 256 bytes: %s\n", ok ? "conferem" : "ERRADAS");
    memset(buf, 0, 128);
    buf[0] = h;
    call(2, 128, buf, 64);
    printf("STRP fechado\n");

    /* lgdev: versão e enumeração sem volante. */
    printf("LgDev_tb_rb_Driver: %s\n", load_named_irx("LgDev_tb_rb_Driver") >= 0 ? "carregado" : "falhou");
    bind(0x046D046D);
    memset(buf, 0, 576);
    call(12, 576, buf, 576);
    printf("lgdev versao: 0x%08x\n", (unsigned)buf[1]);
    memset(buf, 0, 576);
    buf[1] = 0; /* dispositivo 0 */
    printf("lgdev enumeracao do dispositivo 0: 0x%08x\n", (unsigned)call(1, 576, buf, 576));

    /* msifrpc: handshake, bind e call do STRP pelo SIF RPC multi-thread. */
    printf("IOP_MSIF_rpc_interface: %s\n", load_named_irx("IOP_MSIF_rpc_interface") >= 0 ? "carregado" : "falhou");
    SifAddCmdHandler(0x80000018, msif_end, NULL);
    memset(pkt, 0, sizeof(pkt));
    pkt[4] = 1; /* SREG 1 do IOP = 1 */
    pkt[5] = 1;
    SifSendCmd(0x80000001, pkt, 24, NULL, NULL, 0);
    while (!SifGetSreg(1))
        ;
    printf("msif: SREG 1 = %u\n", (unsigned)SifGetSreg(1));

    memset(pkt, 0, sizeof(pkt));
    pkt[7] = 0x1234; /* "cliente": o IOP devolve em [28] */
    pkt[8] = 0x53545250;
    msif_cmd = 0;
    SifSendCmd(0x80000019, pkt, 64, NULL, NULL, 0);
    msif_wait();
    printf("msif bind: resposta 0x%08x, cliente 0x%x, servidor %s, buffer %s\n", (unsigned)msif_cmd,
           (unsigned)msif_client, msif_server ? "ok" : "nulo", msif_buffer ? "ok" : "nulo");
    const u32 server = msif_server, iopbuf = msif_buffer; /* as próximas respostas sobrescrevem */

    memset(buf, 0, 128);
    buf[0] = 27;
    buf[1] = 23;
    buf[2] = 0x8000;
    static u32 recv[16] __attribute__((aligned(64)));
    memset(recv, 0, sizeof(recv));
    memset(pkt, 0, sizeof(pkt));
    pkt[7] = 0x1234;
    pkt[8] = 3;            /* abrir */
    pkt[9] = 128;          /* tamanho do envio */
    pkt[10] = (u32)recv;   /* recepção */
    pkt[11] = 64;
    pkt[13] = server;
    msif_cmd = 0;
    FlushCache(0);
    SifSendCmd(0x8000001A, pkt, 64, buf, (void *)iopbuf, 128);
    msif_wait();
    FlushCache(0);
    h = recv[0];
    printf("msif call: resposta 0x%08x, handle %u\n", (unsigned)msif_cmd, (unsigned)h);

    memset(data, 0, sizeof(data));
    memset(buf, 0, 128);
    buf[0] = h;
    buf[1] = (u32)data;
    buf[2] = 23;
    pkt[8] = 4; /* ler */
    pkt[11] = 0;
    msif_cmd = 0;
    FlushCache(0);
    SifSendCmd(0x8000001A, pkt, 64, buf, (void *)iopbuf, 128);
    msif_wait();
    printf("msif leitura pelo STRP: \"%.22s\"\n", data);

    printf("fim\n");
    return 0;
}
