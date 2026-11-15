/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The D-cache (64 KiB 2-way, 32 B lines) is VIPT but its index lies inside
 * the 64 KiB page offset, so it never aliases: it behaves as PIPT.  It
 * snoops nothing: the other DDR masters (the blitter, core 2, VGA scanout)
 * need CACHE_CTRL FLUSH before a device reads and INVALIDATE after a device
 * writes (klaussCPU MMIO_MAP's "DMA-coherent blit"; klausscpu_fb.c).  No
 * kernel driver DMAs into DDR today (the SD card is PIO, LiteEth has its own
 * buffer RAM); a DMA driver will need ARCH_DMA_MINALIGN = 32 (one dirty bit
 * per 32 B line) and the range clean/invalidate ops (CLANG_ON_BOARD_PLAN.md:
 * the SD DMA draft).  The L2, when present, is coherent for every master.
 * The I-cache snoops CPU stores by line but not aliases or DMA, so code made
 * visible to fetch through a different mapping needs ICACHE_INV.
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
