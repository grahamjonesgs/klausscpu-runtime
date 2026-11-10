/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * The user-visible register set (ptrace PTRACE_GETREGSET NT_PRSTATUS, core
 * dumps, signal frames): r0-r15, the user SP, the PC and the flags half of
 * the hardware frame (klaussCPU MMU_PLAN.md §3.2: [38:32] flags; the
 * INT_MASK/DI/PM bits are the kernel's and are ignored on write).
 */
#ifndef _UAPI_ASM_KLAUSSCPU_PTRACE_H
#define _UAPI_ASM_KLAUSSCPU_PTRACE_H

/* GENERIC_ENTRY's syscall emulation requests (the values loongarch/arm64 use). */
#define PTRACE_SYSEMU			0x1f
#define PTRACE_SYSEMU_SINGLESTEP	0x20

#ifndef __ASSEMBLY__

#include <linux/types.h>

/*
 * Named user_pt_regs, as on arm64: struct user_regs_struct is the C
 * library's (<sys/user.h>, musl's bits/user.h has the same layout), and a
 * uapi struct of that name would clash with it in programs that include
 * both, such as strace.
 */
struct user_pt_regs {
	unsigned long r[16];	/* r0-r15; r12 = syscall number/result, r15 = P */
	unsigned long sp;	/* user stack pointer */
	unsigned long pc;
	unsigned long flags;	/* frame[38:32]: Z, Z, C, V, S, S^V, C */
};

#endif /* __ASSEMBLY__ */

#endif
