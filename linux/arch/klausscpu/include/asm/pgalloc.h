/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_PGALLOC_H
#define _ASM_KLAUSSCPU_PGALLOC_H

#include <linux/mm.h>
#include <asm/tlb.h>

#define __HAVE_ARCH_PGD_FREE
#include <asm-generic/pgalloc.h>

static inline void pmd_populate_kernel(struct mm_struct *mm, pmd_t *pmd, pte_t *pte)
{
	set_pmd(pmd, __pmd(__pa(pte) | _PAGE_TABLE));
}

static inline void pmd_populate(struct mm_struct *mm, pmd_t *pmd, pgtable_t pte)
{
	set_pmd(pmd, __pmd((page_to_pfn(pte) << PAGE_SHIFT) | _PAGE_TABLE));
}

/* The pgd is 8 entries (64 bytes); the kernel (kmap) half is copied in. */
extern pgd_t *pgd_alloc(struct mm_struct *mm);
extern void pgd_free(struct mm_struct *mm, pgd_t *pgd);

#define __pte_free_tlb(tlb, pte, addr)	tlb_remove_ptdesc((tlb), page_ptdesc(pte))

#endif
