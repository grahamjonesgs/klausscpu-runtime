/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The D-cache is PIPT and coherent with DMA; the I-cache snoops CPU stores
 * by line but not aliases or DMA, so code made visible to fetch through a
 * different mapping needs ICACHE_INV.
 */
#ifndef _ASM_KLAUSSCPU_CACHEFLUSH_H
#define _ASM_KLAUSSCPU_CACHEFLUSH_H

#include <asm/klausscpu.h>

static inline void flush_icache_range(unsigned long start, unsigned long end)
{
	kl_icache_invalidate();
}
#define flush_icache_range flush_icache_range

static inline void flush_icache_user_page(struct vm_area_struct *vma,
					  struct page *page, unsigned long addr, int len)
{
	kl_icache_invalidate();
}
#define flush_icache_user_page flush_icache_user_page

#include <asm-generic/cacheflush.h>

#endif
