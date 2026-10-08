/* Fase 6: leitor de disco via libcdvd + cdvdfsv do IOP (HLE) e cdrom0: no
 * fileio. O disco é disc.iso (gerado por make_iso.py), passado em
 * ANYPS2_ISO. */
#include <dirent.h>
#include <fcntl.h>
#include <kernel.h>
#include <libcdvd.h>
#include <sifrpc.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned char sectors[3 * 2048] __attribute__((aligned(64)));

int main(void)
{
    sceCdlFILE f;
    sceCdRMode mode;
    sceCdCLOCK clock;
    struct dirent *de;
    struct stat st;
    DIR *dir;
    FILE *fp;
    char line[128];
    int fd, i, sum;

    sceSifInitRpc(0);
    printf("sceCdInit: %d\n", sceCdInit(SCECdINIT));
    printf("sceCdMmode: %d\n", sceCdMmode(SCECdMmodeCd));
    printf("tipo do disco: 0x%02x\n", sceCdGetDiskType());
    printf("diskReady: %d\n", sceCdDiskReady(0));

    printf("searchFile(\\DATA\\HELLO.TXT;1): %d", sceCdSearchFile(&f, "\\DATA\\HELLO.TXT;1"));
    printf(" lsn %u tamanho %u nome %s data %02d/%02d/%d\n", (unsigned)f.lsn, (unsigned)f.size, f.name, f.date[4],
           f.date[5], f.date[6] | (f.date[7] << 8));
    mode.trycount = 0;
    mode.spindlctrl = SCECdSpinNom;
    mode.datapattern = SCECdSecS2048;
    mode.pad = 0;
    memset(sectors, 0, sizeof(sectors));
    printf("sceCdRead: %d", sceCdRead(f.lsn, 1, sectors, &mode));
    printf(" sync %d erro %d: %s", sceCdSync(0), sceCdGetError(), sectors);

    printf("searchFile(\\DATA\\BIG.BIN;1): %d", sceCdSearchFile(&f, "\\DATA\\BIG.BIN;1"));
    printf(" tamanho %u\n", (unsigned)f.size);
    sceCdRead(f.lsn, 3, sectors, &mode);
    sceCdSync(0);
    for (i = 0, sum = 0; i < (int)f.size; i++) sum = (sum * 31 + sectors[i]) & 0xFFFFFF;
    printf("soma dos 3 setores: %06x\n", sum);
    printf("searchFile(\\NADA.BIN;1): %d\n", sceCdSearchFile(&f, "\\NADA.BIN;1"));

    if (sceCdReadClock(&clock))
        printf("relógio: 20%02x-%02x-%02x %02x:%02x\n", clock.year, clock.month, clock.day, clock.hour,
               clock.minute);

    /* fileio */
    fp = fopen("cdrom0:\\README.TXT;1", "r");
    printf("fopen cdrom0: %s\n", fp ? "ok" : "erro");
    while (fp && fgets(line, sizeof(line), fp)) printf("  > %s", line);
    if (fp) fclose(fp);
    fd = open("cdrom0:\\DATA\\BIG.BIN;1", O_RDONLY);
    printf("lseek fim: %d\n", (int)lseek(fd, 0, SEEK_END));
    lseek(fd, 4096 + 10, SEEK_SET);
    memset(line, 0, sizeof(line));
    printf("read no 3º setor: %d bytes, primeiro %u\n", (int)read(fd, line, 8), (unsigned char)line[0]);
    close(fd);
    printf("open para escrita: %s\n", open("cdrom0:\\README.TXT;1", O_WRONLY) < 0 ? "recusado" : "aceito?!");
    printf("stat DATA: %d", stat("cdrom0:\\DATA", &st));
    printf(" diretório: %s\n", S_ISDIR(st.st_mode) ? "sim" : "não");
    printf("stat README: %d tamanho %d\n", stat("cdrom0:\\README.TXT;1", &st), (int)st.st_size);
    dir = opendir("cdrom0:\\");
    printf("opendir raiz: %s\n", dir ? "ok" : "erro");
    while (dir && (de = readdir(dir)) != NULL) printf("  %s\n", de->d_name);
    if (dir) closedir(dir);
    printf("fim\n");
    return 0;
}
