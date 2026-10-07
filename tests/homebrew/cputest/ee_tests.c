/*
 * Testes que só existem no EE: chamam as funções de asm.S e comparam com
 * implementações de referência em C (MMI) ou com valores esperados da FPU do
 * PS2 (que não segue IEEE 754).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef union {
    uint64_t d[2];
    int64_t sd[2];
    uint32_t w[4];
    int32_t sw[4];
    uint16_t h[8];
    int16_t sh[8];
    uint8_t b[16];
    int8_t sb[16];
} __attribute__((aligned(16))) q128;

static int failures;

static void check_u32(const char *name, uint32_t got, uint32_t expected)
{
    if (got != expected) {
        printf("FALHA %s: obtido %08x, esperado %08x\n", name, (unsigned)got, (unsigned)expected);
        failures++;
    }
}

static void check_q(const char *name, const q128 *got, const q128 *expected)
{
    if (memcmp(got, expected, 16) != 0) {
        printf("FALHA %s: obtido %08x%08x%08x%08x esperado %08x%08x%08x%08x\n", name,
               (unsigned)got->w[3], (unsigned)got->w[2], (unsigned)got->w[1], (unsigned)got->w[0],
               (unsigned)expected->w[3], (unsigned)expected->w[2], (unsigned)expected->w[1],
               (unsigned)expected->w[0]);
        failures++;
    }
}

/* ---- funções de asm.S ------------------------------------------------------ */
extern int cf_likely_not_taken(void);
extern int cf_likely_taken(void);
extern int cf_delay_slot_is_target(int);
extern int cf_branch_reads_before_slot(void);
extern int cf_jr_reads_before_slot(void);
extern uint32_t cf_bal_pc(void);
extern int cf_jalr_rd(void);
extern int cf_movz_movn(int, int);
extern uint32_t mem_lw_unaligned(const uint8_t *);
extern void mem_ld_unaligned(const uint8_t *, uint64_t *);
extern void mem_sw_unaligned(uint8_t *, uint32_t);
extern void mem_sd_unaligned(uint8_t *, const uint64_t *);
typedef void (*mmi_fn)(const q128 *, const q128 *, q128 *);
#define MMI(n) extern void mmi_##n(const q128 *, const q128 *, q128 *);
MMI(paddw) MMI(psubh) MMI(paddsh) MMI(psubsb) MMI(paddub) MMI(psubuw) MMI(pcgtb) MMI(pceqh)
MMI(pmaxw) MMI(pminh) MMI(pextlw) MMI(pextuh) MMI(pextlb) MMI(ppach) MMI(ppacb) MMI(pinth)
MMI(pcpyld) MMI(pcpyud) MMI(pand) MMI(pnor) MMI(padsbh) MMI(pabsw) MMI(pext5) MMI(ppac5)
MMI(pexeh) MMI(prevh) MMI(prot3w) MMI(pexcw) MMI(pcpyh) MMI(psllh5) MMI(psraw7) MMI(psravw)
MMI(qfsrv5) MMI(pmultw) MMI(pmaddh) MMI(pdivw) MMI(pmfhl) MMI(plzcw)
extern void mul3(int, int, int *);
extern uint32_t fpu_add(uint32_t, uint32_t), fpu_mul(uint32_t, uint32_t), fpu_div(uint32_t, uint32_t);
extern uint32_t fpu_max(uint32_t, uint32_t), fpu_min(uint32_t, uint32_t), fpu_sqrt(uint32_t);
extern uint32_t fpu_cvtw(uint32_t), fpu_madd_acc(uint32_t, uint32_t);
extern int fpu_lt(uint32_t, uint32_t);

/* ---- referências em C -------------------------------------------------------- */
static int32_t sat32(int64_t v) { return v > 2147483647 ? 2147483647 : v < -2147483647 - 1 ? -2147483647 - 1 : (int32_t)v; }
static int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
static int8_t sat8(int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : (int8_t)v; }

