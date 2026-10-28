/*
 * test_task_exit.c — RTOS tasks that end (src/rtos.c).
 *
 *   ret_task  returns from its entry function, after using its frame and
 *             the callee-saved registers (work() recurses with a local
 *             array).  It comes back to rtos_task_return (context_switch.S;
 *             the return address task_create puts above the initial frame),
 *             which calls rtos_task_exit.  Before that address existed its
 *             RET popped the word above its stack (the next task's stack
 *             canary) and jumped there.
 *   exit_task calls rtos_task_exit() itself.
 *   checker   waits for both, then checks over many ticks that neither is
 *             scheduled again while it keeps running, and exits: the
 *             emulator stops with status 0 on success.
 */

#include <stdio.h>
#include <stdlib.h>
#include "../../mmio.h"
#include "../../rtos.h"

static volatile int ret_done, exit_done;

static __attribute__((noinline)) long work(long depth, long seed) {
    volatile long buf[16];
    for (int i = 0; i < 16; i++)
        buf[i] = seed * (i + 1);
    long s = 0;
    for (int i = 0; i < 16; i++)
        s += buf[i];
    return depth > 0 ? s + work(depth - 1, seed + 1) : s;
}

static void ret_task(void) {
    long r = work(6, 3);
    rtos_mutex_lock();
    printf("[ret ] work = %ld, returning\n", r);
    rtos_mutex_unlock();
    ret_done = 1;
    /* falls off the end: returns to rtos_task_exit */
}

static void exit_task(void) {
    rtos_mutex_lock();
    printf("[exit] calling rtos_task_exit\n");
    rtos_mutex_unlock();
    exit_done = 1;
    rtos_task_exit();
}

static void checker(void) {
    while (!(ret_done && exit_done))
        rtos_yield();
    uint32_t s0 = rtos_task_switches(0), s1 = rtos_task_switches(1);
    uint32_t t0 = g_tick_count, me = rtos_task_switches(2);
    /* Spin across several ticks (g_tick_count is not volatile), then yield
       many times. */
    while (*(volatile uint32_t *)&g_tick_count - t0 < 5) {
    }
    for (int i = 0; i < 100; i++)
        rtos_yield();
    int ok = rtos_task_switches(0) == s0 && rtos_task_switches(1) == s1 &&
             rtos_task_switches(2) > me + 100;
    printf("ended tasks scheduled again: %s\n", ok ? "no" : "YES");
    printf("test_task_exit %s\n", ok ? "PASSED" : "FAILED");
    exit(ok ? 0 : 1);
}

int main(void) {
    printf("=== KlaussCPU RTOS: tasks that end ===\n");
    rtos_init();
    task_create("ret", ret_task);
    task_create("exit", exit_task);
    task_create("checker", checker);
    rtos_start();
    return 1;
}
