/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Two-level page tables (pgd -> pte, pmd folded) over a 32-bit address
 * space with 64 KiB pages: a PTE page holds 8192 entries (512 MiB), so the
 * pgd has 8.  See LINUX_PLAN.md "Pages and page tables".
 *
 * PTE layout (64 bits).  The hardware bits are TLB_LO's, so the refill
 * handler writes the PTE to TLB_LO unmasked (MTSR keeps only [26:16] and
 * [3:0]):
 *   [0]  V   hardware: valid (= present)
 *   [1]  R   hardware: readable
 *   [2]  W   hardware: writable, set only when WRITE and DIRTY are both set
 *   [3]  X   hardware: executable
 *   [4]  PRESENT   software
 *   [6]  WRITE     software: logically writable
 *   [7]  DIRTY
 *   [8]  ACCESSED
 *   [9]  SPECIAL
 *   [26:16] PFN
 * A swap/migration entry (not present): [9:5] type, [10] exclusive,
 * [63:16] offset; [4] and [0] clear.
 */
#ifndef _ASM_KLAUSSCPU_PGTABLE_H
#define _ASM_KLAUSSCPU_PGTABLE_H

#include <asm/page.h>
#include <asm/klausscpu.h>
#ifndef __ASSEMBLER__
#include <asm-generic/pgtable-nopmd.h>
#endif

#define PGDIR_SHIFT	(PAGE_SHIFT + (PAGE_SHIFT - 3))		/* 29 */
#define PGDIR_SIZE	(_AC(1, UL) << PGDIR_SHIFT)
#define PGDIR_MASK	(~(PGDIR_SIZE - 1))
#define PTRS_PER_PTE	(_AC(1, UL) << (PAGE_SHIFT - 3))		/* 8192 */
#define PTRS_PER_PGD	(_AC(1, UL) << (32 - PGDIR_SHIFT))	/* 8 */

#define TASK_SIZE		_AC(0x80000000, UL)
#define USER_PTRS_PER_PGD	(TASK_SIZE / PGDIR_SIZE)

/* kmap 0xC000_0000-0xEFFF_FFFF: vmalloc, ioremap of non-MMIO, etc. */
#define VMALLOC_START	KMAP_BASE
#define VMALLOC_END	(KMAP_END - PAGE_SIZE)

#define _PAGE_VALID	0x001UL
#define _PAGE_HW_READ	0x002UL
#define _PAGE_HW_WRITE	0x004UL
#define _PAGE_HW_EXEC	0x008UL
#define _PAGE_PRESENT	0x010UL
#define _PAGE_WRITE	0x040UL
#define _PAGE_DIRTY	0x080UL
#define _PAGE_ACCESSED	0x100UL
#define _PAGE_SPECIAL	0x200UL
#define _PAGE_SWP_EXCLUSIVE	0x400UL
#define _PAGE_PFN_MASK	0x07ff0000UL

/* A pgd entry: the PA of a PTE page | _PAGE_TABLE. */
#define _PAGE_TABLE	0x001UL

#define _PAGE_BASE	(_PAGE_PRESENT | _PAGE_VALID | _PAGE_ACCESSED)
#define _PAGE_CHG_MASK	(_PAGE_PFN_MASK | _PAGE_DIRTY | _PAGE_ACCESSED | _PAGE_SPECIAL)

#define PAGE_NONE		__pgprot(_PAGE_BASE)
#define PAGE_READONLY		__pgprot(_PAGE_BASE | _PAGE_HW_READ)
#define PAGE_READONLY_EXEC	__pgprot(_PAGE_BASE | _PAGE_HW_READ | _PAGE_HW_EXEC)
#define PAGE_COPY		PAGE_READONLY
#define PAGE_COPY_EXEC		PAGE_READONLY_EXEC
#define PAGE_SHARED		__pgprot(_PAGE_BASE | _PAGE_HW_READ | _PAGE_WRITE)
#define PAGE_SHARED_EXEC	__pgprot(_PAGE_BASE | _PAGE_HW_READ | _PAGE_WRITE | _PAGE_HW_EXEC)
#define PAGE_KERNEL		__pgprot(_PAGE_BASE | _PAGE_HW_READ | _PAGE_WRITE | \
					 _PAGE_DIRTY | _PAGE_HW_WRITE)
#define PAGE_KERNEL_RO		__pgprot(_PAGE_BASE | _PAGE_HW_READ)
#define PAGE_KERNEL_EXEC	__pgprot(pgprot_val(PAGE_KERNEL) | _PAGE_HW_EXEC)
#define PAGE_KERNEL_RX		__pgprot(_PAGE_BASE | _PAGE_HW_READ | _PAGE_HW_EXEC)

/* No cache-inhibit bit: MMIO has its own identity segment. */
#define _PAGE_IOREMAP		pgprot_val(PAGE_KERNEL)
#define pgprot_noncached(prot)		(prot)
#define pgprot_writecombine(prot)	(prot)

#ifndef __ASSEMBLER__

struct mm_struct;
struct vm_area_struct;
struct vm_fault;

extern pgd_t swapper_pg_dir[PTRS_PER_PGD];
extern void paging_init(void);

/* Hardware W follows WRITE && DIRTY (the first store faults to dirty it). */
static inline unsigned long pte_hw_fix(unsigned long v)
{
	if ((v & (_PAGE_WRITE | _PAGE_DIRTY)) == (_PAGE_WRITE | _PAGE_DIRTY))
		v |= _PAGE_HW_WRITE;
	else
		v &= ~_PAGE_HW_WRITE;
	return v;
}

