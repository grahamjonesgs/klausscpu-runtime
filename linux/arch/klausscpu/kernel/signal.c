// SPDX-License-Identifier: GPL-2.0
#include <linux/entry-common.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/signal.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <asm/ucontext.h>
#include <asm/unistd.h>
#include <asm/klausscpu.h>

struct rt_sigframe {
	struct siginfo info;
	struct ucontext uc;
};

static int restore_sigcontext(struct pt_regs *regs, struct sigcontext __user *sc)
{
	struct user_pt_regs u;

	if (__copy_from_user(&u, &sc->regs, sizeof(u)))
		return -EFAULT;
	memcpy(regs->r, u.r, sizeof(u.r));
	regs->sp = u.sp;
	regs->frame = (regs->frame & ~(FRAME_PC_MASK | FRAME_FLAGS_MASK)) |
		      (u.pc & FRAME_PC_MASK) |
		      ((u.flags << FRAME_FLAGS_SHIFT) & FRAME_FLAGS_MASK);
	regs->syscallno = -1;		/* no syscall restart after sigreturn */
	return 0;
}

SYSCALL_DEFINE0(rt_sigreturn)
{
	struct pt_regs *regs = current_pt_regs();
	struct rt_sigframe __user *frame = (struct rt_sigframe __user *)regs->sp;
	sigset_t set;

	current->restart_block.fn = do_no_restart_syscall;
	if (!access_ok(frame, sizeof(*frame)))
		goto badframe;
	if (__copy_from_user(&set, &frame->uc.uc_sigmask, sizeof(set)))
		goto badframe;
	set_current_blocked(&set);
	if (restore_sigcontext(regs, &frame->uc.uc_mcontext))
		goto badframe;
	if (restore_altstack(&frame->uc.uc_stack))
		goto badframe;
	return regs->r[12];

badframe:
	force_sig(SIGSEGV);
	return 0;
}

static int setup_rt_frame(struct ksignal *ksig, sigset_t *set, struct pt_regs *regs)
{
	struct rt_sigframe __user *frame;
	struct user_pt_regs u;
	unsigned long sp, *ret;
	int err = 0;

	if (!(ksig->ka.sa.sa_flags & SA_RESTORER))
		return -EFAULT;		/* no trampoline without a vDSO */

	sp = sigsp(regs->sp, ksig);
	sp = (sp - sizeof(*frame)) & ~15UL;
	frame = (struct rt_sigframe __user *)sp;
	if (!access_ok(frame, sizeof(*frame)))
		return -EFAULT;

	memcpy(u.r, regs->r, sizeof(u.r));
	u.sp = regs->sp;
	u.pc = instruction_pointer(regs);
	u.flags = (regs->frame & FRAME_FLAGS_MASK) >> FRAME_FLAGS_SHIFT;

	err |= copy_siginfo_to_user(&frame->info, &ksig->info);
	err |= __put_user(0, &frame->uc.uc_flags);
	err |= __put_user(NULL, &frame->uc.uc_link);
	err |= __save_altstack(&frame->uc.uc_stack, regs->sp);
	err |= __copy_to_user(&frame->uc.uc_mcontext.regs, &u, sizeof(u));
	err |= __copy_to_user(&frame->uc.uc_sigmask, set, sizeof(*set));
	/* The handler RETs to sa_restorer: CALL's return slot is below the frame. */
	ret = (unsigned long __user *)(sp - 8);
	err |= __put_user((unsigned long)ksig->ka.sa.sa_restorer, ret);
	if (err)
		return -EFAULT;

	regs->r[0] = ksig->sig;
	regs->r[1] = (unsigned long)&frame->info;
	regs->r[2] = (unsigned long)&frame->uc;
	regs->sp = (unsigned long)ret;
	instruction_pointer_set(regs, (unsigned long)ksig->ka.sa.sa_handler);
	return 0;
}

static void syscall_restart(struct pt_regs *regs, struct ksignal *ksig)
{
	switch (regs->r[12]) {
	case -ERESTART_RESTARTBLOCK:
	case -ERESTARTNOHAND:
		if (ksig) {
			regs->r[12] = -EINTR;
			return;
		}
		break;
	case -ERESTARTSYS:
		if (ksig && !(ksig->ka.sa.sa_flags & SA_RESTART)) {
			regs->r[12] = -EINTR;
			return;
		}
		fallthrough;
	case -ERESTARTNOINTR:
		break;
	default:
		return;
	}
	/* Run SYSCALL again: the number is in r12, the arguments are intact. */
	regs->r[12] = (regs->r[12] == (unsigned long)-ERESTART_RESTARTBLOCK && !ksig) ?
		      __NR_restart_syscall : regs->syscallno;
	instruction_pointer_set(regs, instruction_pointer(regs) - 4);
}

void arch_do_signal_or_restart(struct pt_regs *regs)
{
	struct ksignal ksig;

	if (get_signal(&ksig)) {
		if (in_syscall(regs))
			syscall_restart(regs, &ksig);
		signal_setup_done(setup_rt_frame(&ksig, sigmask_to_save(), regs), &ksig, 0);
		return;
	}
	if (in_syscall(regs))
		syscall_restart(regs, NULL);
	restore_saved_sigmask();
}
