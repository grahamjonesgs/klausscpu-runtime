/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 8-bit ASIDs (SR ASID; TLB_HI[7:0]) with a generation count above them,
 * as MIPS does: mm->context.asid = generation << 8 | asid.  A switch only
 * writes SR ASID; when the 256 ASIDs of a generation are used up the whole
 * TLB is invalidated and a new generation starts.  kmap entries are global
 * (G), so they survive switches.
 */
#ifndef _ASM_KLAUSSCPU_MMU_CONTEXT_H
#define _ASM_KLAUSSCPU_MMU_CONTEXT_H

#include <linux/mm_types.h>
#include <asm/tlbflush.h>
#include <asm/klausscpu.h>
#include <asm/page.h>

/* The CPU has the hardware TLB walker (CPU_FEATURES[25]; mm/tlbwalk.c). */
extern bool klausscpu_tlb_walker;

#define ASID_MASK		0xffUL
#define ASID_VERSION_MASK	(~ASID_MASK)
#define ASID_FIRST_VERSION	(ASID_MASK + 1)

/*
 * The pgd the TLB refill handler walks for useg (kmap uses swapper_pg_dir)
 * is in SR SCRATCH1, set here and in head.S (swapper_pg_dir).
 */
/* The last ASID handed out, with its generation. */
extern unsigned long asid_cache;

static inline void get_new_mmu_context(struct mm_struct *mm)
{
	unsigned long asid = asid_cache + 1;

	if (!(asid & ASID_MASK)) {
		/* A new generation: no old ASID may stay in the TLB. */
		local_flush_tlb_all();
		if (!asid)
			asid = ASID_FIRST_VERSION;
	}
	mm->context.asid = asid_cache = asid;
}

static inline void check_mmu_context(struct mm_struct *mm)
{
	if ((mm->context.asid ^ asid_cache) & ASID_VERSION_MASK)
		get_new_mmu_context(mm);
}

#define init_new_context init_new_context
static inline int init_new_context(struct task_struct *tsk, struct mm_struct *mm)
{
	/* dup_mm copied the parent's context: a new ASID, and no PTE page. */
	mm->context.asid = 0;
	mm->context.pte_frag = NULL;
	return 0;
}

/* Drop the PTE tables this mm's page never handed out (mm/pgalloc.c). */
void pte_frag_destroy(void *pte_frag);
#define destroy_context destroy_context
static inline void destroy_context(struct mm_struct *mm)
{
	if (mm->context.pte_frag)
		pte_frag_destroy(mm->context.pte_frag);
}

static inline void switch_mm(struct mm_struct *prev, struct mm_struct *next,
			     struct task_struct *tsk)
{
	unsigned long flags;

	if (prev == next)
		return;
	local_irq_save(flags);
	check_mmu_context(next);
	mtsr(SR_ASID, next->context.asid & ASID_MASK);
	mtsr(SR_SCRATCH1, (unsigned long)next->pgd);
	/* The hardware walker's pgd (mm/tlbwalk.c): kept current whether
	 * the walker is on or not, so it can be switched on at any time. */
	if (klausscpu_tlb_walker)
		mtsr(SR_TLB_PTB, __pa(next->pgd));
	local_irq_restore(flags);
}
#define switch_mm switch_mm

#include <asm-generic/mm_hooks.h>
#include <asm-generic/mmu_context.h>

#endif
