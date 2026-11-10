/* SPDX-License-Identifier: GPL-2.0 */
/*
 * 64 KiB pages; lowmem is kseg: VA = PA + 0x8000_0000 for all 128 MiB of
 * DDR (klaussCPU MMU_PLAN.md §11.2).  Kernel pointers are 64-bit, holding
 * zero-extended 32-bit addresses.
 */
#ifndef _ASM_KLAUSSCPU_PAGE_H
#define _ASM_KLAUSSCPU_PAGE_H

#include <vdso/page.h>
#include <linux/const.h>

#define PAGE_OFFSET	_AC(0x80000000, UL)
#define PHYS_OFFSET	_AC(0, UL)
#define ARCH_PFN_OFFSET	(PHYS_OFFSET >> PAGE_SHIFT)

#ifndef __ASSEMBLER__

#define clear_page(page)	memset((page), 0, PAGE_SIZE)
#define copy_page(to, from)	memcpy((to), (from), PAGE_SIZE)
#define clear_user_page(page, vaddr, pg) \
	do { (void)(vaddr); (void)(pg); clear_page(page); } while (0)
#define copy_user_page(to, from, vaddr, pg) \
	do { (void)(vaddr); (void)(pg); copy_page(to, from); } while (0)

typedef struct { unsigned long pte; } pte_t;
typedef struct { unsigned long pgd; } pgd_t;
typedef struct { unsigned long pgprot; } pgprot_t;
typedef pte_t *pgtable_t;	/* a PTE table: a fragment of a page (asm/pgalloc.h) */

#define pte_val(x)	((x).pte)
#define pgd_val(x)	((x).pgd)
#define pgprot_val(x)	((x).pgprot)
#define __pte(x)	((pte_t) { (x) })
#define __pgd(x)	((pgd_t) { (x) })
#define __pgprot(x)	((pgprot_t) { (x) })

#define __pa(x)		((unsigned long)(x) - PAGE_OFFSET + PHYS_OFFSET)
#define __va(x)		((void *)((unsigned long)(x) + PAGE_OFFSET - PHYS_OFFSET))
#define __pa_symbol(x)	__pa(RELOC_HIDE((unsigned long)(x), 0))

static inline unsigned long virt_to_pfn(const void *kaddr)
{
	return __pa(kaddr) >> PAGE_SHIFT;
}

#define virt_to_page(vaddr)	pfn_to_page(virt_to_pfn(vaddr))
#define page_to_virt(page)	__va(page_to_pfn(page) << PAGE_SHIFT)

#define virt_addr_valid(kaddr)	((unsigned long)(kaddr) >= PAGE_OFFSET && \
				 pfn_valid(virt_to_pfn(kaddr)))

#endif /* !__ASSEMBLER__ */

#include <asm-generic/memory_model.h>
#include <asm-generic/getorder.h>

#endif
