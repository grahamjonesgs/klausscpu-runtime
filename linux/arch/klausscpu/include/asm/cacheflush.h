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
 * coherent for every master.  The I-cache snoops CPU stores (by physical
 * tag) but not DMA; flush_icache_range() and ptrace's writes still drop it
 * whole (ICACHE_INV also clears the BTIC and fetch window).  A page a
 * device wrote is invalidated in the I-cache before user space executes
 * it: at once if it may be mapped executable, else when it first is
 * (PG_dcache_clean; set_ptes() in asm/pgtable.h, mm/cacheflush.c).
 */
#ifndef _ASM_KLAUSSCPU_CACHEFLUSH_H
#define _ASM_KLAUSSCPU_CACHEFLUSH_H

#include <asm/klausscpu.h>

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
 * PG_dcache_clean on a folio: it may have been mapped executable since it
 * was allocated, its I-cache lines invalidated then (mm/cacheflush.c).
 * Only the page allocator clears it.  flush_dcache_page / _folio stay the
 * generic no-ops: CPU stores never leave the I-cache stale (it snoops them
 * by physical tag), and clearing the flag there would make a DMA into a
 * mapped executable page skip its invalidate.
 */
#define PG_dcache_clean PG_arch_1

#include <asm-generic/cacheflush.h>

#endif
