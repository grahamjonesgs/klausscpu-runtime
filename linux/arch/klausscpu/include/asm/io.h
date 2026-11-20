/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_KLAUSSCPU_IO_H
#define _ASM_KLAUSSCPU_IO_H

#include <linux/types.h>

/*
 * /dev/mem (drivers/char/mem.c) may reach RAM only.  The generic
 * valid_mmap_phys_addr_range() allows any page, but a KlaussCPU PTE holds
 * page frames of RAM alone (_PAGE_PFN_MASK) and has no cache-inhibit bit:
 * MMIO (0xF000_0000-) is reachable only through the kernel's identity
 * segment.  An mmap of an MMIO address was truncated into RAM, so a
 * "register" store from user space landed in kernel memory.  mm/init.c.
 */
#define ARCH_HAS_VALID_PHYS_ADDR_RANGE
extern int valid_phys_addr_range(phys_addr_t addr, size_t size);
extern int valid_mmap_phys_addr_range(unsigned long pfn, size_t size);

#include <asm-generic/io.h>

#endif /* _ASM_KLAUSSCPU_IO_H */
