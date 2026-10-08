/* kpatch: o que jogos comerciais fazem com o kernel e o IOP logo no boot.
 *
 * 1. Tabela de syscalls na RAM do kernel: o programa instala uma syscall
 *    própria (FindAddress, 0x83) e com ela acha a tabela procurando o
 *    ponteiro que acabou de instalar (como a libkernel da Sony faz);
 *    redireciona uma syscall para outra com GetEntryAddress; tenta trocar
 *    uma syscall que o HLE implementa (o HLE continua valendo); chama o
 *    handler de uma syscall direto por ponteiro.
 * 2. ERET saindo de modo kernel (ERL → ErrorEPC; EXL → EPC).
 * 3. Protocolo do fileio do SDK 3.0 (função 255, buffers de conclusão e o
 *    comando SIF 0x80000011), escrito a partir do que o protocolo faz — sem
 *    código da Sony.
 * 4. Reboot do IOP na ordem da Sony: o EE limpa SIFINIT/CMDINIT depois de
 *    mandar o reset e só então espera as flags voltarem.
 * 5. Versão do loadfile e rom0:ROMVER pelo fileio.
 *
 * O fileio passa ao protocolo do SDK 3.0 depois da função 255 e só volta no
 * reboot do IOP; por isso os resultados são guardados e impressos depois.
 */
#include <iopcontrol.h>
#include <kernel.h>
#include <sifcmd.h>
#include <sifdma.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>

u32 sys_83(void *start, void *end, u32 value);
u32 sys_84(void);
u32 sys_7f(void);
u32 get_status(void);
void kmode_write(volatile u32 *addr, u32 value);
u32 eret_epc(void);

static char log_buf[4096];
static int log_len;
#define LOG(...) (log_len += sprintf(log_buf + log_len, __VA_ARGS__))

/* ---- 1. tabela de syscalls -------------------------------------------- */

static u32 find_word(u32 *p, u32 *end, u32 value)
{
    for (; p < end; ++p)
        if (*p == value)
            return (u32)p;
    return 0;
}

static void syscall_table(void)
{
    SetSyscall(0x83, find_word);
    u32 slot = sys_83((void *)0x80000000, (void *)0x80080000, (u32)find_word);
    u32 *table = (u32 *)(slot - 0x83 * 4);
    printf("FindAddress do programa achou a entrada: %s\n", slot ? "sim" : "nao");
    printf("tabela[0x7F] == GetEntryAddress(0x7F): %s\n",
           table[0x7f] == (u32)GetEntryAddress(0x7f) ? "sim" : "nao");

    SetSyscall(0x84, GetEntryAddress(0x7f));
    printf("syscall 0x84 -> GetMemorySize: 0x%x\n", (unsigned)sys_84());

    void *old = GetEntryAddress(0x7f);
    SetSyscall(0x7f, find_word); /* o HLE implementa 0x7F: continua valendo */
    printf("GetMemorySize com handler trocado: 0x%x\n", (unsigned)sys_7f());
    SetSyscall(0x7f, old);

    u32 (*direct)(void) = (u32(*)(void))GetEntryAddress(0x7f);
    printf("handler de 0x7F chamado por ponteiro: 0x%x\n", (unsigned)direct());
}

/* ---- 2. ERET ------------------------------------------------------------ */

static void eret_tests(void)
{
    volatile u32 *t2comp = (volatile u32 *)0xB0001020; /* T2_COMP por kseg1 */
    kmode_write(t2comp, 0x1234);
    printf("T2_COMP escrito em modo kernel: 0x%x\n", (unsigned)*(volatile u32 *)0x10001020);
    printf("ERL depois do ERET: %u\n", (unsigned)((get_status() >> 2) & 1));
    printf("EXL depois do ERET para EPC: %u\n", (unsigned)((eret_epc() >> 1) & 1));
}

/* ---- 3. fileio do SDK 3.0 ----------------------------------------------- */

static SifRpcClientData_t fio_cd __attribute__((aligned(64)));
static u32 req[520] __attribute__((aligned(64)));            /* até 2080 bytes (devctl) */
static u32 reply[4] __attribute__((aligned(64)));
static u8 done_buf[2][0x440] __attribute__((aligned(64)));
static u8 data[64] __attribute__((aligned(64)));
static u8 stat_buf[64] __attribute__((aligned(64)));
static int sema;

/* Comando 0x80000011: opt = buffer; {sema, função, destino, tamanho, dados...} */
static void fio_done(void *packet, void *arg)
{
    const SifCmdHeader_t *h = packet;
    const u32 *b = (const u32 *)done_buf[h->opt & 1];
    (void)arg;
    memcpy((void *)b[2], &b[4], b[3]);
    if (b[1] == 12 && (s32)b[4] == 0) /* getstat: {destino, iox_stat_t} */
        memcpy((void *)b[5], &b[6], 64);
    iSignalSema(b[0]);
}

