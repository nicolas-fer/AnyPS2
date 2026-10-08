/* execchild: o "programa principal" que o execps2 põe na memória e executa
 * com ExecPS2 (como o boot de um jogo faz com o executável descomprimido).
 * Mostra os argumentos recebidos e que o kernel começou do zero: a thread
 * principal volta a ser a 1 e o semáforo criado pelo programa anterior (id
 * em argv[1]) não existe mais.
 */
#include <kernel.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    printf("filho: argc=%d\n", argc);
    for (int i = 0; i < argc; ++i)
        printf("filho: argv[%d]=%s\n", i, argv[i]);
    printf("filho: thread principal = %d\n", GetThreadId());
    if (argc > 1) {
        ee_sema_t info;
        printf("filho: semaforo %s do pai ainda existe? %s\n", argv[1],
               ReferSemaStatus(atoi(argv[1]), &info) >= 0 ? "sim" : "nao");
    }
    return 0;
}
