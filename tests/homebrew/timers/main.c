/*
 * Teste da Fase 3: tempo e interrupções.
 *  1. DelayThread (sistema de timers do ps2sdk sobre o Timer 2 + interrupção)
 *  2. SetAlarm/ReleaseAlarm do kernel
 *  3. handler de VBlank (INTC) e espera ativa em GS_CSR.VSINT
 *  4. interrupção entregue no meio de um laço sem syscalls
 *  5. preempção: um handler acorda uma thread de prioridade maior
 * A saída só depende da ORDEM dos eventos, então vale nos dois relógios
 * (virtual e real).
 */
#include <delaythread.h>
#include <kernel.h>
#include <stdio.h>
#include <timer.h>

#define GS_CSR ((volatile u64 *)0x12001000)
#define STACK_SIZE 0x4000

static u8 stacks[4][STACK_SIZE] __attribute__((aligned(16)));
static int done_sema, alarm_sema, wake_order[3], wake_count;
static volatile int vblank_count, busy_flag, high_ran, alarm2_fired;
static int high_id;

static int spawn(int index, void (*fn)(void *), int prio, void *arg)
{
    ee_thread_t t;
    int id;
    t.func = fn;
    t.stack = stacks[index];
    t.stack_size = STACK_SIZE;
    t.gp_reg = &_gp;
    t.initial_priority = prio;
    t.attr = 0;
    t.option = 0;
    id = CreateThread(&t);
    StartThread(id, arg);
    return id;
}

static int make_sema(int init)
{
    ee_sema_t s;
    s.init_count = init;
    s.max_count = 100;
    s.option = 0;
    return CreateSema(&s);
}

/* 1. três threads dormem tempos diferentes: acordam em ordem de duração */
static void sleeper(void *arg)
{
    int ms = (int)arg;
    DelayThread(ms * 1000);
    wake_order[wake_count++] = ms;
    SignalSema(done_sema);
}

/* 2. alarmes do kernel */
static void alarm_handler(s32 id, u16 time, void *arg)
{
    (void)id;
    (void)time;
    if ((int)arg == 2) alarm2_fired = 1;
    iSignalSema(alarm_sema);
}

/* 3/4. VBlank */
static int vblank_handler(int cause)
{
    (void)cause;
    vblank_count++;
    if (vblank_count == 3) busy_flag = 1;
    return 0;
}

/* 5. thread de prioridade alta acordada por um alarme */
static void high_thread(void *arg)
{
    (void)arg;
    SleepThread();
    high_ran = 1;
    printf("alta: rodou, a principal estava no laço\n");
}

static void wake_high(s32 id, u16 time, void *arg)
{
    (void)id;
    (void)time;
    (void)arg;
    iWakeupThread(high_id);
}

int main(void)
{
    int i, vh, a1, a2, polls = 0;
    u64 t0, t1;

    ChangeThreadPriority(GetThreadId(), 50);
    done_sema = make_sema(0);
    alarm_sema = make_sema(0);

    printf("1. DelayThread\n");
    spawn(0, sleeper, 40, (void *)30);
    spawn(1, sleeper, 40, (void *)10);
    spawn(2, sleeper, 40, (void *)20);
    for (i = 0; i < 3; i++) WaitSema(done_sema);
    printf("ordem: %d %d %d\n", wake_order[0], wake_order[1], wake_order[2]);
    t0 = GetTimerSystemTime();
    DelayThread(50000);
    t1 = GetTimerSystemTime();
    printf("DelayThread(50 ms) durou >= 50 ms: %s\n", (t1 - t0) >= (u64)(kBUSCLK / 20) ? "sim" : "não");

    printf("2. SetAlarm\n");
    a1 = SetAlarm(200, alarm_handler, (void *)1);
    a2 = SetAlarm(400, alarm_handler, (void *)2);
    printf("alarmes criados: %s\n", (a1 >= 0 && a2 >= 0 && a1 != a2) ? "sim" : "não");
    ReleaseAlarm(a2);
    WaitSema(alarm_sema);
    DelayThread(100000); /* o alarme 2 já teria disparado */
    printf("alarme 1 disparou, alarme 2 cancelado: %s\n", alarm2_fired ? "não" : "sim");

    printf("3. VBlank\n");
    vh = AddIntcHandler(INTC_VBLANK_S, vblank_handler, 0);
    EnableIntc(INTC_VBLANK_S);
    /* 4. laço sem syscall: só termina se a interrupção for entregue no meio dele */
    while (!busy_flag) {
    }
    printf("handler de VBlank chamado %d vezes durante o laço\n", vblank_count);
    for (i = 0; i < 3; i++) {
        *GS_CSR = 8; /* limpa VSINT */
        while (!(*GS_CSR & 8)) polls++;
    }
    printf("espera em GS_CSR: 3 VBlanks, %s\n", polls > 0 ? "com espera" : "sem espera?!");
    DisableIntc(INTC_VBLANK_S);
    RemoveIntcHandler(INTC_VBLANK_S, vh);

    printf("5. preempção por interrupção\n");
    high_id = spawn(3, high_thread, 10, 0);
    SetAlarm(50, wake_high, 0);
    while (!high_ran) {
    }
    printf("principal: saiu do laço depois da alta\n");
    printf("fim\n");
    return 0;
}
