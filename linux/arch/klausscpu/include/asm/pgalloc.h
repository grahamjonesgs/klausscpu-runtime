/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_PGALLOC_H
#define _ASM_KLAUSSCPU_PGALLOC_H

#include <linux/mm.h>
#include <asm/tlb.h>

/*
 * PTE tables are 8 KiB fragments of a 64 KiB page (asm/pgtable.h), handed
 * out per mm as powerpc does (arch/klausscpu/mm/pgalloc.c).  So pgtable_t
 * is the table's address, and asm-generic/pgalloc.h (whole-page tables,
 * pte_free taking a struct page) is not used.
 */
pte_t *pte_fragment_alloc(struct mm_struct *mm, int kernel);
void pte_fragment_free(pte_t *table, int kernel);
void pte_frag_destroy(void *pte_frag);

static inline pte_t *pte_alloc_one_kernel(struct mm_struct *mm)
{
	return pte_fragment_alloc(mm, 1);
}

static inline pgtable_t pte_alloc_one(struct mm_struct *mm)
{
	return pte_fragment_alloc(mm, 0);
}

static inline void pte_free_kernel(struct mm_struct *mm, pte_t *pte)
{
	pte_fragment_free(pte, 1);
}

static inline void pte_free(struct mm_struct *mm, pgtable_t pte)
{
	pte_fragment_free(pte, 0);
}

static inline void pmd_populate_kernel(struct mm_struct *mm, pmd_t *pmd, pte_t *pte)
{
	set_pmd(pmd, __pmd(__pa(pte) | _PAGE_TABLE));
}

static inline void pmd_populate(struct mm_struct *mm, pmd_t *pmd, pgtable_t pte)
{
	set_pmd(pmd, __pmd(__pa(pte) | _PAGE_TABLE));
}

/* The pgd is 64 entries (512 bytes); the kernel (kmap) half is copied in. */
extern pgd_t *pgd_alloc(struct mm_struct *mm);
extern void pgd_free(struct mm_struct *mm, pgd_t *pgd);

/*
 * Freed at once, not after the gather's TLB flush: free_pte_range clears
 * the pgd entry first, and the only walker is this CPU's refill handler
 * (no SMP), so nothing can reach the table any more; the TLB caches no
 * table pointers.
 */
#define __pte_free_tlb(tlb, pte, addr)	pte_fragment_free((pte), 0)

#endif
