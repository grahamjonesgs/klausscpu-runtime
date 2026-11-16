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
 * The I-cache snoops CPU stores (by physical tag, so through any mapping)
 * but not DMA.  So a page whose contents came from a device must be
 * invalidated in the I-cache before it is executed.  As arm64 does with
 * PG_dcache_clean, that is done lazily, but the flag (PG_arch_1) means
 * something stronger here: "this folio may have been mapped executable
 * since it was allocated".  It is clear on a newly allocated folio (the
 * page allocator clears PG_arch_1 with PAGE_FLAGS_CHECK_AT_PREP when the
 * folio is freed) and set by __sync_icache_dcache when set_ptes() first
 * maps the folio executable into user space, after an I-cache invalidate
 * of it.  Nothing else clears it: in particular not flush_dcache_folio(),
 * which on arm64 clears it after the kernel writes a folio.  Here a CPU
 * write never makes the I-cache stale, and clearing the flag there would
 * lose the one fact the DMA side needs: GUP calls flush_dcache_page() on
 * every page it pins (mm/gup.c, without HAVE_GUP_FAST), so an O_DIRECT
 * read into an executable mapping that has already run would find the
 * flag clear, skip the invalidate, and the still-mapped page (set_ptes()
 * never runs for it again) would execute stale code.  So:
 *
 * - a DMA into a folio with the flag (it may be mapped executable, now or
 *   by a mapping set up since) invalidates its I-cache lines at once;
 * - a DMA into a folio without it (never mapped executable: the usual
 *   page-cache read) leaves that to the folio's first executable mapping.
 *
 * Until the first DMA from a device since boot, nothing has come from a
 * device and the invalidate at mapping time is skipped (the flag is still
 * set).
 */
#include <linux/bug.h>
#include <linux/cacheflush.h>
#include <linux/delay.h>
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
 * Up to this an I-cache invalidate is a RANGE_IINV of the lines, above it
 * the whole ICACHE_INV (a 64-cycle sweep, but everything the I-cache held
 * is fetched again).  One page goes by range, a larger folio by the sweep.
 * The RTL session aims at >= 1 line per cycle for the range kill and will
 * report the real figure; the crossover is to be measured on the board.
 */
#define KL_IINV_RANGE_MAX	PAGE_SIZE

static bool kl_dma_wrote_memory __read_mostly;

static void __iomem *kl_cache_reg(unsigned int off)
{
	return (void __iomem *)(KL_CACHE_BASE + off);
}

bool klausscpu_dcache_range_ops(void)
{
	return klausscpu_cpu_features & CPU_FEAT_DCACHE_RANGE;
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

/*
 * One D-cache walk over [paddr, +size): with the range ops up to
 * KL_DCACHE_RANGE_MAX, else the whole cache (INVALIDATE writes back before
 * it drops, so it is a clean and invalidate).  False if it timed out.
 */
static bool kl_dcache_walk(phys_addr_t paddr, size_t size, bool clean, bool inv)
{
	u64 ctrl;

	if (!size)
		return true;
	if (klausscpu_dcache_range_ops() && size <= KL_DCACHE_RANGE_MAX) {
		ctrl = (clean ? KL_CACHE_RANGE_CLEAN : 0) | (inv ? KL_CACHE_RANGE_INV : 0);
		return kl_cache_op(ctrl, paddr, size);
	}
	return kl_cache_op(inv ? KL_CACHE_INVALIDATE : KL_CACHE_FLUSH, 0, 0);
}

/*
 * Drop [paddr, +size) from the I-cache.  ICACHE_INV is a sweep, not a
 * walk, so it can't time out: it is also the fallback when a RANGE_IINV
 * walk does, since that may have invalidated nothing.
 */
static void kl_icache_inv(phys_addr_t paddr, size_t size)
{
	if (klausscpu_dcache_range_ops() && size <= KL_IINV_RANGE_MAX &&
	    kl_cache_op(KL_CACHE_RANGE_IINV, paddr, size))
		return;
	kl_icache_invalidate();
}

/*
 * Before a device reads or writes [paddr, +size): its dirty lines go to
 * DDR (for a device write too, so that none is written back over the
 * device's data while it is in flight).  False if the walk timed out: the
 * device may not see the CPU's data.
 */
bool klausscpu_dma_clean(phys_addr_t paddr, size_t size)
{
	return kl_dcache_walk(paddr, size, true, false);
}

/*
 * After a device wrote [paddr, +size): drop its D-cache lines, without a
 * writeback (nothing of the buffer is dirty: it was cleaned before the DMA
 * and the CPU kept off it during it), and see to the I-cache.
 *
 * Dropping is only safe for lines the buffer owns whole.  A line shared
 * with other data that the CPU wrote during the DMA would lose that write
 * if dropped, and clobber the device's bytes if written back: neither way
 * is right, and only the buffer's alignment (ARCH_DMA_MINALIGN for
 * kmalloc; the SD driver takes a read by DMA only when it owns whole
 * lines) keeps it from happening.  So an unaligned buffer is a bug: warn,
 * clean and invalidate its edge lines (right at least when the CPU did not
 * touch them during the DMA), and drop only the whole lines between them,
 * so that a neighbour's store made after the edge clean is not dropped.
 *
 * The I-cache: a folio that may be mapped executable (PG_dcache_clean,
 * see above) has the buffer's I-cache lines dropped now; the others are
 * dropped when they are first mapped executable.
 *
 * False if the D-cache walk timed out: the CPU may see stale data.  (An
 * I-cache walk that times out falls back to ICACHE_INV.)
 */
bool klausscpu_dma_inval(phys_addr_t paddr, size_t size)
{
	phys_addr_t mask = L1_CACHE_BYTES - 1, start = paddr, end = paddr + size;
	unsigned long pfn, last;
	bool ok = true;

	if (!size)
		return true;
	if (WARN_ON_ONCE((paddr | size) & mask)) {
		ok = kl_dcache_walk(start & ~mask, L1_CACHE_BYTES, true, true);
		if ((end - 1) >> L1_CACHE_SHIFT != start >> L1_CACHE_SHIFT)
			ok &= kl_dcache_walk((end - 1) & ~mask, L1_CACHE_BYTES, true, true);
		start = ALIGN(start, L1_CACHE_BYTES);
		end &= ~mask;
	}
	if (end > start)
		ok &= kl_dcache_walk(start, end - start, false, true);

	WRITE_ONCE(kl_dma_wrote_memory, true);
	last = PHYS_PFN(paddr + size - 1);
	for (pfn = PHYS_PFN(paddr); pfn <= last; pfn++) {
		if (!pfn_valid(pfn) || test_bit(PG_dcache_clean, &page_folio(pfn_to_page(pfn))->flags.f)) {
			kl_icache_inv(paddr, size);
			break;
		}
	}
	return ok;
}

/*
 * A DMA engine the kernel did not start (one a loader left running) has
 * been writing memory: drop the whole I-cache and have every later
 * executable mapping check for stale lines.
 */
void klausscpu_dma_stray_write(void)
{
	WRITE_ONCE(kl_dma_wrote_memory, true);
	kl_icache_invalidate();
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
		kl_icache_inv(PFN_PHYS(folio_pfn(folio)), folio_size(folio));
	set_bit(PG_dcache_clean, &folio->flags.f);
}
