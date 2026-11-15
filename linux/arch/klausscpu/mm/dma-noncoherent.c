// SPDX-License-Identifier: GPL-2.0
/*
 * Streaming DMA on a D-cache that snoops nothing (klaussCPU MMIO_MAP,
 * "Cache controller"): the dma-direct hooks clean the buffer before a
 * device reads or writes it, and invalidate it before the CPU reads what a
 * device wrote.
 *
 * The cache controller's range ops (CPU_FEATURES[27]: CACHE_OP_ADDR,
 * CACHE_OP_LEN, CACHE_CTRL RANGE_CLEAN / RANGE_INV) walk only the lines of
 * the buffer.  Without them, or for a buffer larger than the cache, where
 * a walk of the whole cache (1024 sets x 2 ways) is no slower, the
 * whole-cache FLUSH (clean) or INVALIDATE (clean, then drop everything)
 * does it.  Each op is synchronous for the CPU's next cached access, not
 * for MMIO: so this waits for CACHE_STATUS.MNT_BUSY to drop before
 * returning, and a driver's MMIO write that starts the DMA comes after
 * the clean's writebacks are in DDR.  It also waits before an op, since
 * a walk in flight may still be using CACHE_OP_ADDR / CACHE_OP_LEN.
 *
 * Every device is non-coherent (dma_default_coherent is false): there is
 * no uncached mapping for dma_alloc_coherent(), which fails; no driver
 * here needs it.
 */
#include <linux/bug.h>
#include <linux/dma-map-ops.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/processor.h>
#include <asm/cache.h>
#include <asm/klausscpu.h>
#include <asm/setup.h>

/* Above this a range walk is no faster than a whole-cache one (64 KiB). */
#define KL_DMA_RANGE_MAX	(64 * 1024)
/* MNT_BUSY polls before giving up: a whole-cache walk is a few thousand
 * cycles, a range walk about a cycle or two per line. */
#define KL_CACHE_WAIT_POLLS	1000000

static void __iomem *kl_cache_reg(unsigned int off)
{
	return (void __iomem *)(KL_CACHE_BASE + off);
}

/*
 * The D-cache range ops exist.  The one test for them, so that the
 * feature bit can change in one place.
 */
bool klausscpu_dcache_range_ops(void)
{
	return klausscpu_cpu_features & CPU_FEAT_DCACHE_RANGE;
}

static void kl_cache_wait(void)
{
	unsigned long n = KL_CACHE_WAIT_POLLS;

	while (readq(kl_cache_reg(KL_CACHE_STATUS)) & KL_CACHE_MNT_BUSY) {
		if (!--n) {
			WARN_ONCE(1, "klausscpu: D-cache walk did not finish\n");
			return;
		}
		cpu_relax();
	}
}

/*
 * One walk: with the range ops, of the lines [paddr, paddr + size) touches
 * (clean writes the dirty ones back, inv drops them, both = clean and drop);
 * without, of the whole cache (FLUSH for a clean, INVALIDATE otherwise,
 * which writes back before it drops, so it is a clean and invalidate).
 */
static void kl_dcache_walk(phys_addr_t paddr, size_t size, bool clean, bool inv)
{
	unsigned long flags;
	phys_addr_t start;
	u64 ctrl;

	if (!size)
		return;
	local_irq_save(flags);
	kl_cache_wait();
	if (klausscpu_dcache_range_ops() && size <= KL_DMA_RANGE_MAX) {
		/* CACHE_OP_ADDR[4:0] are ignored: start at the line and count
		 * the length from there, whichever way the walk reads them. */
		start = paddr & ~(phys_addr_t)(L1_CACHE_BYTES - 1);
		writeq(start, kl_cache_reg(KL_CACHE_OP_ADDR));
		writeq(size + (paddr - start), kl_cache_reg(KL_CACHE_OP_LEN));
		ctrl = (clean ? KL_CACHE_RANGE_CLEAN : 0) | (inv ? KL_CACHE_RANGE_INV : 0);
	} else {
		ctrl = inv ? KL_CACHE_INVALIDATE : KL_CACHE_FLUSH;
	}
	writeq(ctrl, kl_cache_reg(KL_CACHE_CTRL));
	kl_cache_wait();
	local_irq_restore(flags);
}

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
		kl_dcache_walk(paddr, size, true, false);
		break;
	default:
		WARN_ON_ONCE(1);
	}
}

/*
 * After the device wrote the buffer, the CPU must not hit stale copies of
 * it: drop its lines.  A line only partly in the buffer (it should not be:
 * ARCH_DMA_MINALIGN) is cleaned as well as dropped, so that a neighbour's
 * data in it survives.
 *
 * The I-cache does not snoop DMA either: a page read from a device may be
 * mapped as code next, while the I-cache still holds the code of the page's
 * previous life.  So it is invalidated too (a 64-cycle sweep).
 */
void arch_sync_dma_for_cpu(phys_addr_t paddr, size_t size, enum dma_data_direction dir)
{
	phys_addr_t start = paddr, end = paddr + size;
	phys_addr_t line = L1_CACHE_BYTES;

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
	if (klausscpu_dcache_range_ops() && size <= KL_DMA_RANGE_MAX) {
		if (start & (line - 1)) {
			kl_dcache_walk(start & ~(line - 1), line, true, true);
			start = ALIGN(start, line);
		}
		if ((end & (line - 1)) && end > start) {
			kl_dcache_walk(end & ~(line - 1), line, true, true);
			end &= ~(line - 1);
		}
		if (end > start)
			kl_dcache_walk(start, end - start, false, true);
	} else {
		kl_dcache_walk(paddr, size, true, true);
	}
	kl_icache_invalidate();
}
