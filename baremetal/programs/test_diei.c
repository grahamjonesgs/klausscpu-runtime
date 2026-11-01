// test_diei — DI / EI, the core's interrupt-enable bit (ISA_ENCODING_V2_MAP.md
// §8.2, __builtin_klausscpu_di / _ei), against a timer interrupt.
//
//  T1  DI returns the old enable bit (1 after reset) and disables: no timer
//      interrupt is taken for many timer periods, then EI(1) takes one.
//  T2  DI nests: the inner DI returns 0, its EI(0) leaves interrupts off, the
//      outer EI(1) turns them on.
//  T3  EI uses bit 0 only.
//  T4  WAIT while disabled wakes on the pending interrupt but does not enter
//      the handler; the interrupt is taken at the EI.
//  T5  A handler runs with interrupts enabled (DI inside it returns 1), and
//      IRET restores the enable bit from the frame (bit 43): the handler
//      leaves them disabled, the interrupted code is enabled again.
//  T6  A critical section between DI and EI sees no interrupt: a counter
//      incremented by the handler is the same at both ends.
//
// INT_MASK keeps selecting the source (only the timer is unmasked here);
// nothing below writes it except to set the timer up.
#include <stdint.h>
#include <stdio.h>
#include "../../mmio.h"

#define TIMER_CYCLES 2000
#define SPIN         30000       // many timer periods

static int pass, fail;
static void check(const char *name, int ok) {
    printf("%-44s %s\n", name, ok ? "PASS" : "FAIL");
    if (ok) pass++; else fail++;
}

static volatile uint32_t ticks;
static volatile uint32_t isr_saw;        // what DI returned inside the handler
static volatile uint32_t isr_leave_off;  // handler ends with interrupts off

void timer_isr_c(void) {
    ticks++;
    if (isr_leave_off) isr_saw = __builtin_klausscpu_di();   // and no EI
}

__asm__(
    "  .text\n  .p2align 3\n  .globl diei_timer_isr\n"
    "diei_timer_isr:\n"
    "  push r0\n  push r1\n  push r2\n  push r3\n  push r8\n  push r9\n"
    "  push r10\n  push r11\n  push r12\n  push r13\n  push r14\n"
    "  call timer_isr_c\n"
    "  pop r14\n  pop r13\n  pop r12\n  pop r11\n  pop r10\n  pop r9\n"
    "  pop r8\n  pop r3\n  pop r2\n  pop r1\n  pop r0\n"
    "  iret\n");
extern void diei_timer_isr(void);

static void spin(void) {
    for (volatile uint32_t i = 0; i < SPIN; i++) {}
}

// Wait (enabled) for at least n more ticks; false if none comes.
static int wait_ticks(uint32_t n) {
    uint32_t t0 = ticks;
    for (uint32_t i = 0; i < 20 * SPIN && ticks - t0 < n; i++) {}
    return ticks - t0 >= n;
}

int main(void) {
    printf("\n=== test_diei: DI / EI ===\n");

    REG_INT_MASK = 0;
    REG_INT_VEC(INT_SRC_TIMER) = (uint32_t)(uintptr_t)diei_timer_isr;
    REG_TIMER_PER = TIMER_CYCLES;
    REG_INT_MASK = 1u << INT_SRC_TIMER;
    check("T0 timer interrupts arrive", wait_ticks(3));

    // T1
    unsigned k = __builtin_klausscpu_di();
    uint32_t t0 = ticks;
    spin();
    check("T1 DI returned 1 (was enabled)", k == 1);
    check("T1 no interrupt while disabled", ticks == t0);
    __builtin_klausscpu_ei(k);
    check("T1 EI(1) takes interrupts again", wait_ticks(2));

    // T2
    unsigned outer = __builtin_klausscpu_di();
    unsigned inner = __builtin_klausscpu_di();
    __builtin_klausscpu_ei(inner);                   // back to off
    t0 = ticks;
    spin();
    check("T2 inner DI returned 0", inner == 0);
    check("T2 EI(0) leaves interrupts off", ticks == t0);
    __builtin_klausscpu_ei(outer);
    check("T2 outer EI(1) turns them on", wait_ticks(2));

    // T3: only bit 0 of the EI operand counts.
    unsigned d = __builtin_klausscpu_di();
    __builtin_klausscpu_ei(2);                       // bit 0 = 0: still off
    t0 = ticks;
    spin();
    check("T3 EI(2) keeps interrupts off", ticks == t0);
    __builtin_klausscpu_ei(3);                       // bit 0 = 1: on
    check("T3 EI(3) turns them on", d == 1 && wait_ticks(2));

    // T4: WAIT wakes on the pending interrupt while disabled, without taking it.
    d = __builtin_klausscpu_di();
    t0 = ticks;
    __asm__ volatile("wait" ::: "memory");
    uint32_t t1 = ticks;
    __builtin_klausscpu_ei(d);
    check("T4 WAIT woke, handler not entered", t1 == t0);
    check("T4 interrupt taken at EI", wait_ticks(1));

    // T5: the handler runs enabled; IRET restores the bit from the frame.
    isr_saw = 99;
    isr_leave_off = 1;
    check("T5 handler ran", wait_ticks(1));
    isr_leave_off = 0;
    unsigned after = __builtin_klausscpu_di();
    __builtin_klausscpu_ei(after);
    check("T5 DI in the handler returned 1", isr_saw == 1);
    check("T5 IRET re-enabled the interrupted code", after == 1);

    // T6: no interrupt inside a critical section.
    k = __builtin_klausscpu_di();
    uint32_t a = ticks;
    spin();
    uint32_t b = ticks;
    __builtin_klausscpu_ei(k);
    check("T6 counter unchanged inside the section", a == b);
    check("T6 and moves outside it", wait_ticks(1));

    REG_INT_MASK = 0;
    REG_INT_VEC(INT_SRC_TIMER) = 0;
    printf("Results: %d pass, %d fail\n", pass, fail);
    printf("test_diei %s\n", fail ? "FAILED" : "PASSED");
    return fail != 0;
}
