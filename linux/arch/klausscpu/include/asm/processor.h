/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_PROCESSOR_H
#define _ASM_KLAUSSCPU_PROCESSOR_H

#include <asm/ptrace.h>
#include <asm/pgtable.h>

#define STACK_TOP		TASK_SIZE
#define STACK_TOP_MAX		TASK_SIZE
#define TASK_UNMAPPED_BASE	PAGE_ALIGN(TASK_SIZE / 3)

#ifndef __ASSEMBLER__

struct task_struct;

/*
 * Saved by __switch_to: the callee-saved registers of the klausscpu-llvm
 * ABI (R4-R7 and R15), SP and the return address, plus TP (user TLS).
 */
struct thread_struct {
	unsigned long r4, r5, r6, r7, r15;
	unsigned long sp;
	unsigned long ra;
	unsigned long tp;
	unsigned long align_ctl;	/* prctl(PR_SET_UNALIGN): PR_UNALIGN_* */
};

#define INIT_THREAD	{ }

#define task_pt_regs(tsk) \
	((struct pt_regs *)(task_stack_page(tsk) + THREAD_SIZE) - 1)

#define KSTK_EIP(tsk)	instruction_pointer(task_pt_regs(tsk))
#define KSTK_ESP(tsk)	(task_pt_regs(tsk)->sp)

extern void start_thread(struct pt_regs *regs, unsigned long pc, unsigned long sp);
extern unsigned long __get_wchan(struct task_struct *p);

#define cpu_relax()	barrier()

/* prctl(PR_SET_UNALIGN / PR_GET_UNALIGN): kernel/unaligned.c. */
extern int klausscpu_get_unalign_ctl(struct task_struct *tsk, unsigned long addr);
extern int klausscpu_set_unalign_ctl(struct task_struct *tsk, unsigned int val);
#define GET_UNALIGN_CTL(tsk, addr)	klausscpu_get_unalign_ctl((tsk), (addr))
#define SET_UNALIGN_CTL(tsk, val)	klausscpu_set_unalign_ctl((tsk), (val))

#endif /* !__ASSEMBLER__ */
#endif
