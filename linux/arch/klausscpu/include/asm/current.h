/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_CURRENT_H
#define _ASM_KLAUSSCPU_CURRENT_H

#ifndef __ASSEMBLER__
#include <linux/compiler.h>
#include <asm/klausscpu.h>

struct task_struct;

/* current lives in SCRATCH0 (klausscpu-llvm has no -ffixed-<reg>). */
static __always_inline struct task_struct *get_current(void)
{
	struct task_struct *t;

	asm("mfsr %0, scratch0" : "=r"(t));
	return t;
}
#define current get_current()

static __always_inline unsigned long __current_stack_pointer(void)
{
	unsigned long sp;

	asm volatile("getsp %0" : "=r"(sp));
	return sp;
}
#define current_stack_pointer __current_stack_pointer()

#endif
#endif
