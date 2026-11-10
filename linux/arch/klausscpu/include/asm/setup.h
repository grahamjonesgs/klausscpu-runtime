/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_SETUP_H
#define _ASM_KLAUSSCPU_SETUP_H

#include <uapi/asm-generic/setup.h>

#ifndef __ASSEMBLER__
struct pt_regs;
struct task_struct;

/* Arch-internal entry points shared between files. */
void klausscpu_early_console_init(void);
void klausscpu_handle_irq(unsigned int hwirq);
unsigned int klausscpu_irq(unsigned int hwirq);
void do_page_fault(struct pt_regs *regs, unsigned long cause, unsigned long addr);
asmlinkage void klausscpu_ret_from_fork(struct task_struct *prev, struct pt_regs *regs,
					int (*fn)(void *), void *arg);
#endif

#endif
