// SPDX-License-Identifier: GPL-2.0
/*
 * Cache maintenance through the cache controller (klaussCPU MMIO_MAP,
 * "Cache controller" and "Range operations"; HW_PERF_PLAN §7 for the
 * timings, measured on the RTL in simulation):
 *
 * - CACHE_CTRL FLUSH / INVALIDATE walk the whole D-cache (1024 sets x 2
 *   ways: 4,097 cycles + ~3 per dirty line).
 * - With CPU_FEATURES[27], RANGE_CLEAN / RANGE_INV walk the D-cache lines
 *   of [CACHE_OP_ADDR, +CACHE_OP_LEN) (ADDR's [4:0] read 0: ceil(LEN / 32)
 *   lines from ADDR & ~31) and write the dirty ones back; RANGE_INV then
 *   drops them (it never drops a dirty line unwritten).  A walk of n lines
 *   visits min(n, 1024) sets: 1 + 2 cycles a set, + 6 for a set holding a
 *   dirty line of the range (+ 3 for a second).  RANGE_IINV drops the same
 *   lines from the I-cache (and the whole BTIC, next-line buffer and fetch
 *   window), in min(n, 64) cycles.
 * - ADDR / LEN are copied when an op is accepted.  The queue holds one
 *   walk running and one waiting: a range op that finds another waiting, or
 *   a whole-cache walk waiting, or that covers more than 2048 lines
 *   (64 KiB), becomes a whole-cache walk (correct, but ~4,100 cycles).  So
 *   a range of more than 2048 lines goes as several ops of at most 2048
 *   (kl_range_op), each issued with the queue empty.
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
 * interleave its CACHE_OP_ADDR / LEN with ours, and no op of ours meets
 * another waiting (which would turn it into a whole-cache walk); the wait for the walk
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
#include <linux/init.h>
#include <linux/io.h>
#include <linux/irqflags.h>
#include <linux/mm.h>
#include <linux/processor.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <asm/cache.h>
#include <asm/klausscpu.h>
#include <asm/setup.h>

/*
 * The longest walk is ~11,300 cycles (113 us at 100 MHz: a range of 2048
 * dirty lines, 1 + 2048 + 9 x 1024; a whole D-cache with every line dirty
 * is ~10,250); give up after about nine times that.  It bounds the time a
 * caller with interrupts off spins here.
 */
#define KL_CACHE_WAIT_US	1000
/* MNT_BUSY polls before the wait starts counting microseconds (a range
 * walk of a few KiB is over by then). */
#define KL_CACHE_SPIN_POLLS	256

/* The most lines one range op covers without becoming a whole-cache walk
 * (the D-cache's 2048, 64 KiB; RTL RNG_MAX). */
#define KL_RANGE_OP_LINES	2048

/*
 * The crossover between D-cache range walks and a whole-cache one, in
 * lines (counted from the buffer's first line to its last, so an
 * unaligned buffer counts its partial lines).  A range walk of n lines
 * costs 1 + 2 x min(n, 1024) cycles clean, so up to 2048 lines (64 KiB)
 * one op takes at most 2049 and two ops up to 4096 lines (128 KiB) at most
 * 4098: the whole walk's 4,097.  Dirty lines cost a range walk ~6 each
 * (4.5 when both lines of a set are) and a whole walk 3, but a whole walk
 * also writes back every other dirty line, and a whole INVALIDATE drops
 * every other line too.  So up to 128 KiB a buffer's lines are walked,
 * above it the whole cache.  To be confirmed on the board.
 */
#define KL_DCACHE_RANGE_MAX_LINES	(2 * KL_RANGE_OP_LINES)

/*
 * The I-cache's lines (64 KiB of 32 B): a RANGE_IINV of this many or more
 * sweeps the whole I-cache anyway (the range's index wraps over all of
 * it), so it goes as ICACHE_INV, a 64-cycle sweep that needs no MNT_BUSY
 * wait.  Below it RANGE_IINV keeps the rest of the I-cache, in min(n, 64)
 * cycles.  A 64 KiB page or larger folio therefore takes ICACHE_INV.
 */
#define KL_ICACHE_LINES		2048

static bool kl_dma_wrote_memory __read_mostly;

