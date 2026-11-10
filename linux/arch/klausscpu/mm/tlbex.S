/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TLB refill (TLBVEC, MMU_PLAN §11.3).  Taken on a kseg kernel stack (a
 * user miss has switched stacks; supervisor code always runs on one), so it
 * may push.  ECAUSE/EADDR/TLB_HI are loaded; TLB_HI = {VPN, ASID}.
 *
 * pgd = swapper_pg_dir for kmap (VA >= 0xC000_0000), else the current mm's,
 * which switch_mm keeps in SCRATCH1 (no memory load for it);
 * pte = pte_table[pgd[VA >> 26]][(VA >> 16) & 1023] (8 KiB tables,
 * asm/pgtable.h).  A present PTE goes to
 * TLB_LO as it is and is written at the next round-robin index (no wired
 * entries); anything
 * else falls through to the exception vector (page fault), with the
 * hardware state untouched.
 *
 * klausscpu_tlb_rr counts every refill (it is masked only to pick the
 * index), so /proc/klausscpu_perf can report refills.  The useg path is
 * the straight-line one; kmap (vmalloc, rare) branches off and back.
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
	setru	r2, KMAP_BASE
	mfsr	r1, scratch1		/* the current pgd */
	fbr	r0, r2, 11, 3f		/* UGE: kmap */
1:	shrv	r2, r0, PGD_SHIFT
	shlv	r2, r2, 3
	addr	r1, r1, r2
	ldidx64	r1, r1, 0		/* pgd entry: PA of the PTE table | 1 */
	bextr	r2, r0, 0xa10		/* VA[25:16], independent of the load */
	shlv	r2, r2, 3
	fbri	r1, 0, 0, 9f		/* none */
	andi	r1, r1, -8192		/* PTE_FRAG_SIZE */
	bset	r1, r1, 31		/* its kseg address */
	addr	r1, r1, r2
	ldidx64	r1, r1, 0		/* the PTE */
	setru	r0, klausscpu_tlb_rr	/* independent of the load */
	bextr	r2, r1, 0x104		/* PTE[4] PRESENT */
	fbri	r2, 0, 0, 9f
	mtsr	tlb_lo, r1
	ldidx64	r2, r0, 0
	addi	r2, r2, 1
	stidx64	r2, r0, 0
	/* TLB_INDEX[4:0]: with fewer than 32 entries the bits above log2 N
	 * are ignored (MMU_PLAN §11.3), so 31 serves every TLB size. */
	andi	r2, r2, 31
	mtsr	tlb_index, r2
	tlbwi
	pop	r2
	pop	r1
	pop	r0
	iret
	/* kmap: swapper_pg_dir, and the entry is global (G) for every ASID. */
3:	mfsr	r2, tlb_hi
	bset	r2, r2, 8
	mtsr	tlb_hi, r2
	setru	r1, swapper_pg_dir
	jmp	1b
9:	/* No PTE: undo the kmap G bit, then the page-fault path. */
	setru	r2, KMAP_BASE
	mfsr	r0, eaddr
	fbr	r0, r2, 3, 8f		/* ULT: useg, TLB_HI untouched */
	mfsr	r2, tlb_hi
	bclr	r2, r2, 8
	mtsr	tlb_hi, r2
8:	pop	r2
	pop	r1
	pop	r0
	jmp	klausscpu_exc_vector
ENDPROC(klausscpu_tlb_refill)

	.data
	.p2align 3
	.globl	klausscpu_tlb_rr
klausscpu_tlb_rr:	.quad 0