static void ref(const char *op, const q128 *a, const q128 *b, q128 *d)
{
    int i;
    memset(d, 0, 16);
    if (!strcmp(op, "paddw")) for (i = 0; i < 4; i++) d->w[i] = a->w[i] + b->w[i];
    if (!strcmp(op, "psubh")) for (i = 0; i < 8; i++) d->h[i] = (uint16_t)(a->h[i] - b->h[i]);
    if (!strcmp(op, "paddsh")) for (i = 0; i < 8; i++) d->sh[i] = sat16(a->sh[i] + b->sh[i]);
    if (!strcmp(op, "psubsb")) for (i = 0; i < 16; i++) d->sb[i] = sat8(a->sb[i] - b->sb[i]);
    if (!strcmp(op, "paddub")) for (i = 0; i < 16; i++) { int v = a->b[i] + b->b[i]; d->b[i] = (uint8_t)(v > 255 ? 255 : v); }
    if (!strcmp(op, "psubuw")) for (i = 0; i < 4; i++) d->w[i] = a->w[i] > b->w[i] ? a->w[i] - b->w[i] : 0;
    if (!strcmp(op, "pcgtb")) for (i = 0; i < 16; i++) d->b[i] = a->sb[i] > b->sb[i] ? 0xFF : 0;
    if (!strcmp(op, "pceqh")) for (i = 0; i < 8; i++) d->h[i] = a->h[i] == b->h[i] ? 0xFFFF : 0;
    if (!strcmp(op, "pmaxw")) for (i = 0; i < 4; i++) d->sw[i] = a->sw[i] > b->sw[i] ? a->sw[i] : b->sw[i];
    if (!strcmp(op, "pminh")) for (i = 0; i < 8; i++) d->sh[i] = a->sh[i] < b->sh[i] ? a->sh[i] : b->sh[i];
    if (!strcmp(op, "pextlw")) { d->w[0] = b->w[0]; d->w[1] = a->w[0]; d->w[2] = b->w[1]; d->w[3] = a->w[1]; }
    if (!strcmp(op, "pextuh")) for (i = 0; i < 4; i++) { d->h[2 * i] = b->h[4 + i]; d->h[2 * i + 1] = a->h[4 + i]; }
    if (!strcmp(op, "pextlb")) for (i = 0; i < 8; i++) { d->b[2 * i] = b->b[i]; d->b[2 * i + 1] = a->b[i]; }
    if (!strcmp(op, "ppach")) for (i = 0; i < 4; i++) { d->h[i] = b->h[2 * i]; d->h[4 + i] = a->h[2 * i]; }
    if (!strcmp(op, "ppacb")) for (i = 0; i < 8; i++) { d->b[i] = b->b[2 * i]; d->b[8 + i] = a->b[2 * i]; }
    if (!strcmp(op, "pinth")) for (i = 0; i < 4; i++) { d->h[2 * i] = b->h[i]; d->h[2 * i + 1] = a->h[4 + i]; }
    if (!strcmp(op, "pcpyld")) { d->d[0] = b->d[0]; d->d[1] = a->d[0]; }
    if (!strcmp(op, "pcpyud")) { d->d[0] = a->d[1]; d->d[1] = b->d[1]; }
    if (!strcmp(op, "pand")) { d->d[0] = a->d[0] & b->d[0]; d->d[1] = a->d[1] & b->d[1]; }
    if (!strcmp(op, "pnor")) { d->d[0] = ~(a->d[0] | b->d[0]); d->d[1] = ~(a->d[1] | b->d[1]); }
    if (!strcmp(op, "padsbh")) for (i = 0; i < 8; i++) d->h[i] = (uint16_t)(i < 4 ? a->h[i] - b->h[i] : a->h[i] + b->h[i]);
    if (!strcmp(op, "pabsw")) for (i = 0; i < 4; i++) d->sw[i] = b->w[i] == 0x80000000u ? 0x7FFFFFFF : (b->sw[i] < 0 ? -b->sw[i] : b->sw[i]);
    if (!strcmp(op, "pext5")) for (i = 0; i < 4; i++) { uint32_t v = b->w[i]; d->w[i] = ((v & 0x1F) << 3) | (((v >> 5) & 0x1F) << 11) | (((v >> 10) & 0x1F) << 19) | (((v >> 15) & 1) << 31); }
    if (!strcmp(op, "ppac5")) for (i = 0; i < 4; i++) { uint32_t v = b->w[i]; d->w[i] = ((v >> 3) & 0x1F) | (((v >> 11) & 0x1F) << 5) | (((v >> 19) & 0x1F) << 10) | ((v >> 31) << 15); }
    if (!strcmp(op, "pexeh")) { static const int k[8] = {2, 1, 0, 3, 6, 5, 4, 7}; for (i = 0; i < 8; i++) d->h[i] = b->h[k[i]]; }
    if (!strcmp(op, "prevh")) { static const int k[8] = {3, 2, 1, 0, 7, 6, 5, 4}; for (i = 0; i < 8; i++) d->h[i] = b->h[k[i]]; }
    if (!strcmp(op, "prot3w")) { d->w[0] = b->w[1]; d->w[1] = b->w[2]; d->w[2] = b->w[0]; d->w[3] = b->w[3]; }
    if (!strcmp(op, "pexcw")) { d->w[0] = b->w[0]; d->w[1] = b->w[2]; d->w[2] = b->w[1]; d->w[3] = b->w[3]; }
    if (!strcmp(op, "pcpyh")) for (i = 0; i < 8; i++) d->h[i] = b->h[i < 4 ? 0 : 4];
    if (!strcmp(op, "psllh5")) for (i = 0; i < 8; i++) d->h[i] = (uint16_t)(b->h[i] << 5);
    if (!strcmp(op, "psraw7")) for (i = 0; i < 4; i++) d->sw[i] = b->sw[i] >> 7;
    if (!strcmp(op, "psravw")) for (i = 0; i < 2; i++) d->sd[i] = (int64_t)(b->sw[2 * i] >> (a->w[2 * i] & 31));
    if (!strcmp(op, "qfsrv5")) { uint8_t t[32]; memcpy(t, b, 16); memcpy(t + 16, a, 16); memcpy(d, t + 5, 16); }
}

