#ifndef ZEPHYR_INCLUDE_ARCH_KLAUSSCPU_IRQ_H_
#define ZEPHYR_INCLUDE_ARCH_KLAUSSCPU_IRQ_H_

/*
 * KlaussCPU interrupt interface for Zephyr.
 *
 * The SoC has one flat interrupt controller (0xF00F_0000, CPU_ARCHITECTURE.md
 * 13.1) with four sources.  Zephyr IRQ numbers are the source numbers:
 *
 *   0  periodic timer     the kernel's tick; owned by the timer driver
 *   1  blitter DONE       level; the ISR writes BLIT_STATUS = 2 (W1C DONE)
 *   2  Ethernet (LiteEth) level; the ISR acknowledges in the MAC (EV_PENDING)
 *   3  VGA vsync          level; the ISR writes VGA_STATUS = 2 (W1C VSYNC)
 *
 * There are no priorities.  When several sources are pending the hardware
 * picks timer > blitter > Ethernet > VGA, and a handler runs to completion
 * with interrupts disabled (the entry stub executes DI), so nothing preempts
 * it.  The `priority` argument of IRQ_CONNECT() / irq_connect_dynamic() and
 * the `flags` are accepted and ignored.  One handler per source (no shared
 * interrupts).  A level source must be acknowledged at the device before the
 * handler returns, or it is dispatched again at once.
 */

#include <zephyr/sys/util_macro.h>
#include <zephyr/toolchain.h>

#define KLAUSSCPU_NUM_IRQS     4
#define KLAUSSCPU_IRQ_TIMER    0
#define KLAUSSCPU_IRQ_BLITTER  1
#define KLAUSSCPU_IRQ_ETH      2
#define KLAUSSCPU_IRQ_VGA      3

#ifndef _ASMLANGUAGE

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void arch_irq_enable(unsigned int irq);
extern void arch_irq_disable(unsigned int irq);
extern int arch_irq_is_enabled(unsigned int irq);

/* One static connection: IRQ_CONNECT() emits one into the .klausscpu_irq_conn
 * section, which klausscpu_irq_init() copies into the (RAM) table at boot. */
struct klausscpu_irq_conn {
	uint32_t irq;
	void (*isr)(const void *arg);
	const void *arg;
};

#define Z_KLAUSSCPU_IRQ_DECLARE(irq_p, isr_p, arg_p)                                              \
	static const struct klausscpu_irq_conn Z_GENERIC_SECTION(.klausscpu_irq_conn) __used      \
		_klausscpu_irq_conn = {                                                           \
			.irq = (irq_p),                                                           \
			.isr = (void (*)(const void *))(void *)(isr_p),                           \
			.arg = (const void *)(arg_p),                                             \
		}

/* Priority and flags are ignored (see above).  IRQ 0 belongs to the kernel's
 * tick, and only sources 1..3 exist. */
#define ARCH_IRQ_CONNECT(irq_p, priority_p, isr_p, isr_param_p, flags_p)                          \
	({                                                                                        \
		BUILD_ASSERT((irq_p) >= 1 && (irq_p) < KLAUSSCPU_NUM_IRQS,                        \
			     "KlaussCPU: IRQ_CONNECT needs a device source, 1..3 (0 is the tick)"); \
		Z_KLAUSSCPU_IRQ_DECLARE(irq_p, isr_p, isr_param_p);                               \
		(irq_p);                                                                          \
	})

#ifdef __cplusplus
}
#endif

#endif /* !_ASMLANGUAGE */
#endif /* ZEPHYR_INCLUDE_ARCH_KLAUSSCPU_IRQ_H_ */
