// SPDX-License-Identifier: GPL-2.0
/*
 * TLB maintenance with ASIDs (asm/mmu_context.h).  An mm's entries go
 * away either one by one (TLBP with the mm's ASID, then an invalid entry
 * written at that index) or all at once by giving the mm a new ASID.
 */
#include <linux/mm.h>
#include <linux/sched.h>
#include <asm/klausscpu.h>
#include <asm/mmu_context.h>
#include <asm/tlbflush.h>
#include <asm/pgtable.h>

unsigned long asid_cache = ASID_FIRST_VERSION;

/* Up to this many pages are probed one by one; more drop the ASID. */
#define FLUSH_PAGES_MAX	8

/* Invalidate the entry for the page of addr in address space asid, if any. */
static void flush_one(unsigned long addr, unsigned long asid)
{
	unsigned long old_hi = mfsr(SR_TLB_HI);

	mtsr(SR_TLB_HI, (addr & PAGE_MASK & 0xffffffffUL) | (asid & ASID_MASK));
	tlb_probe();
	if (!(mfsr(SR_TLB_INDEX) & TLB_INDEX_P)) {
		mtsr(SR_TLB_LO, 0);
		tlb_write_indexed();
	}
	mtsr(SR_TLB_HI, old_hi);
}

/* Does mm hold an ASID of the current generation (else it has no entries)? */
static bool mm_has_asid(struct mm_struct *mm)
{
	return !((mm->context.asid ^ asid_cache) & ASID_VERSION_MASK);
}

void local_flush_tlb_mm(struct mm_struct *mm)
{
	unsigned long flags;

	local_irq_save(flags);
	if (mm == current->active_mm) {
		get_new_mmu_context(mm);
		mtsr(SR_ASID, mm->context.asid & ASID_MASK);
	} else {
		mm->context.asid = 0;	/* a new ASID at the next switch */
	}
	local_irq_restore(flags);
}

void local_flush_tlb_page(struct vm_area_struct *vma, unsigned long addr)
{
	struct mm_struct *mm = vma ? vma->vm_mm : current->active_mm;
	unsigned long flags;

	local_irq_save(flags);
	if (mm_has_asid(mm))
		flush_one(addr, mm->context.asid);
	local_irq_restore(flags);
}

void local_flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
			   unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;
	unsigned long flags;

	if ((end - start) >> PAGE_SHIFT > FLUSH_PAGES_MAX) {
		local_flush_tlb_mm(mm);
		return;
	}
	local_irq_save(flags);
	if (mm_has_asid(mm))
		for (start &= PAGE_MASK; start < end; start += PAGE_SIZE)
			flush_one(start, mm->context.asid);
	local_irq_restore(flags);
}

/* kmap entries are global: a probe with any ASID finds them. */
void local_flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	unsigned long flags;

	if ((end - start) >> PAGE_SHIFT > FLUSH_PAGES_MAX) {
		local_flush_tlb_all();
		return;
	}
	local_irq_save(flags);
	for (start &= PAGE_MASK; start < end; start += PAGE_SIZE)
		flush_one(start, 0);
	local_irq_restore(flags);
}

void update_mmu_cache_range(struct vm_fault *vmf, struct vm_area_struct *vma,
			    unsigned long address, pte_t *ptep, unsigned int nr)
{
	while (nr--) {
		local_flush_tlb_page(vma, address);
		address += PAGE_SIZE;
	}
}
