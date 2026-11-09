/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TLB refill (TLBVEC, MMU_PLAN §11.3).  Taken on a kseg kernel stack (a
 * user miss has switched stacks; supervisor code always runs on one), so it
 * may push.  ECAUSE/EADDR/TLB_HI are loaded; TLB_HI = {VPN, ASID}.
 *
 * pgd = swapper_pg_dir for kmap (VA >= 0xC000_0000), else pgd_current;
 * pte = pte_page[pgd[VA >> 29]][(VA >> 16) & 8191].  A present PTE goes to
 * TLB_LO as it is and is written at the next round-robin index; anything
 * else falls through to the exception vector (page fault), with the
 * hardware state untouched.
 */
#include <linux/linkage.h>
#include <asm/asm-offsets.h>
#include <asm/klausscpu.h>

	.text
	.p2align 3
ENTRY(klausscpu_tlb_refill)
	push	r0
	push	r1
	push	r2
	mfsr	r0, eaddr
	setru	r1, swapper_pg_dir
	setru	r2, KMAP_BASE
	fbr	r0, r2, 11, 1f		/* UGE: kmap */
	setru	r1, pgd_current
	ldidx64	r1, r1, 0
1:	shrv	r2, r0, PGD_SHIFT
	shlv	r2, r2, 3
	addr	r1, r1, r2
	ldidx64	r1, r1, 0		/* pgd entry: PA of the PTE page | 1 */
	fbri	r1, 0, 0, 9f		/* none */
	andi	r1, r1, -65536
	bset	r1, r1, 31		/* its kseg address */
	bextr	r2, r0, 0xd10		/* VA[28:16] */
	shlv	r2, r2, 3
	addr	r1, r1, r2
	ldidx64	r1, r1, 0		/* the PTE */
	bextr	r2, r1, 0x104		/* PTE[4] PRESENT */
	fbri	r2, 0, 0, 9f
	mtsr	tlb_lo, r1
	setru	r1, klausscpu_tlb_rr
	ldidx64	r2, r1, 0
	addi	r2, r2, 1
	setru	r0, klausscpu_tlb_mask
	ldidx64	r0, r0, 0
	andr	r2, r2, r0
	stidx64	r2, r1, 0
	mtsr	tlb_index, r2
	tlbwi
	pop	r2
	pop	r1
	pop	r0
	iret
9:	pop	r2
	pop	r1
	pop	r0
	jmp	klausscpu_exc_vector
ENDPROC(klausscpu_tlb_refill)

	.data
	.p2align 3
	.globl	klausscpu_tlb_rr, klausscpu_tlb_mask
klausscpu_tlb_rr:	.quad 0
klausscpu_tlb_mask:	.quad 15
