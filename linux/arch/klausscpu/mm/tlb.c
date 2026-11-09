// SPDX-License-Identifier: GPL-2.0
#include <linux/mm.h>
#include <asm/klausscpu.h>
#include <asm/tlbflush.h>
#include <asm/pgtable.h>

/* Probe for the page's entry and, if there is one, make it invalid (V = 0). */
void local_flush_tlb_page(struct vm_area_struct *vma, unsigned long addr)
{
	unsigned long flags;

	local_irq_save(flags);
	mtsr(SR_TLB_HI, addr & PAGE_MASK & 0xffffffffUL);
	tlb_probe();
	if (!(mfsr(SR_TLB_INDEX) & TLB_INDEX_P)) {
		mtsr(SR_TLB_LO, 0);
		tlb_write_indexed();
	}
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