/* The ops issued: range walks, whole-cache walks (the boot self-test). */
static unsigned long kl_range_ops, kl_whole_ops;

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
		kl_range_ops++;
	} else {
		kl_whole_ops++;
	}
	writeq(ctrl, kl_cache_reg(KL_CACHE_CTRL));
	local_irq_restore(flags);
	return kl_cache_wait() || kl_cache_timeout("after");
}

/* The cache lines [paddr, +size) touches (size > 0). */
static unsigned long kl_cache_lines(phys_addr_t paddr, size_t size)
{
	return ((paddr + size - 1) >> L1_CACHE_SHIFT) - (paddr >> L1_CACHE_SHIFT) + 1;
}

/*
 * A range op over [paddr, +size) as ops of at most KL_RANGE_OP_LINES lines
 * each, split at line boundaries, so that none becomes a whole-cache walk
 * (an unaligned 64 KiB buffer touches 2049 lines).  False if a walk timed
 * out.
 */
static bool kl_range_op(u64 ctrl, phys_addr_t paddr, size_t size)
{
	phys_addr_t end = paddr + size;

	while (paddr < end) {
		phys_addr_t next = (paddr & ~(phys_addr_t)(L1_CACHE_BYTES - 1)) +
				   KL_RANGE_OP_LINES * L1_CACHE_BYTES;
		size_t n = min(end, next) - paddr;

		if (!kl_cache_op(ctrl, paddr, n))
			return false;
		paddr += n;
	}
	return true;
}

/*
 * The D-cache walks over [paddr, +size): with the range ops up to
 * KL_DCACHE_RANGE_MAX_LINES lines, else the whole cache (INVALIDATE writes
 * back before it drops, so it is a clean and invalidate).  False if one
 * timed out.
 */
static bool kl_dcache_walk(phys_addr_t paddr, size_t size, bool clean, bool inv)
{
	u64 ctrl;

	if (!size)
		return true;
	if (klausscpu_dcache_range_ops() && kl_cache_lines(paddr, size) <= KL_DCACHE_RANGE_MAX_LINES) {
		ctrl = (clean ? KL_CACHE_RANGE_CLEAN : 0) | (inv ? KL_CACHE_RANGE_INV : 0);
		return kl_range_op(ctrl, paddr, size);
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
	if (klausscpu_dcache_range_ops() && size && kl_cache_lines(paddr, size) < KL_ICACHE_LINES &&
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
 * After a device wrote [paddr, +size): invalidate its D-cache lines
 * (RANGE_INV, which writes a dirty line back before it drops it; nothing
 * of the buffer should be dirty: it was cleaned before the DMA and the CPU
 * kept off it during it, else that writeback would land over the device's
 * data), and see to the I-cache.
 *
 * This is only right for lines the buffer owns whole.  A line shared with
 * other data that the CPU wrote during the DMA would clobber the device's
 * bytes when written back (and lose the CPU's write if it were dropped):
 * neither way is right, and only the buffer's alignment (ARCH_DMA_MINALIGN
 * for kmalloc; the SD driver takes a read by DMA only when it owns whole
 * lines) keeps it from happening.  So an unaligned buffer is a bug: warn,
 * clean and invalidate its edge lines (right at least when the CPU did not
 * touch them during the DMA), and invalidate the whole lines between them.
 * (Since RANGE_INV writes back too, one walk over the whole span would do
 * the same; the separate edge walks date from reading it as a drop.)
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
 * Boot self-test, with the range ops: an unaligned 64 KiB buffer (2049
 * lines) is cleaned by range ops, two of them, and no whole-cache walk.
 */
static int __init kl_cache_selftest(void)
{
	unsigned long range, whole;
	void *buf;

	if (!klausscpu_dcache_range_ops())
		return 0;
	buf = kmalloc(SZ_64K + 2 * L1_CACHE_BYTES, GFP_KERNEL);
	if (!buf)
		return 0;
	range = kl_range_ops;
	whole = kl_whole_ops;
	klausscpu_dma_clean(virt_to_phys(buf) + 8, SZ_64K);
	pr_info("klausscpu: cache range ops: an unaligned 64 KiB clean took %lu range ops, %lu whole-cache walks\n",
		kl_range_ops - range, kl_whole_ops - whole);
	kfree(buf);
	return 0;
}
late_initcall(kl_cache_selftest);

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
