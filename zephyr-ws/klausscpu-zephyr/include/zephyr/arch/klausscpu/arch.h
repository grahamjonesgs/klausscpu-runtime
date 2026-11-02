#ifndef ZEPHYR_INCLUDE_ARCH_KLAUSSCPU_ARCH_H_
#define ZEPHYR_INCLUDE_ARCH_KLAUSSCPU_ARCH_H_

/*
 * KlaussCPU architecture interface for Zephyr.
 *
 * Physical address space: 32-bit (128 MiB RAM + MMIO above 0xF000_0000).
 * GPRs: R0-R15, 64-bit.  Stack slots: 8 bytes.  Stack: grows down.
 */

#include <zephyr/arch/common/sys_bitops.h>
#include <zephyr/arch/common/ffs.h>
#include <zephyr/arch/common/addr_types.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/timer/system_timer.h>
#include <zephyr/arch/klausscpu/irq.h>
#include <zephyr/irq.h>

/* Minimum stack pointer alignment: 8 bytes (one GPR slot). */
#define ARCH_STACK_PTR_ALIGN  8

/*
 * Thread stack reservation: 0.
 *
 * arch_new_thread() receives stack_ptr = top (the full stack top) and builds
 * the initial frame (16 GPRs + IRET + z_thread_entry's return address =
 * 144 bytes) downward from there.
 * A non-zero reservation would cause Zephyr to pass stack_ptr = top-N, so
 * arch_new_thread would write the frame to top-N-144 — overflowing below the
 * stack for small stacks (e.g. the 256-byte idle stack would overflow by 16 B).
 */
#define ARCH_THREAD_STACK_RESERVED  0

/*
 * arch_esf — the context an interrupt stub saves on the interrupted stack.
 * Contains all 16 GPRs + the hardware IRET word.
 */
struct arch_esf {
    uint64_t r[16];    /* R0..R15 */
    uint64_t iret;     /* hardware IRET frame (INT_MASK | flags | PC) */
};

/* No user-mode or syscall support. */
#define ARCH_SYSCALL_LIMIT  0

/*
 * arch_nop — NOP instruction.
 */
static ALWAYS_INLINE void arch_nop(void)
{
    __asm__ volatile("nop");
}

/*
 * arch_irq_lock / arch_irq_unlock — disable/restore all interrupts with the
 * core's interrupt-enable bit (DI / EI, ISA_ENCODING_V2_MAP.md §8.2), not
 * INT_MASK (which keeps selecting the sources, arch_irq_enable()).  The key
 * is the old enable bit: DI returns it and disables, EI puts it back, so
 * locks nest and leave interrupts as they found them.  About 3 cycles per
 * lock/unlock pair, against about 16.5 for the INT_MASK read/zero/restore it
 * replaces.  An IRQ is never taken between a retired DI and its EI, and WAIT
 * still wakes while disabled.  Boot runs locked (__start executes DI).  Both builtins are compiler barriers for memory.
 */
static ALWAYS_INLINE unsigned int arch_irq_lock(void)
{
    return __builtin_klausscpu_di();
}

static ALWAYS_INLINE void arch_irq_unlock(unsigned int key)
{
    __builtin_klausscpu_ei(key);
}

static ALWAYS_INLINE bool arch_irq_unlocked(unsigned int key)
{
    return key != 0u;
}

/* k_cycle_get_32/64(): the free-running PERF_CYCLES counter behind the timer
 * driver (drivers/timer/klausscpu_timer.c), not the tick counter TIMER_COUNT
 * (0xF00F_0038), which wraps every tick period (1 ms at 1 kHz): k_busy_wait()
 * and every interval over a tick would be wrong. */
static ALWAYS_INLINE uint32_t arch_k_cycle_get_32(void)
{
    return sys_clock_cycle_get_32();
}

static ALWAYS_INLINE uint64_t arch_k_cycle_get_64(void)
{
    return sys_clock_cycle_get_64();
}

/* arch_switch is in swap.S; declared via arch_interface.h */

static ALWAYS_INLINE unsigned int arch_num_cpus(void)
{
	return 1;
}

#include <kernel_arch_func.h>

#endif /* ZEPHYR_INCLUDE_ARCH_KLAUSSCPU_ARCH_H_ */
