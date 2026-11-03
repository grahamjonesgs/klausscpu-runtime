/*
 * klausscpu_timer.c — Zephyr sys_clock driver for KlaussCPU.
 *
 * Uses the MMIO timer at 0xF00F_0000 (same as the FreeRTOS port).
 * The timer ISR (z_klausscpu_timer_isr) lives in swap.S and calls
 * sys_clock_isr() from there; we just need to configure the hardware.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/irq.h>
#include <klausscpu_isr.h>

/* MMIO register shortcuts. */
#define REG_TIMER_PER   (*(volatile uint32_t *)0xF00F0030u)
#define REG_TIMER_CNT   (*(volatile uint32_t *)0xF00F0038u)

/* Free-running cycle counter (PERF_CYCLES, perf-counter block at 0xF00D).
 * 48 bits wide (upper 16 read 0), ticks every CPU cycle, unaffected by
 * CPU_RESETN or program load.  This — not REG_TIMER_CNT, which wraps every
 * tick period — is what backs k_cycle_get_32/64().  Caveat: PERF_CTRL[0]
 * (write-1-clear-all, used by perf profiling tools) zeroes it, so cycle
 * timestamps taken across a perf-counter clear are invalid. */
#define REG_PERF_CYCLES (*(volatile uint64_t *)0xF00D0008u)

static int klausscpu_timer_init(void)
{
    /* Set timer period: CPU_HZ / TICKS_PER_SEC cycles. */
    REG_TIMER_PER = CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC
                  / CONFIG_SYS_CLOCK_TICKS_PER_SEC;

    /* Enable timer interrupt (source 0): installs z_klausscpu_timer_isr
     * (swap.S) as INT_VEC0.  The ISR table and INT_MASK were set up by
     * klausscpu_irq_init() (irq.c, PRE_KERNEL_1). */
    arch_irq_enable(KLAUSSCPU_IRQ_TIMER);

    return 0;
}

void sys_clock_set_timeout(uint32_t ticks, bool idle)
{
    ARG_UNUSED(ticks);
    ARG_UNUSED(idle);
    /* Fixed-rate tick — no tickless support needed for hello_world. */
}

uint32_t sys_clock_elapsed(void)
{
    return 0;
}

uint32_t sys_clock_cycle_get_32(void)
{
    return (uint32_t)REG_PERF_CYCLES;
}

uint64_t sys_clock_cycle_get_64(void)
{
    return REG_PERF_CYCLES;
}

/* Called from the timer ISR stub in swap.S (on the ISR stack, interrupts
 * disabled): announce one tick to the kernel, then resume the highest-priority
 * thread (klausscpu_isr.h). */
void sys_clock_isr(void)
{
    sys_clock_announce(1);
    z_klausscpu_isr_exit();
}

/* Priority 2 = CONFIG_SYSTEM_CLOCK_INIT_PRIORITY default. */
SYS_INIT(klausscpu_timer_init, PRE_KERNEL_2, 2);
