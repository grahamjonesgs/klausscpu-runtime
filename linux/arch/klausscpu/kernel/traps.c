// SPDX-License-Identifier: GPL-2.0
/*
 * The C side of klausscpu_exc_vector and the INT_VECn stubs: one dispatch
 * on regs->ecause (MMU_PLAN §4.1, or IRQ_FLAG | n for an interrupt).
 */
#include <linux/entry-common.h>
#include <linux/irq.h>
#include <asm/irq_regs.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/sched/debug.h>
#include <linux/sched/task_stack.h>
#include <linux/signal.h>
#include <linux/syscalls.h>
#include <linux/extable.h>
#include <linux/uaccess.h>
#include <linux/nospec.h>
#include <asm/klausscpu.h>
#include <asm/syscall.h>

#define IRQ_FLAG	0x10000

int show_unhandled_signals = 1;

extern void klausscpu_exc_vector(void);
extern void klausscpu_tlb_refill(void);
extern void klausscpu_handle_irq(unsigned int hwirq);
extern void do_page_fault(struct pt_regs *regs, unsigned long cause, unsigned long addr);

typedef long (*syscall_fn_t)(unsigned long, unsigned long, unsigned long,
			     unsigned long, unsigned long, unsigned long);

static const char *const cause_names[] = {
	[EXC_MISALIGNED_FETCH] = "misaligned fetch",
	[EXC_ACCESS_FETCH] = "fetch access",
	[EXC_ILLEGAL] = "illegal instruction",
	[EXC_PRIVILEGED] = "privileged instruction",
	[EXC_TRAP] = "trap",
	[EXC_SYSCALL] = "syscall",
	[EXC_MISALIGNED_LOAD] = "misaligned load",
	[EXC_MISALIGNED_STORE] = "misaligned store",
	[EXC_ACCESS_LOAD] = "load access",
	[EXC_ACCESS_STORE] = "store access",
	[EXC_TLB_MISS_LOAD] = "TLB miss (load)",
	[EXC_TLB_MISS_STORE] = "TLB miss (store)",
	[EXC_TLB_MISS_FETCH] = "TLB miss (fetch)",
};

void show_regs(struct pt_regs *regs)
{
	int i;

	show_regs_print_info(KERN_DEFAULT);
	pr_info("pc %08lx frame %016lx sp %016lx ecause %lx eaddr %lx\n",
		instruction_pointer(regs), regs->frame, regs->sp, regs->ecause, regs->eaddr);
	for (i = 0; i < 16; i += 4)
		pr_info("r%-2d %016lx %016lx %016lx %016lx\n", i,
			regs->r[i], regs->r[i + 1], regs->r[i + 2], regs->r[i + 3]);
}

/* Walk the frame-pointer chain: [P] = caller's P, [P + 8] = return address. */
void show_stack(struct task_struct *task, unsigned long *sp, const char *loglvl)
{
	unsigned long fp, pc;
	int depth = 0;

	if (!task || task == current)
		asm volatile("copy %0, r15" : "=r"(fp));
	else
		fp = task->thread.r15;
	printk("%sCall trace:\n", loglvl);
	while (fp && depth++ < 32) {
		if (fp < PAGE_OFFSET || fp >= (unsigned long)high_memory || (fp & 7))
			break;
		pc = ((unsigned long *)fp)[1];
		printk("%s [<%08lx>] %pS\n", loglvl, pc, (void *)pc);
		fp = ((unsigned long *)fp)[0];
	}
}

static void do_syscall(struct pt_regs *regs)
{
	long nr = regs->r[12];

	regs->syscallno = nr;
	regs->orig_r0 = regs->r[0];
	regs->r[12] = -ENOSYS;
	if (likely(syscall_enter_from_user_mode_randomize_stack(regs, &nr)) &&
	    nr >= 0 && nr < NR_syscalls) {
		syscall_fn_t fn = sys_call_table[array_index_nospec(nr, NR_syscalls)];

		regs->r[12] = fn(regs->r[0], regs->r[1], regs->r[2],
				 regs->r[3], regs->r[4], regs->r[5]);
	}
	syscall_exit_to_user_mode(regs);
}

static void do_bad_exception(struct pt_regs *regs, unsigned long cause, int sig, int code)
{
	if (user_mode(regs)) {
		irqentry_state_t state = irqentry_enter(regs);

		if (show_unhandled_signals)
			pr_info_ratelimited("%s[%d]: %s at pc %08lx eaddr %08lx\n",
					    current->comm, task_pid_nr(current),
					    cause < ARRAY_SIZE(cause_names) && cause_names[cause] ?
					    cause_names[cause] : "exception",
					    instruction_pointer(regs), regs->eaddr);
		force_sig_fault(sig, code, (void __user *)instruction_pointer(regs));
		irqentry_exit(regs, state);
		return;
	}
	if (fixup_exception(regs))
		return;
	show_regs(regs);
	panic("kernel %s at %08lx", cause < ARRAY_SIZE(cause_names) && cause_names[cause] ?
	      cause_names[cause] : "exception", instruction_pointer(regs));
}

asmlinkage void klausscpu_do_exception(struct pt_regs *regs);
asmlinkage void klausscpu_do_exception(struct pt_regs *regs)
{
	unsigned long cause = regs->ecause;

	if (cause & IRQ_FLAG) {
		irqentry_state_t state = irqentry_enter(regs);
		struct pt_regs *old_regs = set_irq_regs(regs);

		irq_enter_rcu();
		klausscpu_handle_irq(cause & 0xff);
		irq_exit_rcu();
		set_irq_regs(old_regs);
		irqentry_exit(regs, state);
		return;
	}

	switch (cause & 0xff) {
	case EXC_SYSCALL:
		if (user_mode(regs)) {
			do_syscall(regs);
			return;
		}
		do_bad_exception(regs, EXC_SYSCALL, SIGILL, ILL_ILLTRP);
		return;
	case EXC_TLB_MISS_LOAD:
	case EXC_TLB_MISS_STORE:
	case EXC_TLB_MISS_FETCH:
	case EXC_ACCESS_LOAD:
	case EXC_ACCESS_STORE:
	case EXC_ACCESS_FETCH:
		do_page_fault(regs, cause & 0xff, regs->eaddr);
		return;
	case EXC_MISALIGNED_FETCH:
	case EXC_MISALIGNED_LOAD:
	case EXC_MISALIGNED_STORE:
		do_bad_exception(regs, cause & 0xff, SIGBUS, BUS_ADRALN);
		return;
	case EXC_TRAP:
		do_bad_exception(regs, EXC_TRAP, SIGTRAP, TRAP_BRKPT);
		return;
	case EXC_PRIVILEGED:
		do_bad_exception(regs, EXC_PRIVILEGED, SIGILL, ILL_PRVOPC);
		return;
	default:
		do_bad_exception(regs, cause & 0xff, SIGILL, ILL_ILLOPC);
		return;
	}
}

int fixup_exception(struct pt_regs *regs)
{
	const struct exception_table_entry *e = search_exception_tables(instruction_pointer(regs));

	if (!e)
		return 0;
	instruction_pointer_set(regs, e->fixup);
	return 1;
}

void __init trap_init(void)
{
	mtsr(SR_TLBVEC, (unsigned long)klausscpu_tlb_refill);
	mtsr(SR_EVEC, (unsigned long)klausscpu_exc_vector);
}
