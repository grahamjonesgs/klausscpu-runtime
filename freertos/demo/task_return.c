/*
 * task_return.c — a FreeRTOS task function that returns.
 *
 * A task must not return (it should call vTaskDelete(NULL)), but one that
 * does comes back to the port's configTASK_RETURN_ADDRESS (vPortTaskReturn,
 * portable/KlaussCPU/port.S), which deletes it.  Before that return address
 * existed the task's RET popped the word above its stack and jumped there.
 *
 *   Returner   (priority 2): runs, records that it ran, and returns, from
 *              three different depths of its own frame (three tasks), one of
 *              them with a value in the callee-saved registers.
 *   Checker    (priority 1): waits until every returner is deleted (the idle
 *              task frees them), checks that the others keep running and
 *              that the heap got the stacks back, then exits (HALT): the
 *              emulator stops with status 0 on success.
 *
 * Run: make task_return.elf && klausscc --emulate -i task_return.elf
 */

#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"

#define NRET 3

static TaskHandle_t returners[NRET];
static volatile int ran[NRET];

/* Keep the frame and the callee-saved registers busy before returning. */
static __attribute__((noinline)) long work(long depth, long seed) {
    volatile long buf[16];
    for (int i = 0; i < 16; i++)
        buf[i] = seed * (i + 1);
    long s = 0;
    for (int i = 0; i < 16; i++)
        s += buf[i];
    return depth > 0 ? s + work(depth - 1, seed + 1) : s;
}

static void Returner(void *pvParam) {
    long id = (long)pvParam;
    long r = work(id * 2, id + 1);
    printf("[R%ld] work = %ld, returning\n", id, r);
    ran[id] = 1;
    /* falls off the end: returns to configTASK_RETURN_ADDRESS */
}

static void Checker(void *pvParam) {
    (void)pvParam;
    size_t heap_before = xPortGetFreeHeapSize();
    for (long i = 0; i < NRET; i++)
        xTaskCreate(Returner, "R", 256, (void *)i, 2, &returners[i]);

    /* The returners run first (higher priority) and delete themselves. */
    int ok = 1;
    for (int i = 0; i < NRET; i++) {
        eTaskState st = eTaskGetState(returners[i]);
        if (!ran[i] || st != eDeleted) {
            printf("FAIL: returner %d ran=%d state=%d\n", i, ran[i], (int)st);
            ok = 0;
        }
    }

    /* Let the idle task free their TCBs and stacks. */
    vTaskDelay(3);
    size_t heap_after = xPortGetFreeHeapSize();
    printf("heap: %s (%lu before the returners, %lu after)\n",
           heap_after == heap_before ? "all returned" : "LEAKED",
           (unsigned long)heap_before, (unsigned long)heap_after);
    if (heap_after != heap_before)
        ok = 0;
    printf("tasks left: %lu\n", (unsigned long)uxTaskGetNumberOfTasks());
    printf("task_return %s\n", ok ? "PASSED" : "FAILED");
    exit(ok ? 0 : 1);
}

void vAssertCalled(const char *file, unsigned long line) {
    printf("ASSERT FAILED: %s:%lu\n", file, line);
    exit(2);
}

int main(void) {
    printf("=== FreeRTOS: a task that returns ===\n");
    xTaskCreate(Checker, "Checker", 512, NULL, 1, NULL);
    vTaskStartScheduler();
    printf("ERROR: scheduler returned\n");
    return 1;
}
