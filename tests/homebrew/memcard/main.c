/* Fase 6: memory card via libmc + mcserv do IOP (HLE, cartões = pastas do
 * host). Cria uma pasta e um save, lista, lê de volta, renomeia e apaga. */
#include <kernel.h>
#include <libmc.h>
#include <loadfile.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>

/* Flags de mcOpen no formato do mcserv (iguais às do ioman do IOP). */
#define MC_RDONLY 0x0001
#define MC_WRONLY 0x0002
#define MC_CREAT 0x0200

static sceMcTblGetDir table[16];
static char buf[4096] __attribute__((aligned(64)));

static int sync_ret(void)
{
    int cmd, ret;
    mcSync(0, &cmd, &ret);
    return ret;
}

static void info(int port)
{
    int type = -1, free = -1, format = -1, ret;
    mcGetInfo(port, 0, &type, &free, &format);
    ret = sync_ret();
    printf("mc%d: getInfo %d tipo %d livre %d formatado %d\n", port, ret, type, free, format);
}

static void list(const char *pattern)
{
    int i, n;
    mcGetDir(0, 0, pattern, 0, 16, table);
    n = sync_ret();
    printf("getDir(%s): %d\n", pattern, n);
    for (i = 0; i < n; i++)
        printf("  %-12s %5u bytes attr 0x%04x\n", table[i].EntryName, (unsigned)table[i].FileSizeByte,
               table[i].AttrFile);
}

int main(void)
{
    int fd, ret, i;
    char dir[1024];

    sceSifInitRpc(0);
    if (SifLoadModule("rom0:XSIO2MAN", 0, NULL) < 0) printf("erro sio2man\n");
    if (SifLoadModule("rom0:XMCMAN", 0, NULL) < 0) printf("erro mcman\n");
    if (SifLoadModule("rom0:XMCSERV", 0, NULL) < 0) printf("erro mcserv\n");
    printf("mcInit: %d\n", mcInit(MC_TYPE_XMC));

    info(0);
    info(0); /* segunda chamada: o cartão não foi trocado */
    info(1); /* sem cartão na porta 2 */

    mcMkDir(0, 0, "AP2TEST");
    printf("mkdir: %d\n", sync_ret());
    mcMkDir(0, 0, "AP2TEST");
    printf("mkdir de novo: %d\n", sync_ret());

    for (i = 0; i < (int)sizeof(buf); i++) buf[i] = (char)('A' + i % 26);
    mcOpen(0, 0, "AP2TEST/SAVE.DAT", MC_CREAT | MC_WRONLY);
    fd = sync_ret();
    printf("open(escrita): %s\n", fd >= 0 ? "ok" : "erro");
    mcWrite(fd, buf, 3000);
    printf("write: %d\n", sync_ret());
    mcClose(fd);
    printf("close: %d\n", sync_ret());

    mcOpen(0, 0, "AP2TEST/NOTE.TXT", MC_CREAT | MC_WRONLY);
    fd = sync_ret();
    mcWrite(fd, "salvo pelo AnyPS2\n", 18);
    printf("write nota: %d\n", sync_ret());
    mcClose(fd);
    sync_ret();

    list("AP2TEST/*");
    list("/*");
    info(0);

    mcOpen(0, 0, "AP2TEST/SAVE.DAT", MC_RDONLY);
    fd = sync_ret();
    mcSeek(fd, 2990, 0);
    printf("seek: %d\n", sync_ret());
    memset(buf, 0, sizeof(buf));
    mcRead(fd, buf, 100);
    ret = sync_ret();
    printf("read após seek: %d \"%s\"\n", ret, buf);
    mcClose(fd);
    sync_ret();

    mcChdir(0, 0, "AP2TEST", dir);
    printf("chdir: %d\n", sync_ret());
    mcOpen(0, 0, "NOTE.TXT", MC_RDONLY);
    fd = sync_ret();
    memset(buf, 0, sizeof(buf));
    mcRead(fd, buf, 64);
    printf("read relativo: %d %s", sync_ret(), buf);
    mcClose(fd);
    sync_ret();
    mcChdir(0, 0, "/", dir);
    sync_ret();

    mcRename(0, 0, "AP2TEST/NOTE.TXT", "README.TXT");
    printf("rename: %d\n", sync_ret());
    mcOpen(0, 0, "AP2TEST/NOTE.TXT", MC_RDONLY);
    printf("open do nome antigo: %d\n", sync_ret());
    list("AP2TEST/*");

    mcDelete(0, 0, "AP2TEST/README.TXT");
    printf("delete: %d\n", sync_ret());
    mcDelete(0, 0, "AP2TEST");
    printf("delete pasta não vazia: %d\n", sync_ret());
    mcDelete(0, 0, "AP2TEST/SAVE.DAT");
    sync_ret();
    mcDelete(0, 0, "AP2TEST");
    printf("delete pasta vazia: %d\n", sync_ret());
    list("/*");
    info(0);
    printf("fim\n");
    return 0;
}
