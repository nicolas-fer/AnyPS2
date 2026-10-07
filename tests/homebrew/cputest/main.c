/*
 * Teste de ponta a ponta da CPU recompilada.
 *
 * Este arquivo compila tanto para o PS2 (ps2dev, -D_EE) quanto para o host
 * (gcc/clang/MSVC). A saída do host é o oráculo: se o código recompilado do
 * EE imprimir exatamente a mesma coisa, aritmética de 32/64 bits, controle de
 * fluxo, libc e ponto flutuante (valores exatos) estão corretos.
 *
 * As seções que só existem no EE (MMI, FPU do R5900, asm) ficam em asm.S e
 * se auto-verificam contra uma implementação de referência em C.
 */
#include <inttypes.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_hash = 1469598103934665603ULL;

static void mix(uint64_t v)
{
    g_hash ^= v;
    g_hash *= 1099511628211ULL;
}

/* volatile impede o compilador de dobrar as contas em tempo de compilação */
static volatile int32_t vs32[] = {0, 1, -1, 7, -7, 1000000007, -2147483647 - 1, 2147483647, 12345, -98765};
static volatile uint32_t vu32[] = {0u, 1u, 3u, 0x80000000u, 0xFFFFFFFFu, 0xDEADBEEFu, 65536u, 1000u};
static volatile int64_t vs64[] = {0, 1, -1, 3, -3, 0x123456789ABCDEFLL, -0x7FFFFFFFFFFFFFFFLL - 1, 0x7FFFFFFFFFFFFFFFLL, 1000000000000LL};

static void test_integers(void)
{
    int n32 = (int)(sizeof(vs32) / sizeof(vs32[0]));
    int nu = (int)(sizeof(vu32) / sizeof(vu32[0]));
    int n64 = (int)(sizeof(vs64) / sizeof(vs64[0]));
    int i, j;
    for (i = 0; i < n32; i++) {
        for (j = 0; j < n32; j++) {
            int32_t a = vs32[i], b = vs32[j];
            mix((uint32_t)a + (uint32_t)b);
            mix((uint32_t)((uint32_t)a * (uint32_t)b));
            mix((uint64_t)((int64_t)a * (int64_t)b));
            if (b != 0 && !(a == (-2147483647 - 1) && b == -1)) {
                mix((uint32_t)(a / b));
                mix((uint32_t)(a % b));
            }
            mix((uint32_t)(a < b));
            mix((uint32_t)a >> (j & 31));
            mix((uint32_t)(a >> (j & 31)));
            mix((uint32_t)a << (i & 31));
        }
    }
    for (i = 0; i < nu; i++) {
        for (j = 0; j < nu; j++) {
            uint32_t a = vu32[i], b = vu32[j];
            mix((uint64_t)a * b);
            if (b) {
                mix(a / b);
                mix(a % b);
            }
            mix(a < b);
            mix(a ^ (b >> 3) ^ (a << 5));
        }
    }
    for (i = 0; i < n64; i++) {
        for (j = 0; j < n64; j++) {
            int64_t a = vs64[i], b = vs64[j];
            mix((uint64_t)a + (uint64_t)b);
            mix((uint64_t)a * (uint64_t)b);
            if (b != 0 && !(a == (-0x7FFFFFFFFFFFFFFFLL - 1) && b == -1)) {
                mix((uint64_t)(a / b));
                mix((uint64_t)(a % b));
                mix((uint64_t)a / (uint64_t)b);
            }
            mix((uint64_t)(a < b));
            mix((uint64_t)a >> (j & 63));
            mix((uint64_t)(a >> (j & 63)));
            mix((uint64_t)a << (i & 63));
        }
    }
    printf("inteiros: hash=%016" PRIx64 "\n", g_hash);
    printf("inteiros: 7/-2=%d 7%%-2=%d -7/2=%d -7%%2=%d\n", (int)(vs32[3] / -2), (int)(vs32[3] % -2), (int)(vs32[4] / 2), (int)(vs32[4] % 2));
    printf("inteiros: 64 bits %" PRId64 " %" PRIu64 " %" PRIx64 "\n", vs64[5] * 3, (uint64_t)vs64[6] / 7, (uint64_t)vs64[7] >> 13);
}

/* ---- Controle de fluxo -------------------------------------------------- */

