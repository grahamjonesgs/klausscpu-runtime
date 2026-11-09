/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_TLBFLUSH_H
#define _ASM_KLAUSSCPU_TLBFLUSH_H

#include <linux/mm.h>
#include <asm/klausscpu.h>

static inline void local_flush_tlb_all(void)
{
	tlb_invalidate_all();
}

extern void local_flush_tlb_mm(struct mm_struct *mm);
extern void local_flush_tlb_page(struct vm_area_struct *vma, unsigned long addr);
extern void local_flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
				  unsigned long end);
extern void local_flush_tlb_kernel_range(unsigned long start, unsigned long end);

#define flush_tlb_all()			local_flush_tlb_all()
#define flush_tlb_mm(mm)		local_flush_tlb_mm(mm)
#define flush_tlb_range(vma, s, e)	local_flush_tlb_range(vma, s, e)
#define flush_tlb_page(vma, addr)	local_flush_tlb_page(vma, addr)
#define flush_tlb_kernel_range(s, e)	local_flush_tlb_kernel_range(s, e)

#endif
