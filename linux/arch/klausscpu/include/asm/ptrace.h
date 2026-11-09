/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_PTRACE_H
#define _ASM_KLAUSSCPU_PTRACE_H

#include <uapi/asm/ptrace.h>
#include <asm/klausscpu.h>

#ifndef __ASSEMBLER__

/*
 * The trap frame on the kernel stack.  The hardware pushes `frame` (one
 * 64-bit word: PC, flags, INT_MASK, DI, PM, MMU_PLAN §3.2); the entry code
 * pushes the rest below it, so the struct ends with `frame` and IRET pops
 * it after the registers are restored.
 */
struct pt_regs {
	unsigned long r[16];	/* r0-r15 */
	unsigned long sp;	/* the interrupted SP: USP from user, else the kernel SP */
	unsigned long ecause;
	unsigned long eaddr;
	unsigned long orig_r0;	/* syscall restart */
	long syscallno;		/* -1 if not a syscall */
	unsigned long frame;	/* the hardware frame; must be last */
};

#define user_mode(regs)			(((regs)->frame & FRAME_PM) != 0)
#define instruction_pointer(regs)	((regs)->frame & FRAME_PC_MASK)
#define user_stack_pointer(regs)	((regs)->sp)
#define profile_pc(regs)		instruction_pointer(regs)

static inline void instruction_pointer_set(struct pt_regs *regs, unsigned long val)
{
	regs->frame = (regs->frame & ~FRAME_PC_MASK) | (val & FRAME_PC_MASK);
}

static inline unsigned long kernel_stack_pointer(struct pt_regs *regs)
{
	return regs->sp;
}

static inline unsigned long frame_pointer(struct pt_regs *regs)
{
	return regs->r[15];
}

static inline unsigned long regs_return_value(struct pt_regs *regs)
{
	return regs->r[12];
}

static inline void regs_set_return_value(struct pt_regs *regs, unsigned long val)
{
	regs->r[12] = val;
}

static inline int regs_irqs_disabled(struct pt_regs *regs)
{
	return (regs->frame & FRAME_DI) != 0;
}

static inline bool in_syscall(struct pt_regs const *regs)
{
	return regs->syscallno != -1;
}

static inline void forget_syscall(struct pt_regs *regs)
{
	regs->syscallno = -1;
}

extern unsigned long regs_get_register(struct pt_regs *regs, unsigned int offset);
extern int regs_query_register_offset(const char *name);
extern unsigned long regs_get_kernel_stack_nth(struct pt_regs *regs, unsigned int n);

#define arch_has_single_step()	(0)

#endif /* !__ASSEMBLER__ */
#endif