static int classify(int x)
{
    /* switch denso: o gcc gera jump table em .rodata */
    switch (x) {
        case 0: return 11;
        case 1: return 23;
        case 2: return 37;
        case 3: return 41;
        case 4: return 59;
        case 5: return 61;
        case 6: return 73;
        case 7: return 89;
        case 8: return 97;
        case 9: return 101;
        default: return -1;
    }
}

static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

static int op_add(int a, int b) { return a + b; }
static int op_sub(int a, int b) { return a - b; }
static int op_mul(int a, int b) { return a * b; }
static int (*const ops[])(int, int) = {op_add, op_sub, op_mul};

static jmp_buf jb;
static int depth_reached;

static void dive(int depth)
{
    depth_reached = depth;
    if (depth == 25) longjmp(jb, 42);
    if (depth < 1000) dive(depth + 1);
}

static int cmp_int(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static void test_control_flow(void)
{
    int i, sum = 0, r;
    for (i = -2; i < 12; i++) sum = sum * 3 + classify(i);
    printf("fluxo: switch=%d\n", sum);
    printf("fluxo: fib(20)=%d\n", fib(20));
    sum = 0;
    for (i = 0; i < 30; i++) sum = ops[i % 3](sum, i + 1) % 100003;
    printf("fluxo: ponteiros=%d\n", sum);
    r = setjmp(jb);
    if (r == 0) {
        dive(0);
        printf("fluxo: longjmp NÃO voltou\n");
    } else {
        printf("fluxo: longjmp=%d profundidade=%d\n", r, depth_reached);
    }
    {
        int v[16] = {9, -3, 7, 100, 0, 42, -77, 5, 5, 13, 8, -1, 64, 2, 31, 17};
        qsort(v, 16, sizeof(int), cmp_int);
        printf("fluxo: qsort=");
        for (i = 0; i < 16; i++) printf("%d%s", v[i], i == 15 ? "\n" : ",");
    }
}

/* ---- libc ------------------------------------------------------------------ */

static void test_libc(void)
{
    char buf[128];
    char *p;
    int i;
    snprintf(buf, sizeof buf, "%08x|%-5s|%+d|%5.2s|%c", 0xBEEFu, "ab", 17, "xyz", 'Q');
    printf("libc: snprintf=[%s]\n", buf);
    printf("libc: strtol=%ld %ld %lu\n", strtol("-12345", NULL, 10), strtol("7fff", NULL, 16), strtoul("0777", NULL, 0));
    p = malloc(1000);
    for (i = 0; i < 1000; i++) p[i] = (char)(i * 7);
    p = realloc(p, 5000);
    memmove(p + 1, p, 999);
    for (i = 0; i < 1000; i++) mix((unsigned char)p[i]);
    free(p);
    printf("libc: memória hash=%016" PRIx64 "\n", g_hash);
    strcpy(buf, "PlayStation 2");
    printf("libc: strlen=%d strcmp=%d strchr=%s\n", (int)strlen(buf), strcmp(buf, "PlayStation 3") < 0, strchr(buf, 'S'));
}

/* ---- Ponto flutuante (valores exatos: independem do arredondamento) -------- */

static volatile float vf[] = {1.5f, -2.25f, 0.5f, 1024.0f, 3.0f, -0.125f};
static volatile double vd[] = {1.5, -2.25, 0.5, 1e10, 3.0, -0.125};

static void test_float(void)
{
    float a = vf[0], b = vf[1];
    double c = vd[3], d = vd[4];
    printf("float: %.4f %.4f %.4f %.4f\n", a + b, a * b, vf[3] / vf[2], b - vf[5]);
    printf("float: %d %d %d\n", (int)(vf[3] * 3.5f), (int)b, a < b);
    printf("double: %.3f %.3f %.6e %.1f\n", c + d, c * vd[2], c / vd[2], d * d * d);
    printf("double: %d %lld\n", (int)(vd[1] * 4.0), (long long)(c * 3.0));
}

#ifdef _EE
extern int run_asm_tests(void);
#endif

int main(int argc, char *argv[])
{
    (void)argv;
    printf("cputest: argc=%d\n", argc);
    test_integers();
    test_control_flow();
    test_libc();
    test_float();
#ifdef _EE
    printf("asm: %d falhas\n", run_asm_tests());
#else
    printf("asm: 0 falhas\n");
#endif
    printf("cputest: fim\n");
    return 0;
}