static inline void set_pte(pte_t *ptep, pte_t pte)
{
	WRITE_ONCE(*ptep, pte);
}

#define pte_none(x)	(!pte_val(x))
#define pte_present(x)	(pte_val(x) & _PAGE_PRESENT)
#define pte_clear(mm, addr, ptep)	set_pte(ptep, __pte(0))

static inline int pte_read(pte_t pte)    { return pte_val(pte) & _PAGE_HW_READ; }
static inline int pte_write(pte_t pte)   { return pte_val(pte) & _PAGE_WRITE; }
static inline int pte_exec(pte_t pte)    { return pte_val(pte) & _PAGE_HW_EXEC; }
static inline int pte_dirty(pte_t pte)   { return pte_val(pte) & _PAGE_DIRTY; }
static inline int pte_young(pte_t pte)   { return pte_val(pte) & _PAGE_ACCESSED; }
static inline int pte_special(pte_t pte) { return pte_val(pte) & _PAGE_SPECIAL; }

#define PTE_OP(name, expr)					\
static inline pte_t name(pte_t pte)				\
{								\
	unsigned long v = pte_val(pte);				\
	expr;							\
	return __pte(pte_hw_fix(v));				\
}
PTE_OP(pte_wrprotect,     v &= ~_PAGE_WRITE)
PTE_OP(pte_mkwrite_novma, v |= _PAGE_WRITE)
PTE_OP(pte_mkclean,       v &= ~_PAGE_DIRTY)
PTE_OP(pte_mkdirty,       v |= _PAGE_DIRTY)
PTE_OP(pte_mkold,         v &= ~_PAGE_ACCESSED)
PTE_OP(pte_mkyoung,       v |= _PAGE_ACCESSED)
PTE_OP(pte_mkspecial,     v |= _PAGE_SPECIAL)
PTE_OP(pte_mkexec,        v |= _PAGE_HW_EXEC)
PTE_OP(pte_exprotect,     v &= ~_PAGE_HW_EXEC)
#undef PTE_OP

#define PFN_PTE_SHIFT	PAGE_SHIFT
#define pte_pfn(pte)	((pte_val(pte) & _PAGE_PFN_MASK) >> PAGE_SHIFT)
#define pfn_pte(pfn, prot) \
	__pte(pte_hw_fix(((unsigned long)(pfn) << PAGE_SHIFT) | pgprot_val(prot)))
#define pte_page(pte)	pfn_to_page(pte_pfn(pte))

static inline pte_t pte_modify(pte_t pte, pgprot_t newprot)
{
	return __pte(pte_hw_fix((pte_val(pte) & _PAGE_CHG_MASK) | pgprot_val(newprot)));
}

/* The folded pmd is the pgd entry: PA of the PTE page | _PAGE_TABLE. */
#define pmd_none(pmd)		(!pmd_val(pmd))
#define pmd_bad(pmd)		((pmd_val(pmd) & ~PAGE_MASK) != _PAGE_TABLE)
#define pmd_present(pmd)	(pmd_val(pmd) & _PAGE_TABLE)
#define pmd_clear(pmdp)		WRITE_ONCE(*(pmdp), __pmd(0))
#define set_pmd(pmdp, pmd)	WRITE_ONCE(*(pmdp), (pmd))
#define pmd_pfn(pmd)		(pmd_val(pmd) >> PAGE_SHIFT)
#define pmd_page(pmd)		pfn_to_page(pmd_pfn(pmd))

static inline unsigned long pmd_page_vaddr(pmd_t pmd)
{
	return (unsigned long)__va(pmd_val(pmd) & PAGE_MASK);
}

#define pte_ERROR(e) pr_err("%s:%d: bad pte %016lx\n", __FILE__, __LINE__, pte_val(e))
#define pgd_ERROR(e) pr_err("%s:%d: bad pgd %016lx\n", __FILE__, __LINE__, pgd_val(e))

/* Swap and migration entries. */
#define __swp_type(x)		(((x).val >> 5) & 0x1f)
#define __swp_offset(x)		((x).val >> 16)
#define __swp_entry(type, offset) \
	((swp_entry_t) { (((type) & 0x1f) << 5) | ((unsigned long)(offset) << 16) })
#define __pte_to_swp_entry(pte)	((swp_entry_t) { pte_val(pte) })
#define __swp_entry_to_pte(x)	((pte_t) { (x).val })

static inline bool pte_swp_exclusive(pte_t pte)
{
	return pte_val(pte) & _PAGE_SWP_EXCLUSIVE;
}
static inline pte_t pte_swp_mkexclusive(pte_t pte)
{
	return __pte(pte_val(pte) | _PAGE_SWP_EXCLUSIVE);
}
static inline pte_t pte_swp_clear_exclusive(pte_t pte)
{
	return __pte(pte_val(pte) & ~_PAGE_SWP_EXCLUSIVE);
}

extern void update_mmu_cache_range(struct vm_fault *vmf, struct vm_area_struct *vma,
				   unsigned long address, pte_t *ptep, unsigned int nr);
#define update_mmu_cache(vma, addr, ptep) \
	update_mmu_cache_range(NULL, vma, addr, ptep, 1)

#endif /* !__ASSEMBLER__ */
#endif
