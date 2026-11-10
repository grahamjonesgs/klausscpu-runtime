// SPDX-License-Identifier: GPL-2.0
/*
 * ptrace: the general registers as one regset, NT_PRSTATUS, in the layout
 * of struct user_pt_regs (uapi/asm/ptrace.h: r0-r15, the user SP, the
 * PC and the flags half of the hardware frame) — the same layout as core
 * dumps (klausscpu_elf_core_copy_regs) and signal frames.  PTRACE_GETREGSET
 * and PTRACE_SETREGSET use it, and PTRACE_PEEKUSR/POKEUSR address it by
 * byte offset.
 *
 * At a syscall-entry stop r12 already reads -ENOSYS (do_syscall keeps the
 * number in regs->syscallno and the first argument in orig_r0, as arm64
 * does); a tracer reads and changes the number and arguments with
 * PTRACE_GET/SET_SYSCALL_INFO, which use asm/syscall.h.
 */
#include <linux/elf.h>
#include <linux/ptrace.h>
#include <linux/regset.h>
#include <linux/sched.h>
#include <linux/sched/task_stack.h>
#include <linux/uaccess.h>

/* No hardware single step. */
void user_enable_single_step(struct task_struct *child)
{
}

void user_disable_single_step(struct task_struct *child)
{
}

void ptrace_disable(struct task_struct *child)
{
}

static void regs_to_user(const struct pt_regs *regs, struct user_pt_regs *u)
{
	memcpy(u->r, regs->r, sizeof(u->r));
	u->sp = regs->sp;
	u->pc = instruction_pointer(regs);
	u->flags = (regs->frame & FRAME_FLAGS_MASK) >> FRAME_FLAGS_SHIFT;
}

/* As sigreturn does: the kernel's INT_MASK, DI and PM bits of the frame stay. */
static void user_to_regs(const struct user_pt_regs *u, struct pt_regs *regs)
{
	memcpy(regs->r, u->r, sizeof(regs->r));
	regs->sp = u->sp;
	regs->frame = (regs->frame & ~(FRAME_PC_MASK | FRAME_FLAGS_MASK)) |
		      (u->pc & FRAME_PC_MASK) |
		      ((u->flags << FRAME_FLAGS_SHIFT) & FRAME_FLAGS_MASK);
}

static int gpr_get(struct task_struct *target, const struct user_regset *regset,
		   struct membuf to)
{
	struct user_pt_regs u;

	regs_to_user(task_pt_regs(target), &u);
	return membuf_write(&to, &u, sizeof(u));
}

static int gpr_set(struct task_struct *target, const struct user_regset *regset,
		   unsigned int pos, unsigned int count,
		   const void *kbuf, const void __user *ubuf)
{
	struct pt_regs *regs = task_pt_regs(target);
	struct user_pt_regs u;
	int ret;

	regs_to_user(regs, &u);
	ret = user_regset_copyin(&pos, &count, &kbuf, &ubuf, &u, 0, sizeof(u));
	if (ret)
		return ret;
	user_to_regs(&u, regs);
	return 0;
}

enum klausscpu_regset { REGSET_GPR };

static const struct user_regset klausscpu_regsets[] = {
	[REGSET_GPR] = {
		USER_REGSET_NOTE_TYPE(PRSTATUS),
		.n		= ELF_NGREG,
		.size		= sizeof(elf_greg_t),
		.align		= sizeof(elf_greg_t),
		.regset_get	= gpr_get,
		.set		= gpr_set,
	},
};

static const struct user_regset_view klausscpu_regset_view = {
	.name		= "klausscpu",
	.e_machine	= EM_KLAUSSCPU,
	.regsets	= klausscpu_regsets,
	.n		= ARRAY_SIZE(klausscpu_regsets),
};

const struct user_regset_view *task_user_regset_view(struct task_struct *task)
{
	return &klausscpu_regset_view;
}

/* PTRACE_PEEKUSR/POKEUSR: an aligned word of struct user_pt_regs. */
static int peek_user(struct task_struct *child, unsigned long addr, unsigned long __user *data)
{
	struct user_pt_regs u;

	if (addr & (sizeof(long) - 1) || addr >= sizeof(u))
		return -EIO;
	regs_to_user(task_pt_regs(child), &u);
	return put_user(((unsigned long *)&u)[addr / sizeof(long)], data);
}

static int poke_user(struct task_struct *child, unsigned long addr, unsigned long data)
{
	struct pt_regs *regs = task_pt_regs(child);
	struct user_pt_regs u;

	if (addr & (sizeof(long) - 1) || addr >= sizeof(u))
		return -EIO;
	regs_to_user(regs, &u);
	((unsigned long *)&u)[addr / sizeof(long)] = data;
	user_to_regs(&u, regs);
	return 0;
}

long arch_ptrace(struct task_struct *child, long request,
		 unsigned long addr, unsigned long data)
{
	switch (request) {
	case PTRACE_PEEKUSR:
		return peek_user(child, addr, (unsigned long __user *)data);
	case PTRACE_POKEUSR:
		return poke_user(child, addr, data);
	default:
		return ptrace_request(child, request, addr, data);
	}
}
