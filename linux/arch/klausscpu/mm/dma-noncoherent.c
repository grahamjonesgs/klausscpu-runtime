// SPDX-License-Identifier: GPL-2.0
/*
 * Streaming DMA on caches that snoop no other DDR master (klaussCPU
 * MMIO_MAP, "Cache controller"): the dma-direct hooks clean the buffer
 * from the D-cache before a device reads or writes it, and invalidate it
 * before the CPU reads what a device wrote; the I-cache side of a device
 * write is handled lazily (mm/cacheflush.c).  The walks are those of
 * mm/cacheflush.c: the range ops (CPU_FEATURES[27]) up to a crossover,
 * else the whole-cache FLUSH / INVALIDATE, each waited for, so a driver's
 * MMIO write that starts the DMA comes after the clean's writebacks are
 * in DDR.
 *
 * A walk that does not finish in time is counted
 * (klausscpu_cache_timeouts()): the hooks can't fail a mapping, so a
 * driver that cares compares the count around dma_map_*() and
 * dma_unmap_*() and treats a change as a failed mapping (kernel/sd_native.c
 * falls back to PIO, or fails the request).
 *
 * Every device is non-coherent (dma_default_coherent is false): there is
 * no uncached mapping for dma_alloc_coherent(), which fails; no driver
 * here needs it.
 */
#include <linux/bug.h>
#include <linux/dma-map-ops.h>
#include <asm/cache.h>
#include <asm/setup.h>

/*
 * Before the device touches the buffer, its dirty lines go to DDR: for
 * DMA_TO_DEVICE so the device reads the CPU's data, and for
 * DMA_FROM_DEVICE too, so that no dirty line of the buffer is written back
 * over the device's data while it is in flight.
 */
void arch_sync_dma_for_device(phys_addr_t paddr, size_t size, enum dma_data_direction dir)
{
	switch (dir) {
	case DMA_TO_DEVICE:
	case DMA_FROM_DEVICE:
	case DMA_BIDIRECTIONAL:
		klausscpu_dcache_walk(paddr, size, true, false);
		break;
	default:
		WARN_ON_ONCE(1);
	}
}

/*
 * After the device wrote the buffer, the CPU must not hit stale copies of
 * it: drop its lines, without a writeback (nothing of the buffer is dirty:
 * it was cleaned before the DMA and the CPU kept off it during it).
 *
 * That is only safe for lines the buffer owns whole.  A line shared with
 * other data that the CPU wrote during the DMA would lose that write if
 * dropped, and clobber the device's bytes if written back: neither way is
 * right, and only the buffer's alignment (ARCH_DMA_MINALIGN for kmalloc;
 * the SD driver takes a read by DMA only when it owns whole lines) keeps
 * it from happening.  So an unaligned buffer is a bug: warn, and clean and
 * invalidate its edge lines, which is right at least when the CPU did not
 * touch them during the DMA.
 *
 * The I-cache does not snoop DMA either: klausscpu_icache_dma_written()
 * drops the I-cache lines of the buffer now where it may be mapped
 * executable, else when its pages are next mapped executable.
 */
void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size, enum dma_data_direction dir)
{
	phys_addr_t mask = L1_CACHE_BYTES - 1;

	switch (dir) {
	case DMA_TO_DEVICE:
		return;
	case DMA_FROM_DEVICE:
	case DMA_BIDIRECTIONAL:
		break;
	default:
		WARN_ON_ONCE(1);
		return;
	}
	if (!size)
		return;
	if (WARN_ON_ONCE((paddr | size) & mask)) {
		klausscpu_dcache_walk(paddr & ~mask, L1_CACHE_BYTES, true, true);
		klausscpu_dcache_walk((paddr + size - 1) & ~mask, L1_CACHE_BYTES, true, true);
	}
	klausscpu_dcache_walk(paddr, size, false, true);
	klausscpu_icache_dma_written(paddr, size);
}
