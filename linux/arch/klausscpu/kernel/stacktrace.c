// SPDX-License-Identifier: GPL-2.0
/*
 * Stack walking over the frame-pointer chain: every function that calls
 * another has an ENTER frame (klausscpu-llvm keeps R15 = P), where [P] is
 * the caller's P and [P + 8] the return address.
 */
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/stacktrace.h>
#include <asm/ptrace.h>

void arch_stack_walk(stack_trace_consume_fn consume_entry, void *cookie,
		     struct task_struct *task, struct pt_regs *regs)
{
	unsigned long fp, pc, low, high;

	if (!task)
		task = current;
	low = (unsigned long)task_stack_page(task);
	high = low + THREAD_SIZE;

	if (regs) {
		if (!consume_entry(cookie, instruction_pointer(regs)))
			return;
		fp = regs->r[15];
	} else if (task == current) {
		fp = (unsigned long)__builtin_frame_address(0);
	} else {
		fp = task->thread.r15;
	}

	while (fp >= low && fp + 16 <= high && !(fp & 7)) {
		pc = ((unsigned long *)fp)[1];
		if (!pc || !consume_entry(cookie, pc))
			break;
		if (((unsigned long *)fp)[0] <= fp)
			break;		/* frames move up the stack */
		fp = ((unsigned long *)fp)[0];
	}
}
