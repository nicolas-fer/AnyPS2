/* execps2: um "boot" no estilo dos jogos comerciais. Linkado em 0x01000000
 * (LOADADDR), traz o execchild.elf embutido, copia os segmentos dele para a
 * memória e chama ExecPS2 — o código do filho só existe na memória em tempo
 * de execução. O teste recompila este ELF com "--extra execchild.elf".
 */
#include <kernel.h>
#include <stdio.h>
#include <string.h>

extern unsigned char child_elf[];

typedef struct {
    u8 ident[16];
    u16 type, machine;
    u32 version, entry, phoff, shoff, flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} ElfHeader;

typedef struct {
    u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
} ElfPhdr;

static void *child_thread(void *arg)
{
    (void)arg;
    return NULL;
}

int main(void)
{
    /* Estado que o ExecPS2 tem de descartar: semáforos e outra thread. São
     * 40 semáforos a mais para o id do último passar dos que as bibliotecas
     * do filho criam (os ids recomeçam do 1 depois do ExecPS2). */
    ee_sema_t s = {.init_count = 0, .max_count = 1};
    int sema = 0;
    for (int i = 0; i < 40; ++i)
        sema = CreateSema(&s);
    static u8 stack[0x1000] __attribute__((aligned(16)));
    ee_thread_t t = {.func = child_thread, .stack = stack, .stack_size = sizeof(stack),
                     .gp_reg = &_gp, .initial_priority = 64};
    StartThread(CreateThread(&t), NULL);

    const ElfHeader *eh = (const ElfHeader *)child_elf;
    const ElfPhdr *ph = (const ElfPhdr *)(child_elf + eh->phoff);
    for (int i = 0; i < eh->phnum; ++i) {
        if (ph[i].type != 1)
            continue;
        memcpy((void *)ph[i].vaddr, child_elf + ph[i].offset, ph[i].filesz);
        memset((u8 *)ph[i].vaddr + ph[i].filesz, 0, ph[i].memsz - ph[i].filesz);
    }
    FlushCache(0);
    FlushCache(2);

    static char sema_arg[16];
    sprintf(sema_arg, "%d", sema);
    static char *args[] = {"cdrom0:\\FILHO.ELF;1", sema_arg};
    printf("pai: ExecPS2(0x%x), semaforo %d\n", (unsigned)eh->entry, sema);
    ExecPS2((void *)eh->entry, NULL, 2, args);
    printf("pai: ExecPS2 voltou (nao deveria)\n");
    return 1;
}
