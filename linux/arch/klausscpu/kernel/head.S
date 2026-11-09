/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Entry from the klausscc loader: physical, TE = 0, interrupts as reset
 * left them.  Wire a global identity TLB entry for this code's 64 KiB page
 * (the instruction after the MTSR that sets TE is fetched translated,
 * MMU_PLAN §11.2), turn translation on, jump to the kseg link address, drop
 * the entry, clear BSS, and call start_kernel on init_task's stack.
 */
#include <linux/linkage.h>
#include <linux/init.h>
#include <asm/asm-offsets.h>
#include <asm/klausscpu.h>
#include <asm/thread_info.h>

	__HEAD
	.globl	_start
_start:
	di	r0
	setr	r1, 0
	mtsr	tlb_index, r1
	setru	r1, TLB_HI_G		/* VPN 0, global */
	mtsr	tlb_hi, r1
	setr	r1, 0xb			/* PFN 0, X | R | V */
	mtsr	tlb_lo, r1
	tlbwi
	mfsr	r1, status
	bset	r1, r1, 9		/* TE */
	mtsr	status, r1
	setru	r1, 1f			/* the kseg alias */
	jmpr	r1
1:
	tlbinv				/* drop the identity entry */

	/* Clear BSS. */
	setru	r1, __bss_start
	setru	r2, __bss_stop
	setr	r3, 0
2:	fbr	r1, r2, 11, 3f		/* UGE: done */
	stidx64	r3, r1, 0
	addi	r1, r1, 8
	jmp	2b
3:
	/* init_task's stack, with room for its pt_regs; current = &init_task. */
	setru	r1, init_thread_union + THREAD_SIZE - PT_SIZE
	setsp	r1
	setru	r1, init_task
	mtsr	scratch0, r1
	setr	r15, 0
	call	start_kernel
4:	halt
	jmp	4b
