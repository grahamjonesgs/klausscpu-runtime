// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <asm/pgtable.h>

pgd_t swapper_pg_dir[PTRS_PER_PGD] __aligned(64);

/* The core calls free_area_init() itself and asks for the zone limits. */
void __init arch_zone_limits_init(unsigned long *max_zone_pfns)
{
	max_zone_pfns[ZONE_NORMAL] = max_low_pfn;
}

void __init paging_init(void)
{
}

static const pgprot_t protection_map[16] = {
	[VM_NONE]					= PAGE_NONE,
	[VM_READ]					= PAGE_READONLY,
	[VM_WRITE]					= PAGE_COPY,
	[VM_WRITE | VM_READ]				= PAGE_COPY,
	[VM_EXEC]					= PAGE_READONLY_EXEC,
	[VM_EXEC | VM_READ]				= PAGE_READONLY_EXEC,
	[VM_EXEC | VM_WRITE]				= PAGE_COPY_EXEC,
	[VM_EXEC | VM_WRITE | VM_READ]			= PAGE_COPY_EXEC,
	[VM_SHARED]					= PAGE_NONE,
	[VM_SHARED | VM_READ]				= PAGE_READONLY,
	[VM_SHARED | VM_WRITE]				= PAGE_SHARED,
	[VM_SHARED | VM_WRITE | VM_READ]		= PAGE_SHARED,
	[VM_SHARED | VM_EXEC]				= PAGE_READONLY_EXEC,
	[VM_SHARED | VM_EXEC | VM_READ]			= PAGE_READONLY_EXEC,
	[VM_SHARED | VM_EXEC | VM_WRITE]		= PAGE_SHARED_EXEC,
	[VM_SHARED | VM_EXEC | VM_WRITE | VM_READ]	= PAGE_SHARED_EXEC,
};
DECLARE_VM_GET_PAGE_PROT

/*
 * /dev/mem's range checks (asm/io.h): RAM only, for read/write and for
 * mmap.  The limits are tested so that a huge user offset cannot wrap.
 */
int valid_phys_addr_range(phys_addr_t addr, size_t size)
{
	phys_addr_t end = __pa(high_memory);

	return addr <= end && size <= end - addr;
}

int valid_mmap_phys_addr_range(unsigned long pfn, size_t size)
{
	unsigned long end = PHYS_PFN(__pa(high_memory));

	return pfn < end && size <= (end - pfn) << PAGE_SHIFT;
}
