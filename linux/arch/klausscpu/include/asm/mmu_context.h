/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_MMU_CONTEXT_H
#define _ASM_KLAUSSCPU_MMU_CONTEXT_H

#include <linux/mm_types.h>
#include <asm/tlbflush.h>

/* The pgd the TLB refill handler walks for useg (kmap uses swapper_pg_dir). */
extern pgd_t *pgd_current;

static inline void switch_mm(struct mm_struct *prev, struct mm_struct *next,
			     struct task_struct *tsk)
{
	if (prev == next)
		return;
	pgd_current = next->pgd;
	/* First version: no ASIDs, so every address-space change flushes. */
	local_flush_tlb_all();
}
#define switch_mm switch_mm

#include <asm-generic/mmu_context.h>

#endif
