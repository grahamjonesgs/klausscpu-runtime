// SPDX-License-Identifier: GPL-2.0
/*
 * Streaming DMA on caches that snoop no other DDR master (klaussCPU
 * MMIO_MAP, "Cache controller"): the dma-direct hooks clean the buffer
 * from the D-cache before a device reads or writes it, and invalidate it
 * (and see to the I-cache) before the CPU reads what a device wrote.  The
 * work, and why, is in mm/cacheflush.c (klausscpu_dma_clean / _inval).
 *
 * These hooks can't fail a mapping.  A walk that times out warns once; a
 * driver that must know (kernel/sd_native.c) maps with
 * DMA_ATTR_SKIP_CPU_SYNC and calls klausscpu_dma_clean / _inval itself,
 * which return false on a D-cache walk timeout.
 *
 * Every device is non-coherent (dma_default_coherent is false): there is
 * no uncached mapping for dma_alloc_coherent(), which fails; no driver
 * here needs it.
 */
#include <linux/bug.h>
#include <linux/dma-map-ops.h>
#include <asm/setup.h>

void arch_sync_dma_for_device(phys_addr_t paddr, size_t size, enum dma_data_direction dir)
{
	switch (dir) {
	case DMA_TO_DEVICE:
	case DMA_FROM_DEVICE:
	case DMA_BIDIRECTIONAL:
		klausscpu_dma_clean(paddr, size);
		break;
	default:
		WARN_ON_ONCE(1);
	}
}

void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size, enum dma_data_direction dir)
{
	switch (dir) {
	case DMA_TO_DEVICE:
		break;
	case DMA_FROM_DEVICE:
	case DMA_BIDIRECTIONAL:
		klausscpu_dma_inval(paddr, size);
		break;
	default:
		WARN_ON_ONCE(1);
	}
}
