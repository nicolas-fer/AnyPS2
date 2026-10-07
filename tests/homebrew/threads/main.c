/*
 * Teste do escalonador do kernel do EE (HLE): prioridade estrita,
 * preempção em syscalls, FIFO por prioridade, semáforos, Sleep/Wakeup,
 * Terminate + Start e retorno da função de entrada como ExitThread.
 * A saída esperada (expected.txt) segue as regras do kernel real.
 */
#include <kernel.h>
#include <stdio.h>

#define STACK_SIZE 0x4000
static u8 stacks[5][STACK_SIZE] __attribute__((aligned(16)));
static int sema, ping, pong, done;
static int main_id;

static int make_sema(int init)
{
    ee_sema_t s;
    s.init_count = init;
    s.max_count = 100;
    s.option = 0;
    return CreateSema(&s);
}

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

static void high(void *arg)
{
    printf("alta: começou (arg=%d)\n", (int)arg);
    WaitSema(sema);
    printf("alta: acordou\n");
}

static void equal(void *arg)
{
    (void)arg;
    printf("igual: rodou depois do Rotate\n");
    ExitThread();
}

static void low(void *arg)
{
    (void)arg;
    printf("baixa: acorda a principal\n");
    WakeupThread(main_id);
    printf("baixa: voltou\n");
    SignalSema(done);
}

static void pinger(void *arg)
{
    int i;
    (void)arg;
    for (i = 0; i < 3; i++) {
        WaitSema(ping);
        printf("ping %d\n", i);
        SignalSema(pong);
    }
}

static void victim(void *arg)
{
    printf("vítima: início %d\n", (int)arg);
    WaitSema(sema);
    printf("vítima: NÃO deveria chegar aqui\n");
}

int main(void)
{
    ee_thread_status_t st;
    int i, id;
    main_id = GetThreadId();
    ChangeThreadPriority(main_id, 50);
    sema = make_sema(0);
    ping = make_sema(0);
    pong = make_sema(0);
    done = make_sema(0);

    printf("1. prioridade maior preempta\n");
    spawn(0, high, 40, (void *)7);
    printf("principal: depois do StartThread\n");
    SignalSema(sema);
    printf("principal: depois do SignalSema\n");

    printf("2. mesma prioridade não preempta\n");
    spawn(1, equal, 50, 0);
    printf("principal: antes do Rotate\n");
    RotateThreadReadyQueue(50);
    printf("principal: depois do Rotate\n");

    printf("3. sleep/wakeup\n");
    spawn(2, low, 60, 0);
    printf("principal: dormindo\n");
    SleepThread();
    printf("principal: acordada\n");
    WaitSema(done);
    printf("principal: baixa terminou\n");

    printf("4. ping-pong\n");
    spawn(3, pinger, 45, 0);
    for (i = 0; i < 3; i++) {
        SignalSema(ping);
        WaitSema(pong);
        printf("pong %d\n", i);
    }

    printf("5. terminate + start\n");
    id = spawn(4, victim, 30, (void *)1);
    TerminateThread(id);
    ReferThreadStatus(id, &st);
    printf("principal: status depois de terminate=%d\n", st.status);
    StartThread(id, (void *)2);
    TerminateThread(id);
    ReferThreadStatus(main_id, &st);
    printf("principal: prioridade=%d\n", st.current_priority);
    printf("fim\n");
    return 0;
}
