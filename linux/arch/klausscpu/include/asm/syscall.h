/* SPDX-License-Identifier: GPL-2.0 */
/*
 * System call ABI: SYSCALL with the number in r12 and arguments in r0-r5;
 * the result comes back in r12.  The frame PC is already past SYSCALL
 * (MMU_PLAN §4.1), so a restart rewinds it by 4.
 */
#ifndef _ASM_KLAUSSCPU_SYSCALL_H
#define _ASM_KLAUSSCPU_SYSCALL_H

#include <uapi/linux/audit.h>
#include <linux/err.h>
#include <linux/sched.h>
#include <asm/ptrace.h>

/* EM_KLAUSSCPU (0x4B43) with the 64-bit ABI flag. */
#define AUDIT_ARCH_KLAUSSCPU	(0x4B43 | __AUDIT_ARCH_64BIT | __AUDIT_ARCH_LE)

extern void *sys_call_table[];

static inline int syscall_get_nr(struct task_struct *task, struct pt_regs *regs)
{
	return regs->syscallno;
}

static inline void syscall_set_nr(struct task_struct *task, struct pt_regs *regs, int nr)
{
	regs->syscallno = nr;
}

static inline void syscall_rollback(struct task_struct *task, struct pt_regs *regs)
{
	regs->r[0] = regs->orig_r0;
}

static inline long syscall_get_error(struct task_struct *task, struct pt_regs *regs)
{
	unsigned long error = regs->r[12];

	return IS_ERR_VALUE(error) ? error : 0;
}

static inline long syscall_get_return_value(struct task_struct *task, struct pt_regs *regs)
{
	return regs->r[12];
}

static inline void syscall_set_return_value(struct task_struct *task, struct pt_regs *regs,
					    int error, long val)
{
	regs->r[12] = (long)error ?: val;
}

static inline void syscall_get_arguments(struct task_struct *task, struct pt_regs *regs,
					 unsigned long *args)
{
	args[0] = regs->orig_r0;
	args[1] = regs->r[1];
	args[2] = regs->r[2];
	args[3] = regs->r[3];
	args[4] = regs->r[4];
	args[5] = regs->r[5];
}

static inline void syscall_set_arguments(struct task_struct *task, struct pt_regs *regs,
					 const unsigned long *args)
{
	regs->orig_r0 = args[0];
	regs->r[0] = args[0];
	regs->r[1] = args[1];
	regs->r[2] = args[2];
	regs->r[3] = args[3];
	regs->r[4] = args[4];
	regs->r[5] = args[5];
}

static inline int syscall_get_arch(struct task_struct *task)
{
	return AUDIT_ARCH_KLAUSSCPU;
}

static inline bool arch_syscall_is_vdso_sigreturn(struct pt_regs *regs)
{
	return false;
}

#endif
