/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DI rd: rd = 1 if interrupts were enabled, then disable.  EI rs: enable if
 * rs bit 0 is 1, else disable.  So the flags word is "interrupts enabled".
 */
#ifndef _ASM_KLAUSSCPU_IRQFLAGS_H
#define _ASM_KLAUSSCPU_IRQFLAGS_H

#include <asm/klausscpu.h>

#define ARCH_IRQ_ENABLED	1UL

static inline unsigned long arch_local_save_flags(void)
{
	return (mfsr(SR_STATUS) & STATUS_DI) ? 0 : ARCH_IRQ_ENABLED;
}

static inline unsigned long arch_local_irq_save(void)
{
	return __builtin_klausscpu_di();
}

static inline void arch_local_irq_restore(unsigned long flags)
{
	__builtin_klausscpu_ei(flags);
}

static inline void arch_local_irq_enable(void)
{
	__builtin_klausscpu_ei(1);
}

static inline void arch_local_irq_disable(void)
{
	(void)__builtin_klausscpu_di();
}

static inline int arch_irqs_disabled_flags(unsigned long flags)
{
	return !(flags & ARCH_IRQ_ENABLED);
}

static inline int arch_irqs_disabled(void)
{
	return arch_irqs_disabled_flags(arch_local_save_flags());
}

#endif
