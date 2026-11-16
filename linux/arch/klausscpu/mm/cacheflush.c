// SPDX-License-Identifier: GPL-2.0
/*
 * Cache maintenance through the cache controller (klaussCPU MMIO_MAP,
 * "Cache controller"; the SD DMA spec v1.2 for the range ops):
 *
 * - CACHE_CTRL FLUSH / INVALIDATE walk the whole D-cache (1024 sets x 2
 *   ways: ~2048 cycles + ~10 per dirty line).
 * - With CPU_FEATURES[27], RANGE_CLEAN / RANGE_INV walk the D-cache lines
 *   of [CACHE_OP_ADDR, +CACHE_OP_LEN) (ADDR's [4:0] read 0: ceil(LEN / 32)
 *   lines from ADDR & ~31; ~8 + 2 per line + ~10 per dirty line), and
 *   RANGE_IINV drops the same lines from the I-cache (and its BTIC, next-
 *   line buffer and fetch window).  ADDR / LEN are copied when an op is
 *   accepted, so one written while CACHE_STATUS.MNT_BUSY queues behind the
 *   walk in flight with its own values.
 * - ICACHE_INV drops the whole I-cache (a 64-cycle sweep).
 *
 * A walk is synchronous for the CPU's next cached access, not for MMIO:
 * so every op here waits for MNT_BUSY to drop before it returns, and an
 * MMIO write after it (an SDM_CMD that starts a DMA) comes after the
 * walk's writebacks are in DDR.  Ordering needs no barrier beyond
 * writeq's: a CACHE_CTRL write takes effect only after every earlier
 * store in program order has reached the D-cache, and the core's memory
 * requests, MMIO included, go through one in-order port (spec v1.2 m3).
 *
 * The register writes of one op are made with interrupts off, after
 * MNT_BUSY was seen clear, so that an op from an interrupt handler can't
 * interleave its CACHE_OP_ADDR / LEN with ours (a queued op keeps its
 * values, but the queue's depth is not specified); the wait for the walk
 * runs with interrupts as the caller had them, and gives up after
 * KL_CACHE_WAIT_US, about four times the slowest walk.
 *
 * The I-cache snoops CPU stores but not DMA.  So a page whose contents
 * came from a device must be invalidated in the I-cache before it is
 * executed.  As arm64 does with PG_dcache_clean, that is done lazily:
 * the flag (PG_arch_1) on a folio says the I-cache holds nothing stale
 * of it; it is clear on a newly allocated folio, cleared when the kernel
 * writes one (flush_dcache_folio), and set by __sync_icache_dcache when
 * set_ptes() first maps it executable into user space, after an I-cache
 * invalidate of the folio.  A DMA into a folio that has the flag (it may
 * be mapped executable now) invalidates at once (mm/dma-noncoherent.c).
 * Until the first DMA from a device since boot, nothing has come from a
 * device and the invalidate is skipped (the flag is still kept).
 */
#include <linux/bug.h>
#include <linux/cacheflush.h>
#include <linux/delay.h>
#include <linux/export.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/mm.h>
#include <linux/processor.h>
#include <asm/cache.h>
#include <asm/klausscpu.h>
#include <asm/setup.h>

/*
 * The longest legal walk is ~25000 cycles (250 us at 100 MHz: a whole
 * D-cache with every line dirty, or a 64 KiB range of dirty lines); give
 * up after four times that.  It bounds the time a caller with interrupts
 * off spins here.
 */
#define KL_CACHE_WAIT_US	1000
/* MNT_BUSY polls before the wait starts counting microseconds (a range
 * walk of a few KiB is over by then). */
#define KL_CACHE_SPIN_POLLS	256

/*
 * The crossover between a D-cache range walk and a whole-cache one: up to
 * this many bytes a buffer's lines are walked (~8 + 2 cycles per line), above
 * it the whole cache (~2048 cycles: 1024 sets x 2 ways), which a range walk
 * of 64 KiB (2048 lines, ~4100 cycles) has passed.  A whole INVALIDATE also
 * drops every other line, so the true crossover may be higher.  To be
 * measured on the board (CLANG_ON_BOARD_PLAN.md, SD DMA review n2).
 */
#define KL_DCACHE_RANGE_MAX	(64 * 1024)

/*
 * Above this an I-cache range invalidate is not worth its walk: ~8 + 2
 * cycles per line, 4100 cycles for a 64 KiB page, against ICACHE_INV's
 * 64-cycle sweep and the refetch of what the 16 KiB I-cache held.  One
 * page goes by range, a larger folio by the whole sweep.  To be measured
 * on the board (CLANG_ON_BOARD_PLAN.md n2).
 */
#define KL_IINV_RANGE_MAX	PAGE_SIZE

static unsigned long kl_cache_timeouts;
static bool kl_dma_wrote_memory __read_mostly;

static void __iomem *kl_cache_reg(unsigned int off)
{
	return (void __iomem *)(KL_CACHE_BASE + off);
}

bool klausscpu_dcache_range_ops(void)
{
	return klausscpu_cpu_features & CPU_FEAT_DCACHE_RANGE;
}

unsigned long klausscpu_cache_timeouts(void)
{
	return READ_ONCE(kl_cache_timeouts);
}