static const q128 inputs[][2] = {
    {{.w = {0x00000001u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu}}, {.w = {0x00000002u, 0x00000001u, 0x80000000u, 0x00000001u}}},
    {{.h = {0x7FFF, 0x8000, 0xFFFF, 0x0001, 0x1234, 0xFEDC, 0x4000, 0xC000}}, {.h = {0x0001, 0x0001, 0x8000, 0x7FFF, 0x4321, 0x0123, 0x4000, 0xC000}}},
    {{.b = {0x7F, 0x80, 0xFF, 0x00, 0x01, 0x10, 0x40, 0xC0, 0x55, 0xAA, 0x0F, 0xF0, 0x7E, 0x81, 0x02, 0xFE}},
     {.b = {0x01, 0x01, 0x01, 0xFF, 0x80, 0x7F, 0x40, 0xC0, 0xAA, 0x55, 0xF0, 0x0F, 0x02, 0x80, 0x7F, 0x80}}},
    {{.d = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull}}, {.d = {0x0000000500000003ull, 0x8000001F00000021ull}}},
};

static void test_mmi(void)
{
    static const struct { const char *name; mmi_fn fn; } ops[] = {
        {"paddw", mmi_paddw}, {"psubh", mmi_psubh}, {"paddsh", mmi_paddsh}, {"psubsb", mmi_psubsb},
        {"paddub", mmi_paddub}, {"psubuw", mmi_psubuw}, {"pcgtb", mmi_pcgtb}, {"pceqh", mmi_pceqh},
        {"pmaxw", mmi_pmaxw}, {"pminh", mmi_pminh}, {"pextlw", mmi_pextlw}, {"pextuh", mmi_pextuh},
        {"pextlb", mmi_pextlb}, {"ppach", mmi_ppach}, {"ppacb", mmi_ppacb}, {"pinth", mmi_pinth},
        {"pcpyld", mmi_pcpyld}, {"pcpyud", mmi_pcpyud}, {"pand", mmi_pand}, {"pnor", mmi_pnor},
        {"padsbh", mmi_padsbh}, {"pabsw", mmi_pabsw}, {"pext5", mmi_pext5}, {"ppac5", mmi_ppac5},
        {"pexeh", mmi_pexeh}, {"prevh", mmi_prevh}, {"prot3w", mmi_prot3w}, {"pexcw", mmi_pexcw},
        {"pcpyh", mmi_pcpyh}, {"psllh5", mmi_psllh5}, {"psraw7", mmi_psraw7}, {"psravw", mmi_psravw},
        {"qfsrv5", mmi_qfsrv5},
    };
    unsigned i, j;
    int count = 0;
    for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        for (j = 0; j < sizeof(inputs) / sizeof(inputs[0]); j++) {
            q128 got, expected;
            char name[48];
            ops[i].fn(&inputs[j][0], &inputs[j][1], &got);
            ref(ops[i].name, &inputs[j][0], &inputs[j][1], &expected);
            snprintf(name, sizeof name, "%s[%u]", ops[i].name, j);
            check_q(name, &got, &expected);
            count++;
        }
    }
    /* Multiplicação/divisão paralelas e HI/LO. */
    {
        q128 out[3], e;
        const q128 a = {.sw = {-3, 99, 100000, 7}}, b = {.sw = {5, 99, -100000, 9}};
        mmi_pmultw(&a, &b, out);
        e.sd[0] = -15; e.sd[1] = -10000000000ll;
        check_q("pmultw rd", &out[0], &e);
        e.sd[0] = -1; e.sd[1] = (int32_t)0xFFFFFFFDu;  /* HI = palavra alta estendida */
        check_q("pmultw hi", &out[1], &e);
        e.sd[0] = -15; e.sd[1] = (int32_t)(uint32_t)(-10000000000ll);
        check_q("pmultw lo", &out[2], &e);
    }
    {
        q128 out[3], e;
        const q128 a = {.sh = {1, -2, 3, -4, 300, 400, -500, 600}}, b = {.sh = {10, 20, 30, 40, 1000, -1000, 1000, 1000}};
        mmi_pmaddh(&a, &b, out);
        /* duas acumulações: produtos * 2 */
        e.sw[0] = 2 * 10; e.sw[1] = 2 * 90; e.sw[2] = 2 * 300000; e.sw[3] = 2 * -500000;
        check_q("pmaddh rd", &out[0], &e);
        e.sw[0] = 2 * 90; e.sw[1] = 2 * -160; e.sw[2] = 2 * -500000; e.sw[3] = 2 * 600000;
        check_q("pmaddh hi", &out[1], &e);
        e.sw[0] = 2 * 10; e.sw[1] = 2 * -40; e.sw[2] = 2 * 300000; e.sw[3] = 2 * -400000;
        check_q("pmaddh lo", &out[2], &e);
    }
    {
        q128 out[2], e;
        const q128 a = {.sw = {-17, 0, 100, 0}}, b = {.sw = {5, 0, -7, 0}};
        mmi_pdivw(&a, &b, out);
        e.sd[0] = -2; e.sd[1] = 2;       /* restos */
        check_q("pdivw hi", &out[0], &e);
        e.sd[0] = -3; e.sd[1] = -14;     /* quocientes */
        check_q("pdivw lo", &out[1], &e);
    }
    {
        q128 out[5], e;
        const q128 hi = {.w = {0x00000000u, 0x11112222u, 0xFFFFFFFFu, 0x33334444u}};
        const q128 lo = {.w = {0x7FFFFFFFu, 0x55556666u, 0x80000000u, 0x00012345u}};
        int k;
        mmi_pmfhl(&hi, &lo, out);
        e.w[0] = lo.w[0]; e.w[1] = hi.w[0]; e.w[2] = lo.w[2]; e.w[3] = hi.w[2];
        check_q("pmfhl.lw", &out[0], &e);
        e.w[0] = lo.w[1]; e.w[1] = hi.w[1]; e.w[2] = lo.w[3]; e.w[3] = hi.w[3];
        check_q("pmfhl.uw", &out[1], &e);
        e.sd[0] = sat32((int64_t)(((uint64_t)hi.w[0] << 32) | lo.w[0]));
        e.sd[1] = sat32((int64_t)(((uint64_t)hi.w[2] << 32) | lo.w[2]));
        check_q("pmfhl.slw", &out[2], &e);
        e.h[0] = lo.h[0]; e.h[1] = lo.h[2]; e.h[2] = hi.h[0]; e.h[3] = hi.h[2];
        e.h[4] = lo.h[4]; e.h[5] = lo.h[6]; e.h[6] = hi.h[4]; e.h[7] = hi.h[6];
        check_q("pmfhl.lh", &out[3], &e);
        {
            const int32_t src[8] = {lo.sw[0], lo.sw[1], hi.sw[0], hi.sw[1], lo.sw[2], lo.sw[3], hi.sw[2], hi.sw[3]};
            for (k = 0; k < 8; k++) e.sh[k] = sat16(src[k]);
        }
        check_q("pmfhl.sh", &out[4], &e);
    }
    {
        q128 out, e;
        const q128 a = {.w = {0x00000001u, 0xFFFF0000u, 0, 0}};
        mmi_plzcw(&a, &a, &out);
        e.d[0] = 0; e.d[1] = 0;
        e.w[0] = 30;  /* 0x00000001: 31 zeros à esquerda -> 30 */
        e.w[1] = 15;  /* 0xFFFF0000: 16 uns -> 15 */
        check_q("plzcw", &out, &e);
    }
    {
        int r[5];
        mul3(-7, 6, r);
        check_u32("mult rd", (uint32_t)r[0], (uint32_t)-42);
        check_u32("mult1/mflo1", (uint32_t)r[1], (uint32_t)-42);
        check_u32("div1 resto", (uint32_t)r[2], (uint32_t)-1);
        check_u32("div1 quociente", (uint32_t)r[3], (uint32_t)-1);
        check_u32("madd x2", (uint32_t)r[4], (uint32_t)-84);
    }
    (void)count;
}

