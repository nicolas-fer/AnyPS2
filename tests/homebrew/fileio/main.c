/* Teste de E/S de arquivos via host: (fileio do IOP em HLE). */
#include <stdio.h>
#include <string.h>

int main(void)
{
    FILE *f;
    char line[64];
    long size;
    int n = 0;

    f = fopen("host:saida.txt", "w");
    if (!f) {
        printf("fopen(w) falhou\n");
        return 1;
    }
    fprintf(f, "linha %d\n", 1);
    fputs("segunda linha\n", f);
    fprintf(f, "%s=%d\n", "valor", 1234);
    fclose(f);

    f = fopen("host:saida.txt", "r");
    if (!f) {
        printf("fopen(r) falhou\n");
        return 1;
    }
    while (fgets(line, sizeof line, f)) printf("lido[%d]: %s", n++, line);
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 6, SEEK_SET);
    printf("tamanho=%ld byte6=%c\n", size, fgetc(f));
    fclose(f);

    f = fopen("host:nao_existe.txt", "r");
    printf("arquivo inexistente: %s\n", f ? "aberto?!" : "NULL");
    return 0;
}
