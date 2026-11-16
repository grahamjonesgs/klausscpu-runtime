/* SPDX-License-Identifier: GPL-2.0 */
/*
 * The D-cache (64 KiB 2-way, 32 B lines) is VIPT but its index lies inside
 * the 64 KiB page offset, so it never aliases: it behaves as PIPT.  It
 * snoops nothing: the other DDR masters (the blitter, core 2, VGA scanout,
 * the SD controller's DMA) need a clean before a device reads and an
 * invalidate after a device writes (klaussCPU MMIO_MAP's "DMA-coherent
 * blit"; klausscpu_fb.c).  The streaming DMA API does that in
 * mm/dma-noncoherent.c, with the range ops (CPU_FEATURES[27]) or the
 * whole-cache FLUSH / INVALIDATE (mm/cacheflush.c); ARCH_DMA_MINALIGN = 32
 * (asm/cache.h: one dirty bit per 32 B line).  The L2, when present, is
 * coherent for every master.  The I-cache snoops CPU stores by line but not
 * aliases or DMA, so code made visible to fetch through a different mapping
 * needs ICACHE_INV, and a page a device wrote is invalidated in the
 * I-cache before user space executes it: lazily, through PG_dcache_clean
 * as on arm64 (set_ptes() below, mm/cacheflush.c).
 */
#ifndef _ASM_KLAUSSCPU_CACHEFLUSH_H
#define _ASM_KLAUSSCPU_CACHEFLUSH_H

#include <asm/klausscpu.h>

struct folio;
struct page;
struct vm_area_struct;

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

/*
 * PG_dcache_clean on a folio: the I-cache holds nothing stale of it (see
 * mm/cacheflush.c).  Clear on a new folio and after the kernel writes one.
 */
#define PG_dcache_clean PG_arch_1

#define ARCH_IMPLEMENTS_FLUSH_DCACHE_PAGE 1
void flush_dcache_page(struct page *page);
void flush_dcache_folio(struct folio *folio);
#define flush_dcache_folio flush_dcache_folio

#include <asm-generic/cacheflush.h>

#endif
