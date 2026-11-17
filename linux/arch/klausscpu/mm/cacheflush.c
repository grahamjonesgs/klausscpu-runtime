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
#include <linux/moduleparam.h>
#include <linux/processor.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <asm/cache.h>
#include <asm/cachectl.h>
#include <asm/klausscpu.h>
#include <asm/setup.h>
#include <asm/timex.h>

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
 * The crossovers between D-cache range walks and a whole-cache one, in
 * lines (counted from the buffer's first line to its last, so an
 * unaligned buffer counts its partial lines), measured on the board
 * (sddma 3ae5ed6, the bench below, cycles from an emptied cache):
 *
 *   lines            1024           2048           4096           8192
 *   clean, buffer    range  whole   range  whole   range  whole   range  whole
 *     read            2193   4247    2200   4229    4386   4348    8724   4374
 *     written         8341   9638   14489  10361   13656  10497   21071  10547
 *   invalidate,
 *     buffer read     2193   4329    2199   4246    4386   4354    8720   4337
 *   reading 32 KiB of other data after the invalidate:
 *                     2113  10169   11501  11006   17981  22341   26741  26850
 *
 * A range walk writes a dirty line back in ~6 cycles, a whole walk in ~3,
 * so for a written buffer the whole walk wins from about 1.5k lines; for a
 * clean one they meet at 4096.  A whole clean (FLUSH) leaves the rest of
 * the cache in place, so a clean takes range ops up to one op's 2048 lines
 * (64 KiB, a page) and the whole cache above.  A whole INVALIDATE also
 * drops every other line, which the CPU then fetches again (8,000 cycles
 * more to read 32 KiB back): an invalidate, whose buffer is clean (it was
 * cleaned before the DMA), takes range ops up to 8192 lines (256 KiB),
 * as up to four ops of 2048 (kl_range_op: the hardware would turn one
 * larger op into a whole walk; the range columns above are 1, 1, 2, 4 ops).
 * Either way the difference is tens of microseconds against a transfer of
 * milliseconds.
 */
#define KL_DCACHE_CLEAN_MAX_LINES	KL_RANGE_OP_LINES
#define KL_DCACHE_INV_MAX_LINES		(4 * KL_RANGE_OP_LINES)

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
 * KL_DCACHE_CLEAN_MAX_LINES lines for a clean, KL_DCACHE_INV_MAX_LINES
 * for an invalidate, else the whole cache (INVALIDATE writes back before
 * it drops, so it is a clean and invalidate).  False if one timed out.
 */
static bool kl_dcache_walk(phys_addr_t paddr, size_t size, bool clean, bool inv)
{
	u64 ctrl;

	if (!size)
		return true;
	if (klausscpu_dcache_range_ops() &&
	    kl_cache_lines(paddr, size) <= (inv ? KL_DCACHE_INV_MAX_LINES : KL_DCACHE_CLEAN_MAX_LINES)) {
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
 * lines) is invalidated (a clean one is past KL_DCACHE_CLEAN_MAX_LINES)
 * by range ops, two of them, and no whole-cache walk.  RANGE_INV writes
 * the buffer's dirty lines back first, so its contents are kept.
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
	kl_dcache_walk(virt_to_phys(buf) + 8, SZ_64K, false, true);
	pr_info("klausscpu: cache range ops: an unaligned 64 KiB invalidate took %lu range ops, %lu whole-cache walks\n",
		kl_range_ops - range, kl_whole_ops - whole);
	kfree(buf);
	return 0;
}
late_initcall(kl_cache_selftest);

/*
 * The range-vs-whole crossover, measured on demand: as root, write 1 to
 * /sys/module/klausscpu_cache/parameters/bench; the results go to the
 * kernel log.  For buffers of 1 to 16384 lines it times, in cycles (the
 * least of KL_BENCH_RUNS, interrupts off, each from a cache emptied by a
 * whole INVALIDATE), a clean and an invalidate of the buffer by range ops
 * (kl_range_op) and by one whole-cache walk, with the buffer read (clean
 * lines), written (dirty), and written after 32 KiB of other data was
 * (dirty, and the rest of the cache dirty too: what a whole walk also
 * writes back).  For the invalidates of that last case it also times
 * reading the other data again afterwards: what a whole INVALIDATE costs
 * the rest of the system.  Then RANGE_IINV against ICACHE_INV (the op
 * alone; a whole sweep's refetches come on top).
 */
#undef MODULE_PARAM_PREFIX
#define MODULE_PARAM_PREFIX "klausscpu_cache."
#define KL_BENCH_RUNS		8
#define KL_BENCH_ORDER		3	/* 512 KiB of 64 KiB pages: 16384 lines */
#define KL_BENCH_OTHER		SZ_32K

static void kl_bench_touch(void *p, size_t len, bool write)
{
	size_t i;

	for (i = 0; i < len; i += L1_CACHE_BYTES) {
		if (write)
			WRITE_ONCE(*(u64 *)(p + i), i);
		else
			(void)READ_ONCE(*(u64 *)(p + i));
	}
}

/* Cycles of one op on [buf, +len) from a fresh cache in state mode (0
 * read, 1 written, 2 other then buffer written); *reread the cycles to read
 * the other data afterwards. */
static unsigned long kl_bench_one(void *buf, size_t len, void *other, int mode, u64 ctrl,
				  bool range, unsigned long *reread)
{
	unsigned long flags, t0, t1, t2;

	kl_cache_op(KL_CACHE_INVALIDATE, 0, 0);
	if (mode == 2)
		kl_bench_touch(other, KL_BENCH_OTHER, true);
	kl_bench_touch(buf, len, mode != 0);
	local_irq_save(flags);
	t0 = get_cycles();
	if (range)
		kl_range_op(ctrl, virt_to_phys(buf), len);
	else
		kl_cache_op(ctrl, 0, 0);
	t1 = get_cycles();
	kl_bench_touch(other, KL_BENCH_OTHER, false);
	t2 = get_cycles();
	local_irq_restore(flags);
	*reread = t2 - t1;
	return t1 - t0;
}