static bool kl_cache_busy(void)
{
	return readq(kl_cache_reg(KL_CACHE_STATUS)) & KL_CACHE_MNT_BUSY;
}

/* Wait for MNT_BUSY to drop; false if it did not within KL_CACHE_WAIT_US. */
static bool kl_cache_wait(void)
{
	unsigned int n;

	for (n = 0; n < KL_CACHE_SPIN_POLLS; n++) {
		if (!kl_cache_busy())
			return true;
		cpu_relax();
	}
	for (n = 0; n < KL_CACHE_WAIT_US; n++) {
		udelay(1);
		if (!kl_cache_busy())
			return true;
	}
	return false;
}

static bool kl_cache_timeout(const char *what)
{
	kl_cache_timeouts++;
	WARN_ONCE(1, "klausscpu: cache walk (%s) did not finish in %u us\n", what, KL_CACHE_WAIT_US);
	return false;
}

/*
 * One op: CACHE_CTRL = ctrl, with CACHE_OP_ADDR / LEN = [paddr, +size)
 * for a range op; then wait for its walk.  False if a walk did not end.
 */
static bool kl_cache_op(u64 ctrl, phys_addr_t paddr, size_t size)
{
	unsigned long flags;
	phys_addr_t start = paddr & ~(phys_addr_t)(L1_CACHE_BYTES - 1);

	for (;;) {
		if (!kl_cache_wait())
			return kl_cache_timeout("before");
		local_irq_save(flags);
		if (!kl_cache_busy())
			break;
		local_irq_restore(flags);	/* an interrupt handler queued one */
	}
	if (ctrl & (KL_CACHE_RANGE_CLEAN | KL_CACHE_RANGE_INV | KL_CACHE_RANGE_IINV)) {
		/* The walk counts LEN from ADDR & ~31: cover the partial first line. */
		writeq(start, kl_cache_reg(KL_CACHE_OP_ADDR));
		writeq(size + (paddr - start), kl_cache_reg(KL_CACHE_OP_LEN));
	}
	writeq(ctrl, kl_cache_reg(KL_CACHE_CTRL));
	local_irq_restore(flags);
	return kl_cache_wait() || kl_cache_timeout("after");
}

bool klausscpu_dcache_walk(phys_addr_t paddr, size_t size, bool clean, bool inv)
{
	u64 ctrl;

	if (!size)
		return true;
	if (klausscpu_dcache_range_ops() && size <= KL_DCACHE_RANGE_MAX) {
		ctrl = (clean ? KL_CACHE_RANGE_CLEAN : 0) | (inv ? KL_CACHE_RANGE_INV : 0);
		return kl_cache_op(ctrl, paddr, size);
	}
	/* INVALIDATE writes back before it drops: a clean and invalidate. */
	return kl_cache_op(inv ? KL_CACHE_INVALIDATE : KL_CACHE_FLUSH, 0, 0);
}

static void klausscpu_icache_inv_phys(phys_addr_t paddr, size_t size)
{
	if (klausscpu_dcache_range_ops() && size <= KL_IINV_RANGE_MAX)
		kl_cache_op(KL_CACHE_RANGE_IINV, paddr, size);
	else
		kl_icache_invalidate();
}

/*
 * A device wrote [paddr, +size) (arch_sync_dma_for_cpu): a folio there that
 * may be mapped executable (PG_dcache_clean) has its I-cache lines dropped
 * now; the others are when they are next mapped executable.
 */
void klausscpu_icache_dma_written(phys_addr_t paddr, size_t size)
{
	unsigned long pfn = PHYS_PFN(paddr), last = PHYS_PFN(paddr + size - 1);

	if (!size)
		return;
	WRITE_ONCE(kl_dma_wrote_memory, true);
	for (; pfn <= last; pfn++) {
		if (!pfn_valid(pfn) || test_bit(PG_dcache_clean, &page_folio(pfn_to_page(pfn))->flags.f)) {
			klausscpu_icache_inv_phys(paddr, size);
			return;
		}
	}
}

/* set_ptes() maps the folio of pte executable into user space. */
void __sync_icache_dcache(pte_t pte)
{
	unsigned long pfn = pte_pfn(pte);
	struct folio *folio;

	if (!pfn_valid(pfn)) {
		kl_icache_invalidate();
		return;
	}
	folio = page_folio(pfn_to_page(pfn));
	if (test_bit(PG_dcache_clean, &folio->flags.f))
		return;
	if (READ_ONCE(kl_dma_wrote_memory))
		klausscpu_icache_inv_phys(PFN_PHYS(folio_pfn(folio)), folio_size(folio));
	set_bit(PG_dcache_clean, &folio->flags.f);
}

/* The kernel wrote the folio: its next executable mapping invalidates. */
void flush_dcache_folio(struct folio *folio)
{
	if (test_bit(PG_dcache_clean, &folio->flags.f))
		clear_bit(PG_dcache_clean, &folio->flags.f);
}
EXPORT_SYMBOL(flush_dcache_folio);

void flush_dcache_page(struct page *page)
{
	flush_dcache_folio(page_folio(page));
}
EXPORT_SYMBOL(flush_dcache_page);
