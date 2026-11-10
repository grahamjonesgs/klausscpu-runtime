// SPDX-License-Identifier: GPL-2.0
/*
 * PTE tables as 8 KiB fragments of a 64 KiB page, after powerpc's
 * arch/powerpc/mm/pgtable-frag.c.  An mm takes its tables from its own
 * page in turn (mm->context.pte_frag is the next one); the page is freed
 * when every fragment it handed out is freed and the mm has dropped the
 * ones it didn't hand out (pte_frag_destroy, from destroy_context).
 *
 * Unlike powerpc the page is not allocated zeroed: each fragment is zeroed
 * when it is handed out (each is handed out once per page), so a process
 * with two tables zeroes 16 KiB instead of 64 KiB, at DDR write speed
 * (about 40 MB/s here) a saving of about 1.2 ms.
 *
 * ptdesc->pt_frag_refcount counts the fragments not yet freed: PTE_FRAG_NR
 * when the page goes into the mm's cache, 1 when it is used for a single
 * table (another table took the cache meanwhile).
 */
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <asm/pgalloc.h>

void pte_frag_destroy(void *pte_frag)
{
	struct ptdesc *ptdesc = virt_to_ptdesc(pte_frag);
	/* The fragments before pte_frag were handed out; drop the rest. */
	int handed = ((unsigned long)pte_frag & ~PAGE_MASK) >> PTE_FRAG_SIZE_SHIFT;

	if (atomic_sub_and_test(PTE_FRAG_NR - handed, &ptdesc->pt_frag_refcount))
		pagetable_dtor_free(ptdesc);
}

static pte_t *get_pte_from_cache(struct mm_struct *mm)
{
	void *ret, *next;

	spin_lock(&mm->page_table_lock);
	ret = mm->context.pte_frag;
	if (ret) {
		next = ret + PTE_FRAG_SIZE;
		/* The last fragment: the page leaves the cache. */
		if (!((unsigned long)next & ~PAGE_MASK))
			next = NULL;
		mm->context.pte_frag = next;
	}
	spin_unlock(&mm->page_table_lock);
	return ret;
}

static pte_t *alloc_for_ptecache(struct mm_struct *mm, int kernel)
{
	gfp_t gfp = GFP_KERNEL;
	struct ptdesc *ptdesc;
	void *ret;

	if (!kernel)
		gfp |= __GFP_ACCOUNT;
	ptdesc = pagetable_alloc(gfp, 0);
	if (!ptdesc)
		return NULL;
	if (!pagetable_pte_ctor(mm, ptdesc)) {
		pagetable_free(ptdesc);
		return NULL;
	}
	atomic_set(&ptdesc->pt_frag_refcount, 1);
	ret = ptdesc_address(ptdesc);
	spin_lock(&mm->page_table_lock);
	if (likely(!mm->context.pte_frag)) {
		atomic_set(&ptdesc->pt_frag_refcount, PTE_FRAG_NR);
		mm->context.pte_frag = ret + PTE_FRAG_SIZE;
	}
	spin_unlock(&mm->page_table_lock);
	return ret;
}

pte_t *pte_fragment_alloc(struct mm_struct *mm, int kernel)
{
	pte_t *pte = get_pte_from_cache(mm);

	if (!pte)
		pte = alloc_for_ptecache(mm, kernel);
	if (pte)
		memset(pte, 0, PTE_FRAG_SIZE);
	return pte;
}

void pte_fragment_free(pte_t *table, int kernel)
{
	struct ptdesc *ptdesc = virt_to_ptdesc(table);

	BUG_ON(atomic_read(&ptdesc->pt_frag_refcount) <= 0);
	if (atomic_dec_and_test(&ptdesc->pt_frag_refcount))
		pagetable_dtor_free(ptdesc);
}