static void test_control_flow(void)
{
    check_u32("beql não tomado", (uint32_t)cf_likely_not_taken(), 1);
    check_u32("beql tomado", (uint32_t)cf_likely_taken(), 7);
    check_u32("delay slot alvo (x=0)", (uint32_t)cf_delay_slot_is_target(0), 100);
    check_u32("delay slot alvo (x=1)", (uint32_t)cf_delay_slot_is_target(1), 110);
    check_u32("condição antes do slot", (uint32_t)cf_branch_reads_before_slot(), 1);
    check_u32("jr antes do slot", (uint32_t)cf_jr_reads_before_slot(), 5);
    check_u32("bal pc", cf_bal_pc(), 8);
    check_u32("jalr rd", (uint32_t)cf_jalr_rd(), 21);
    check_u32("movz/movn 0,0", (uint32_t)cf_movz_movn(0, 0), 11);
    check_u32("movz/movn 1,1", (uint32_t)cf_movz_movn(1, 1), 22);
}

static void test_unaligned(void)
{
    static uint8_t buf[32] __attribute__((aligned(16)));
    int off, i;
    for (off = 0; off < 8; off++) {
        uint32_t w, wexp;
        uint64_t d, dexp;
        char name[32];
        for (i = 0; i < 32; i++) buf[i] = (uint8_t)(i * 17 + 3);
        w = mem_lw_unaligned(buf + off);
        memcpy(&wexp, buf + off, 4);
        snprintf(name, sizeof name, "lwl/lwr +%d", off);
        check_u32(name, w, wexp);
        mem_ld_unaligned(buf + off, &d);
        memcpy(&dexp, buf + off, 8);
        snprintf(name, sizeof name, "ldl/ldr +%d", off);
        check_u32(name, (uint32_t)(d ^ (d >> 32)), (uint32_t)(dexp ^ (dexp >> 32)));
        mem_sw_unaligned(buf + off + 8, 0xA1B2C3D4u);
        memcpy(&w, buf + off + 8, 4);
        snprintf(name, sizeof name, "swl/swr +%d", off);
        check_u32(name, w, 0xA1B2C3D4u);
        dexp = 0x1122334455667788ull;
        mem_sd_unaligned(buf + off + 16, &dexp);
        memcpy(&d, buf + off + 16, 8);
        snprintf(name, sizeof name, "sdl/sdr +%d", off);
        check_u32(name, (uint32_t)(d ^ (d >> 32)), (uint32_t)(dexp ^ (dexp >> 32)));
        /* bytes vizinhos intactos */
        snprintf(name, sizeof name, "vizinho +%d", off);
        check_u32(name, buf[off + 16 + 8], (uint8_t)((off + 24) * 17 + 3));
    }
}