static s32 fio_call(int fn, int size)
{
    static s32 result;
    req[0] = sema;
    req[1] = (u32)&result;
    req[2] = 4;
    if (SifCallRpc(&fio_cd, fn, 0, req, size, reply, 16, NULL, NULL) < 0 || reply[0] == 0)
        return -1000;
    WaitSema(sema);
    return result;
}

static void sce_fileio(void)
{
    ee_sema_t s = {.init_count = 0, .max_count = 1};
    sema = CreateSema(&s);
    SifAddCmdHandler(0x80000011, fio_done, NULL);
    while (SifBindRpc(&fio_cd, 0x80000001, 0) >= 0 && !fio_cd.server)
        ;

    req[0] = (u32)done_buf[0];
    req[1] = (u32)done_buf[1];
    SifCallRpc(&fio_cd, 255, 0, req, 8, reply, 16, NULL, NULL);
    LOG("fileio 255: versao %.4s, %u buffers\n", (const char *)reply, (unsigned)reply[1]);

    memset(req, 0, sizeof(req));
    req[3] = 1; /* O_RDONLY */
    strcpy((char *)&req[5], "rom0:ROMVER");
    const s32 fd = fio_call(0, 1048);
    LOG("open(rom0:ROMVER): %s\n", fd >= 0 ? "ok" : "erro");

    req[3] = fd;
    req[4] = (u32)data;
    req[5] = 16;
    const s32 got = fio_call(2, 32);
    LOG("read: %d bytes, \"%.14s\"\n", (int)got, (const char *)data);

    req[3] = fd;
    req[4] = 4;
    req[5] = 0; /* SEEK_SET */
    LOG("lseek(4): %d\n", (int)fio_call(4, 28));
    req[3] = fd;
    LOG("close: %d\n", (int)fio_call(1, 20));

    memset(req, 0, sizeof(req));
    req[3] = (u32)stat_buf;
    strcpy((char *)&req[4], "host:dado.bin");
    const s32 st = fio_call(12, 1040);
    LOG("getstat(host:dado.bin): %d, modo 0x%x, tamanho %u\n", (int)st, (unsigned)(*(u32 *)stat_buf & 0xF000),
        (unsigned)((u32 *)stat_buf)[2]);
    strcpy((char *)&req[4], "host:nao_existe");
    LOG("getstat(host:nao_existe): %d\n", (int)fio_call(12, 1040));

    /* devctl no adaptador de rede/HDD, que este console não tem */
    memset(req, 0, sizeof(req));
    strcpy((char *)&req[3], "dev9x:");
    req[259] = 0x4401; /* comando */
    LOG("devctl(dev9x:): %d\n", (int)fio_call(23, 2076));
}

/* ---- 4. reboot do IOP na ordem da Sony ---------------------------------- */

static void reboot_iop(void)
{
    SifIopReset("", 0);
    /* Logo depois de mandar o reset, a Sony limpa estas flags... */
    SifSetReg(SIF_REG_SMFLAG, SIF_STAT_SIFINIT);
    SifSetReg(SIF_REG_SMFLAG, SIF_STAT_CMDINIT);
    /* ...e espera o IOP terminar o boot e religá-las. */
    while (!SifIopSync())
        ;
    while (!(SifGetReg(SIF_REG_SMFLAG) & SIF_STAT_CMDINIT))
        ;
    SifInitRpc(0);
}

int main(void)
{
    SifInitRpc(0);
    syscall_table();
    eret_tests();

    FILE *f = fopen("host:dado.bin", "wb");
    static char zeros[1234];
    fwrite(zeros, 1, sizeof(zeros), f);
    fclose(f);

    sce_fileio();
    reboot_iop();
    printf("reboot do IOP: ok\n");
    fputs(log_buf, stdout);

    /* Depois do reboot o fileio voltou ao protocolo do ps2sdk. */
    f = fopen("rom0:ROMVER", "rb");
    char romver[17] = {0};
    fread(romver, 1, 16, f);
    fclose(f);
    printf("rom0:ROMVER pelo fileio do ps2sdk: %s\n", romver);

    SifRpcClientData_t lf __attribute__((aligned(64)));
    memset(&lf, 0, sizeof(lf));
    while (SifBindRpc(&lf, 0x80000006, 0) >= 0 && !lf.server)
        ;
    SifCallRpc(&lf, 255, 0, NULL, 0, reply, 16, NULL, NULL);
    printf("versao do loadfile: %.4s\n", (const char *)reply);
    return 0;
}
