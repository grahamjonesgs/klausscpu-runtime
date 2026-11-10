// SPDX-License-Identifier: GPL-2.0
/* Offsets for the assembly entry, refill and switch code. */
#define COMPILE_OFFSETS

#include <linux/kbuild.h>
#include <linux/sched.h>
#include <asm/ptrace.h>
#include <asm/processor.h>

void asm_offsets(void);
void asm_offsets(void)
{
	DEFINE(PT_R0, offsetof(struct pt_regs, r[0]));
	DEFINE(PT_R12, offsetof(struct pt_regs, r[12]));
	DEFINE(PT_SP, offsetof(struct pt_regs, sp));
	DEFINE(PT_ECAUSE, offsetof(struct pt_regs, ecause));
	DEFINE(PT_EADDR, offsetof(struct pt_regs, eaddr));
	DEFINE(PT_ORIG_R0, offsetof(struct pt_regs, orig_r0));
	DEFINE(PT_SYSCALLNO, offsetof(struct pt_regs, syscallno));
	DEFINE(PT_FRAME, offsetof(struct pt_regs, frame));
	DEFINE(PT_SIZE, sizeof(struct pt_regs));
	BLANK();
	DEFINE(TASK_THREAD, offsetof(struct task_struct, thread));
	DEFINE(TASK_STACK, offsetof(struct task_struct, stack));
	DEFINE(THREAD_R4, offsetof(struct thread_struct, r4));
	DEFINE(THREAD_R5, offsetof(struct thread_struct, r5));
	DEFINE(THREAD_R6, offsetof(struct thread_struct, r6));
	DEFINE(THREAD_R7, offsetof(struct thread_struct, r7));
	DEFINE(THREAD_R15, offsetof(struct thread_struct, r15));
	DEFINE(THREAD_SP, offsetof(struct thread_struct, sp));
	DEFINE(THREAD_RA, offsetof(struct thread_struct, ra));
	DEFINE(THREAD_TP, offsetof(struct thread_struct, tp));
	BLANK();
	DEFINE(PGD_SHIFT, PGDIR_SHIFT);
	DEFINE(PTE_PRESENT, _PAGE_PRESENT);
}