static void test_fpu(void)
{
    check_u32("add.s", fpu_add(0x3FC00000u, 0x40100000u), 0x40700000u);         /* 1.5 + 2.25 */
    check_u32("mul.s overflow -> +Fmax", fpu_mul(0x7F7FFFFFu, 0x40000000u), 0x7F7FFFFFu);
    check_u32("div.s 1/0 -> +Fmax", fpu_div(0x3F800000u, 0x00000000u), 0x7F7FFFFFu);
    check_u32("div.s -1/0 -> -Fmax", fpu_div(0xBF800000u, 0x00000000u), 0xFF7FFFFFu);
    check_u32("div.s 6/4", fpu_div(0x40C00000u, 0x40800000u), 0x3FC00000u);
    check_u32("max.s", fpu_max(0xBF800000u, 0x40000000u), 0x40000000u);
    check_u32("min.s", fpu_min(0xBF000000u, 0xC0400000u), 0xC0400000u);
    check_u32("sqrt.s 16", fpu_sqrt(0x41800000u), 0x40800000u);
    check_u32("sqrt.s -4 -> 2", fpu_sqrt(0xC0800000u), 0x40000000u);
    check_u32("cvt.w.s 1e20 satura", fpu_cvtw(0x60AD78ECu), 0x7FFFFFFFu);
    check_u32("cvt.w.s -3.75 trunca", fpu_cvtw(0xC0700000u), (uint32_t)-3);
    check_u32("cvt.w.s 2.5", fpu_cvtw(0x40200000u), 2);
    check_u32("mula/madd", fpu_madd_acc(0x40400000u, 0x40800000u), 0x41C00000u); /* 3*4 + 3*4 = 24 */
    check_u32("c.lt 1<2", (uint32_t)fpu_lt(0x3F800000u, 0x40000000u), 1);
    check_u32("c.lt 2<1", (uint32_t)fpu_lt(0x40000000u, 0x3F800000u), 0);
}

int run_asm_tests(void)
{
    failures = 0;
    test_control_flow();
    test_unaligned();
    test_mmi();
    test_fpu();
    return failures;
}
