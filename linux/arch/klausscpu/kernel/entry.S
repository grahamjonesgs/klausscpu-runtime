/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Exception, interrupt and syscall entry; return to user/kernel; fork
 * return; context switch.  See LINUX_PLAN.md "Exceptions, interrupts and
 * system calls".
 *
 * On entry the hardware has switched to the kernel SP (from user mode) and
 * pushed the 64-bit frame (MMU_PLAN §3.3).  The stub builds struct pt_regs
 * below the frame:  r0..r15, sp, ecause, eaddr, orig_r0, syscallno, frame.
 */
#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/klausscpu.h>

#define IRQ_FLAG	0x10000		/* ecause marker for an interrupt */

/*
 * Save everything below the frame; afterwards r12 = &pt_regs and r1 = the
 * frame's PM bit (1: from user mode).
 */
.macro SAVE_REGS
	push	r12			/* into the syscallno slot */
	getsp	r12
	addi	r12, r12, 16 - PT_SIZE
	setsp	r12
	stidx64	r0, r12, 0
	stidx64	r1, r12, 8
	stidx64	r2, r12, 16
	stidx64	r3, r12, 24
	stidx64	r4, r12, 32
	stidx64	r5, r12, 40
	stidx64	r6, r12, 48
	stidx64	r7, r12, 56
	stidx64	r8, r12, 64
	stidx64	r9, r12, 72
	stidx64	r10, r12, 80
	stidx64	r11, r12, 88
	stidx64	r13, r12, 104
	stidx64	r14, r12, 112
	stidx64	r15, r12, 120
	ldidx64	r2, r12, PT_SYSCALLNO
	stidx64	r2, r12, PT_R12
	/* The interrupted SP: USP if from user, else just above the frame. */
	ldidx64	r1, r12, PT_FRAME
	shrv	r1, r1, 44		/* frame[44] PM (bextr reaches bits < 32 only) */
	andi	r1, r1, 1
	fbri	r1, 0, 0, 5f		/* EQ: from the kernel */
	mfsr	r2, usp
	jmp	6f
5:	addi	r2, r12, PT_SIZE
6:	stidx64	r2, r12, PT_SP
	stidx64	r0, r12, PT_ORIG_R0
	setr	r2, -1
	stidx64	r2, r12, PT_SYSCALLNO
	setr	r15, 0
.endm

/* Restore from pt_regs at SP and IRET. */
.macro RESTORE_REGS_IRET
	/*
	 * Interrupts off for the restore: its last load (r12) reads the frame
	 * below the new SP, where an interrupt's frame would land.  The C
	 * handlers return with them off already; this keeps one that doesn't
	 * from corrupting r12.  IRET restores the disable bit from the frame.
	 */
	di	r1
	getsp	r12
	ldidx64	r1, r12, PT_FRAME
	shrv	r1, r1, 44
	andi	r1, r1, 1
	fbri	r1, 0, 0, 7f
	ldidx64	r2, r12, PT_SP
	mtsr	usp, r2
7:	ldidx64	r0, r12, 0
	ldidx64	r1, r12, 8
	ldidx64	r2, r12, 16
	ldidx64	r3, r12, 24
	ldidx64	r4, r12, 32
	ldidx64	r5, r12, 40
	ldidx64	r6, r12, 48
	ldidx64	r7, r12, 56
	ldidx64	r8, r12, 64
	ldidx64	r9, r12, 72
	ldidx64	r10, r12, 80
	ldidx64	r11, r12, 88
	ldidx64	r13, r12, 104
	ldidx64	r14, r12, 112
	ldidx64	r15, r12, 120
	addi	r12, r12, PT_FRAME
	setsp	r12
	ldidx64	r12, r12, PT_R12 - PT_FRAME
	iret
.endm

	.text
	.p2align 3
ENTRY(klausscpu_exc_vector)		/* EVEC */
	SAVE_REGS
	mfsr	r0, ecause
	stidx64	r0, r12, PT_ECAUSE
	mfsr	r2, eaddr
	stidx64	r2, r12, PT_EADDR
	/* A system call from user mode goes straight to its handler. */
	fbri	r0, EXC_SYSCALL, 8, 1f	/* NE */
	fbri	r1, 0, 0, 1f		/* EQ: from the kernel */
	copy	r0, r12
	call	klausscpu_do_syscall
	RESTORE_REGS_IRET
1:	copy	r0, r12
	call	klausscpu_do_exception
	RESTORE_REGS_IRET
ENDPROC(klausscpu_exc_vector)

/* INT_VECn: interrupts arrive with DI as it was; turn it off first. */
.macro IRQ_STUB n
	.p2align 3
ENTRY(klausscpu_irq_vector\n)
	push	r0
	di	r0
	pop	r0
	SAVE_REGS
	setru	r0, IRQ_FLAG | \n
	stidx64	r0, r12, PT_ECAUSE
	setr	r0, 0
	stidx64	r0, r12, PT_EADDR
	copy	r0, r12
	call	klausscpu_do_exception
	RESTORE_REGS_IRET
ENDPROC(klausscpu_irq_vector\n)
.endm
	IRQ_STUB 0
	IRQ_STUB 1
	IRQ_STUB 2
	IRQ_STUB 3

/*
 * First return of a new task.  __switch_to returned prev in r12; r4/r5 are
 * the kernel thread's fn/arg (0 for a user fork); SP = its pt_regs.
 */
ENTRY(ret_from_fork)
	copy	r0, r12
	getsp	r1
	copy	r2, r4
	copy	r3, r5
	call	klausscpu_ret_from_fork
	RESTORE_REGS_IRET
ENDPROC(ret_from_fork)

/*
 * struct task_struct *__switch_to(prev = r0, next = r1): save the
 * callee-saved registers, SP, return address and TP in prev->thread, load
 * next's, set current (SCRATCH0) and return prev in r12 on next's stack.
 */
ENTRY(__switch_to)
	pop	r3			/* our return address */
	addi	r2, r0, TASK_THREAD
	stidx64	r4, r2, THREAD_R4
	stidx64	r5, r2, THREAD_R5
	stidx64	r6, r2, THREAD_R6
	stidx64	r7, r2, THREAD_R7
	stidx64	r15, r2, THREAD_R15
	stidx64	r3, r2, THREAD_RA
	getsp	r3
	stidx64	r3, r2, THREAD_SP
	mfsr	r3, tp
	stidx64	r3, r2, THREAD_TP

	addi	r2, r1, TASK_THREAD
	ldidx64	r4, r2, THREAD_R4
	ldidx64	r5, r2, THREAD_R5
	ldidx64	r6, r2, THREAD_R6
	ldidx64	r7, r2, THREAD_R7
	ldidx64	r15, r2, THREAD_R15
	ldidx64	r3, r2, THREAD_SP
	setsp	r3
	ldidx64	r3, r2, THREAD_TP
	mtsr	tp, r3
	mtsr	scratch0, r1
	copy	r12, r0
	ldidx64	r3, r2, THREAD_RA
	jmpr	r3
ENDPROC(__switch_to)
