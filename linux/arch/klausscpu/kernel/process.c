// SPDX-License-Identifier: GPL-2.0
#include <linux/entry-common.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/debug.h>
#include <linux/elf.h>
#include <linux/elfcore.h>
#include <linux/reboot.h>
#include <asm/setup.h>
#include <asm/klausscpu.h>
#include <asm/processor.h>
#include <asm/switch_to.h>

extern void ret_from_fork(void);

/* Every frame enables all four sources (enable lives in INT_VECn). */
#define USER_FRAME_BITS	((0xfUL << FRAME_INT_MASK_SHIFT) | FRAME_PM)

/*
 * WAIT with interrupts enabled.  The RTL does not take a pending interrupt
 * in a one-instruction EI -> DI window (the emulator does), so idle must
 * never be "ei; di; wait".
 */
void arch_cpu_idle(void)
{
	raw_local_irq_enable();
	asm volatile("wait");
}

asmlinkage void klausscpu_ret_from_fork(struct task_struct *prev, struct pt_regs *regs,
					int (*fn)(void *), void *arg)
{
	schedule_tail(prev);
	if (fn)
		fn(arg);	/* a kernel thread; returns only after kernel_execve() */
	syscall_exit_to_user_mode(regs);
}

int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	struct pt_regs *childregs = task_pt_regs(p);

	memset(&p->thread, 0, sizeof(p->thread));
	if (unlikely(args->fn)) {
		memset(childregs, 0, sizeof(*childregs));
		childregs->syscallno = -1;
		p->thread.r4 = (unsigned long)args->fn;
		p->thread.r5 = (unsigned long)args->fn_arg;
	} else {
		*childregs = *current_pt_regs();
		childregs->r[12] = 0;		/* the child's fork() returns 0 */
		if (args->stack)
			childregs->sp = args->stack;
		p->thread.tp = (args->flags & CLONE_SETTLS) ? args->tls : mfsr(SR_TP);
	}
	p->thread.ra = (unsigned long)ret_from_fork;
	p->thread.sp = (unsigned long)childregs;
	return 0;
}

void start_thread(struct pt_regs *regs, unsigned long pc, unsigned long sp)
{
	memset(regs->r, 0, sizeof(regs->r));
	regs->frame = (pc & FRAME_PC_MASK) | USER_FRAME_BITS;
	regs->sp = sp;
	regs->syscallno = -1;
	mtsr(SR_TP, 0);
}

/* No reset or power control from software: stop the core. */
void machine_halt(void)
{
	local_irq_disable();
	for (;;)
		asm volatile("halt");
}

void machine_restart(char *cmd)
{
	machine_halt();
}

void machine_power_off(void)
{
	machine_halt();
}

void flush_thread(void)
{
}

unsigned long __get_wchan(struct task_struct *p)
{
	return 0;
}

void klausscpu_elf_core_copy_regs(elf_gregset_t dest, struct pt_regs *regs)
{
	struct user_pt_regs *u = (struct user_pt_regs *)dest;

	memcpy(u->r, regs->r, sizeof(u->r));
	u->sp = regs->sp;
	u->pc = instruction_pointer(regs);
	u->flags = (regs->frame & FRAME_FLAGS_MASK) >> FRAME_FLAGS_SHIFT;
}