static void kl_cache_bench(void)
{
	static const unsigned int lines[] = { 1, 4, 16, 64, 256, 1024, 2048, 3072, 4096,
					      6144, 8192, 16384 };
	/* clean (RANGE_CLEAN / FLUSH), invalidate (RANGE_INV / INVALIDATE) */
	static const u64 rctrl[2] = { KL_CACHE_RANGE_CLEAN, KL_CACHE_RANGE_INV };
	static const u64 wctrl[2] = { KL_CACHE_FLUSH, KL_CACHE_INVALIDATE };
	unsigned long t[2][2][3], rr[2], v, r, flags, t0, t1, best;
	struct page *pg;
	void *buf, *other;
	unsigned int i, n;
	int op, w, mode, k;

	pg = alloc_pages(GFP_KERNEL, KL_BENCH_ORDER);
	other = kmalloc(KL_BENCH_OTHER, GFP_KERNEL);
	if (!pg || !other)
		goto out;
	buf = page_address(pg);
	pr_info("klausscpu cache bench: cycles, least of %d; columns read / written / written+32K other dirty\n",
		KL_BENCH_RUNS);
	for (i = 0; i < ARRAY_SIZE(lines); i++) {
		size_t len = lines[i] * L1_CACHE_BYTES;

		for (op = 0; op < 2; op++)
			for (w = 0; w < 2; w++)
				for (mode = 0; mode < 3; mode++) {
					best = ULONG_MAX;
					for (k = 0; k < KL_BENCH_RUNS; k++) {
						v = kl_bench_one(buf, len, other, mode,
								 w ? wctrl[op] : rctrl[op], !w, &r);
						if (v < best) {
							best = v;
							if (op == 1 && mode == 2)
								rr[w] = r;
						}
					}
					t[op][w][mode] = best;
				}
		pr_info("klausscpu cache bench: %5u lines  clean range %5lu %5lu %5lu whole %5lu %5lu %5lu  inv range %5lu %5lu %5lu whole %5lu %5lu %5lu  reread %5lu / %5lu\n",
			lines[i], t[0][0][0], t[0][0][1], t[0][0][2], t[0][1][0], t[0][1][1], t[0][1][2],
			t[1][0][0], t[1][0][1], t[1][0][2], t[1][1][0], t[1][1][1], t[1][1][2], rr[0], rr[1]);
	}
	for (n = 1; n <= KL_ICACHE_LINES; n *= 4) {
		unsigned long whole = ULONG_MAX;

		best = ULONG_MAX;
		for (k = 0; k < KL_BENCH_RUNS; k++) {
			local_irq_save(flags);
			t0 = get_cycles();
			kl_cache_op(KL_CACHE_RANGE_IINV, virt_to_phys(buf), n * L1_CACHE_BYTES);
			t1 = get_cycles();
			best = min(best, t1 - t0);
			t0 = get_cycles();
			kl_icache_invalidate();
			t1 = get_cycles();
			whole = min(whole, t1 - t0);
			local_irq_restore(flags);
		}
		pr_info("klausscpu cache bench: I-cache %5u lines  RANGE_IINV %5lu  ICACHE_INV %5lu\n",
			n, best, whole);
	}
out:
	kfree(other);
	if (pg)
		__free_pages(pg, KL_BENCH_ORDER);
}

static int kl_bench_set(const char *val, const struct kernel_param *kp)
{
	if (!klausscpu_dcache_range_ops())
		return -ENODEV;
	kl_cache_bench();
	return 0;
}

static const struct kernel_param_ops kl_bench_ops = { .set = kl_bench_set };
module_param_cb(bench, &kl_bench_ops, NULL, 0200);
MODULE_PARM_DESC(bench, "Write 1: time range ops against whole-cache walks (kernel log)");

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

/*
 * cacheflush(addr, bytes, cache), syscall 244 (as on csky / nios2), for
 * user code that writes instructions through one mapping and runs them
 * through another: a JIT keeping W^X with two views of one page, libffi's
 * double-mapped closures, compiler-rt's __clear_cache.  The architecture
 * requires it (klaussCPU MMU_PLAN.md §11.3): the I-cache and BTIC snoop
 * this core's stores by physical address, but the front end's copies of
 * code already fetched (the fetch window, the next-line buffers) are
 * checked by virtual address, so code written through another mapping may
 * run stale once unless the I-cache is invalidated.  Today's core happens
 * to refetch it correctly in every shape tried (a store that posts
 * speculatively is checked against the I-cache by physical address;
 * linux/userland/tests/icache/aliasjit.c finds no stale case on the
 * board), but that is not promised.  Code written and run at the same
 * address needs nothing.
 *
 * ICACHE: the whole-cache ICACHE_INV (a 64-cycle sweep that also empties
 * the window, the next-line buffers and the BTIC), cheaper than turning
 * the range into physical pages for RANGE_IINV.  DCACHE: nothing to do;
 * the D-cache is coherent with this core and I-cache fills read through
 * it.  addr and bytes are only checked for wrapping; as on csky, cache
 * must name ICACHE, DCACHE or both.
 */
SYSCALL_DEFINE3(cacheflush, unsigned long, addr, unsigned long, bytes,
		int, cache)
{
	if (!cache || (cache & ~BCACHE))
		return -EINVAL;
	if (addr + bytes < addr)
		return -EFAULT;
	if (cache & ICACHE)
		kl_icache_invalidate();
	return 0;
}
