/*
 * display_vnc.c — exposes the in-RAM VNC framebuffer as a Zephyr display.
 *
 * A minimal display driver (compatible "klausscpu,vnc-display") whose write()
 * copies flushed RGB565 rectangles into the VNC framebuffer and marks them
 * dirty.  With it set as chosen zephyr,display, display-based stacks (LVGL)
 * render straight over VNC.  Pair with a matching devicetree node.
 */

#define DT_DRV_COMPAT klausscpu_vnc_display

#include <zephyr/drivers/display.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <string.h>
#include <errno.h>

#include "framebuffer.h"
#ifdef CONFIG_KLAUSSCPU_VNC_BLITTER
#include "blitter.h"
#endif

/* Free-running 100 MHz CPU cycle counter (perf block 0xF00D_0008) — reliable
 * and non-destructive (unlike k_cycle_get_32 on this core); used to time the
 * flush copy at sub-ms precision now the blitter can make it near-free. */
#define VNCD_PERF_CYCLES (*(volatile uint64_t *)(unsigned long)0xF00D0008u)

/* Accumulated CPU cycles spent moving flushed pixels into the framebuffer.
 * Lets a caller (the LVGL benchmark) split total frame time into render vs this
 * flush copy — the part the blitter offloads. */
static uint64_t vncd_copy_cyc_acc;

/* Of the copy time, the cycles the blitter engine itself reported busy (its own
 * BLIT_CYCLES) — so copy minus this is the whole-cache FLUSH/INVALIDATE cost. */
static uint64_t vncd_blit_cyc_acc;

/* Whether a 2D DMA blitter is present in this bitstream (probed at init).  When
 * false (or built without CONFIG_KLAUSSCPU_VNC_BLITTER) the flush falls back to
 * a CPU memcpy, so the same image runs on a pre-blitter bitstream. */
static bool vncd_blit;

#ifdef CONFIG_KLAUSSCPU_VNC_BLITTER
/*
 * Asynchronous flush.  vncd_write() STARTS the blit and returns immediately;
 * the DMA then runs concurrently with LVGL rendering the next frame into its
 * other draw buffer (LVGL 8.3's full-size double-buffer mode waits for
 * display_write to return before rendering — an early return is what unlocks
 * the render/copy overlap; the buffers alternate, so the source of an
 * in-flight blit is never the buffer being rendered into).
 *
 * The in-flight blit is retired by vncd_blit_fence(), called from:
 *   - the next vncd_write (steady state: before starting the next blit),
 *   - a delayed work item (idle tail: the LAST flush of a burst still gets
 *     its INVALIDATE + fb_mark_dirty when no further flush arrives),
 *   - the cycle-counter reset functions (so the LVGL benchmark reads settled
 *     numbers) and the self-test entry.
 *
 * Cache maintenance halves versus the sync bracket: ONE whole-cache
 * INVALIDATE per flush, issued between the previous blit's completion and
 * the next blit's START.  A single walk does both jobs — it writes back every
 * dirty line (pushing the freshly-RENDERED source pixels to DDR for the blit
 * about to start; INVALIDATE flushes dirty lines before dropping) and drops
 * the CPU's stale cached copies of the previous blit's destination.
 * fb_mark_dirty (the VNC update trigger) fires only after that walk, so the
 * VNC sender never reads a completed rect through a stale cache.
 *
 * Semantic shift vs the sync path: fb_lock is NOT held while the DMA runs
 * (k_mutex is owner-release-only, and holding it would re-serialize nothing
 * but the VNC sender).  A VNC copy-out can therefore overlap an active blit
 * and transiently see a half-written rect — cosmetic, self-healing on the
 * next dirty update, and the affected rect is not even marked dirty yet.
 */
static struct {
	bool     in_flight;
	uint16_t x, y, w, h;      /* rect of the in-flight blit (for mark_dirty) */
	uint64_t c0;              /* cycle stamp at START (diagnostics) */
} vncd_async;
static K_MUTEX_DEFINE(vncd_async_lock);

static void vncd_fence_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(vncd_fence_work, vncd_fence_work_handler);

/* Retire the in-flight blit (if any): wait DONE, W1C, account the engine
 * cycles, run the single INVALIDATE walk, and only then mark the rect dirty.
 * force_inval runs the walk even with nothing in flight (vncd_write uses this
 * so the FIRST blit of a burst still gets its source pushed to DDR). */
static void vncd_blit_fence(bool force_inval)
{
	k_mutex_lock(&vncd_async_lock, K_FOREVER);
	if (!vncd_async.in_flight && !force_inval) {
		k_mutex_unlock(&vncd_async_lock);
		return;
	}
	if (vncd_async.in_flight) {
		while (blit_busy()) {
#ifdef CONFIG_KLAUSSCPU_VNC_BLIT_SLEEP
			k_sleep(K_TICKS(1));
#endif
		}
		vncd_blit_cyc_acc += blit_last_cycles();
		BLIT_R64(BLIT_STATUS) = BLIT_STATUS_DONE;   /* W1C */
	}
	CACHE_CTRL = CACHE_CTRL_INVAL;   /* one walk: push rendered src, drop stale dst */
	if (vncd_async.in_flight) {
		vncd_async.in_flight = false;
		int mx = vncd_async.x, my = vncd_async.y;
		int mw = vncd_async.w, mh = vncd_async.h;

		k_mutex_unlock(&vncd_async_lock);
		fb_mark_dirty(mx, my, mw, mh);   /* takes fb_lock itself */
		return;
	}
	k_mutex_unlock(&vncd_async_lock);
}

static void vncd_fence_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	vncd_blit_fence(false);
}
#endif /* CONFIG_KLAUSSCPU_VNC_BLITTER */

uint64_t vncd_copy_cyc_reset(void)
{
	uint64_t v;

#ifdef CONFIG_KLAUSSCPU_VNC_BLITTER
	vncd_blit_fence(false);   /* settle so the caller reads final numbers */
#endif
	v = vncd_copy_cyc_acc;
	vncd_copy_cyc_acc = 0;
	return v;
}

uint64_t vncd_blit_cyc_reset(void)
{
	uint64_t v;

#ifdef CONFIG_KLAUSSCPU_VNC_BLITTER
	vncd_blit_fence(false);
#endif
	v = vncd_blit_cyc_acc;
	vncd_blit_cyc_acc = 0;
	return v;
}

bool vncd_blit_active(void)
{
	return vncd_blit;
}

/*
 * Standalone blitter self-test — no LVGL, no VNC.  Blits known patterns between
 * two cached DDR buffers (same row width/stride as the real framebuffer) and the
 * CPU reads them back, so it isolates the blitter hardware from everything else:
 *
 *   FILL  : write-only path  (no DDR read).
 *   COPY  : read + write path.  Source pixel encodes its column in the low byte
 *           and row in the high byte, so any horizontal misplacement is obvious
 *           in the "want vs got" of the first mismatches.
 *
 * FILL-clean + COPY-corrupt  => the read-data capture is the fault.
 * Both corrupt                => the write path (or cache flush/invalidate).
 * Both clean                  => the blitter is fine; look elsewhere.
 */
#ifdef CONFIG_KLAUSSCPU_VNC_BLITTER
#define ST_W      640                 /* same row width/stride as the framebuffer */
#define ST_H      32
#define ST_STRIDE (ST_W * (int)sizeof(uint16_t))
#define ST_POISON 0xDEADu

static uint16_t st_src[ST_W * ST_H];
static uint16_t st_dst[ST_W * ST_H];

static inline uint16_t st_pat(unsigned x, unsigned y)
{
	return (uint16_t)((x & 0xFFu) | ((y & 0xFFu) << 8));
}

void vncd_blit_selftest(void)
{
	if (!vncd_blit) {
		printk("blit self-test: no blitter present (memcpy path)\n");
		return;
	}
	vncd_blit_fence(false);   /* never overlap an async flush blit */
	printk("=== blit self-test (%dx%d, stride %d B, aligned) ===\n",
	       ST_W, ST_H, ST_STRIDE);

	/* ── FILL: write-only ──────────────────────────────────────────────── */
	for (int i = 0; i < ST_W * ST_H; i++) {
		st_dst[i] = ST_POISON;
	}
	blit_fill_rect((uint32_t)(uintptr_t)st_dst, ST_STRIDE, ST_W, ST_H, 0x07E0);

	int fbad = 0, ffx = 0, ffy = 0;
	uint16_t fgot = 0;

	for (int y = 0; y < ST_H; y++) {
		for (int x = 0; x < ST_W; x++) {
			uint16_t v = st_dst[y * ST_W + x];

			if (v != 0x07E0) {
				if (!fbad) { ffx = x; ffy = y; fgot = v; }
				fbad++;
			}
		}
	}
	if (fbad == 0) {
		printk("FILL : PASS\n");
	} else {
		printk("FILL : FAIL %d/%d bad; first (%d,%d) want=07e0 got=%04x\n",
		       fbad, ST_W * ST_H, ffx, ffy, fgot);
	}

	/* ── COPY: read + write ────────────────────────────────────────────── */
	for (int y = 0; y < ST_H; y++) {
		for (int x = 0; x < ST_W; x++) {
			st_src[y * ST_W + x] = st_pat(x, y);
		}
	}
	for (int i = 0; i < ST_W * ST_H; i++) {
		st_dst[i] = ST_POISON;
	}
	blit_copy_rect((uint32_t)(uintptr_t)st_dst, ST_STRIDE,
		       (uint32_t)(uintptr_t)st_src, ST_STRIDE, ST_W, ST_H);

	int cbad = 0, shown = 0;

	for (int y = 0; y < ST_H; y++) {
		for (int x = 0; x < ST_W; x++) {
			uint16_t want = st_pat(x, y);    /* from the formula, not st_src */
			uint16_t got  = st_dst[y * ST_W + x];

			if (want != got) {
				cbad++;
				if (shown < 12) {
					printk("  (%3d,%2d) want=%04x got=%04x\n",
					       x, y, want, got);
					shown++;
				}
			}
		}
	}
	if (cbad == 0) {
		printk("COPY : PASS\n");
	} else {
		printk("COPY : FAIL %d/%d bad\n", cbad, ST_W * ST_H);
	}
	printk("=== blit self-test done ===\n");
}
#else
void vncd_blit_selftest(void)
{
	printk("blit self-test: built without CONFIG_KLAUSSCPU_VNC_BLITTER\n");
}
#endif

static int vncd_write(const struct device *dev, const uint16_t x,
		      const uint16_t y,
		      const struct display_buffer_descriptor *desc,
		      const void *buf)
{
	ARG_UNUSED(dev);
	const uint16_t *src = buf;

	if ((uint32_t)x + desc->width > FB_WIDTH ||
	    (uint32_t)y + desc->height > FB_HEIGHT) {
		return -EINVAL;
	}

	uint64_t c0 = VNCD_PERF_CYCLES;

#ifdef CONFIG_KLAUSSCPU_VNC_BLITTER
	if (vncd_blit) {
		/* ASYNC flush: retire the previous blit and run the single
		 * INVALIDATE walk — which also pushes THIS frame's rendered
		 * pixels (already drawn, flush_cb runs after rendering) down
		 * to DDR for the blit about to start. */
		vncd_blit_fence(true);

		k_mutex_lock(&vncd_async_lock, K_FOREVER);
		fb_lock();
		uint16_t *dst = fb_pixels() + (size_t)y * FB_WIDTH + x;

		/* Program + START only — no cache op, no completion wait.
		 * (blit_start_copy() would issue a second, redundant walk.) */
		BLIT_R64(BLIT_DST_ADDR)   = (uint32_t)(uintptr_t)dst;
		BLIT_R64(BLIT_DST_STRIDE) = FB_WIDTH * sizeof(uint16_t);
		BLIT_R64(BLIT_SRC_ADDR)   = (uint32_t)(uintptr_t)src;
		BLIT_R64(BLIT_SRC_STRIDE) = (uint32_t)desc->pitch * sizeof(uint16_t);
		BLIT_R64(BLIT_WIDTH)      = desc->width;
		BLIT_R64(BLIT_HEIGHT)     = desc->height;
		BLIT_R64(BLIT_CTRL)       = BLIT_CTRL_START | (BLIT_OP_COPY << 1);

		vncd_async.in_flight = true;
		vncd_async.x  = x;
		vncd_async.y  = y;
		vncd_async.w  = desc->width;
		vncd_async.h  = desc->height;
		vncd_async.c0 = c0;
		fb_unlock();
		k_mutex_unlock(&vncd_async_lock);

		/* Idle-tail fence: if no further flush arrives to retire this
		 * blit (last frame of a burst), the system workqueue does it —
		 * scheduled past the worst-case blit so it never has to wait. */
		k_work_reschedule(&vncd_fence_work, K_MSEC(40));

		vncd_copy_cyc_acc += VNCD_PERF_CYCLES - c0;
		/* fb_mark_dirty is DEFERRED to the fence: the rect only becomes
		 * VNC-visible once the DMA finished and the walk dropped the
		 * CPU's stale cached copies of it. */
		return 0;
	}
#endif
	fb_lock();
	uint16_t *dst = fb_pixels() + (size_t)y * FB_WIDTH + x;

	for (uint16_t row = 0; row < desc->height; row++) {
		memcpy(dst + (size_t)row * FB_WIDTH,
		       src + (size_t)row * desc->pitch,
		       (size_t)desc->width * sizeof(uint16_t));
	}
	fb_unlock();
	vncd_copy_cyc_acc += VNCD_PERF_CYCLES - c0;

	fb_mark_dirty(x, y, desc->width, desc->height);
	return 0;
}

static void vncd_get_capabilities(const struct device *dev,
				  struct display_capabilities *cap)
{
	ARG_UNUSED(dev);
	memset(cap, 0, sizeof(*cap));
	cap->x_resolution = FB_WIDTH;
	cap->y_resolution = FB_HEIGHT;
	cap->supported_pixel_formats = PIXEL_FORMAT_RGB_565;
	cap->current_pixel_format = PIXEL_FORMAT_RGB_565;
}

static int vncd_set_pixel_format(const struct device *dev,
				 const enum display_pixel_format pf)
{
	ARG_UNUSED(dev);
	return (pf == PIXEL_FORMAT_RGB_565) ? 0 : -ENOTSUP;
}

static int vncd_blanking_off(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static int vncd_blanking_on(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

static int vncd_init(const struct device *dev)
{
	ARG_UNUSED(dev);
#ifdef CONFIG_KLAUSSCPU_VNC_BLITTER
	vncd_blit = blit_probe();
	if (vncd_blit) {
		/* Async flush runs the blit CONCURRENTLY with LVGL rendering, so
		 * the blitter should yield the DDR bus to the CPU after every
		 * transaction and take leftover bandwidth (the blit is hidden —
		 * its duration doesn't matter until it exceeds render time).
		 * Board-swept 2026-07-15: tenure 8 -> 1 cuts render inflation
		 * from ~+21 ms to ~+9 ms/frame; the hidden blit stretches
		 * 26.7 -> 32.5 ms.  (Hardware default 8 is the sync-flush
		 * optimum; it survives program loads, hence set explicitly.) */
		BLIT_R64(BLIT_CHUNK) = 1;
	}
#endif
	return 0;
}

static const struct display_driver_api vncd_api = {
	.blanking_on = vncd_blanking_on,
	.blanking_off = vncd_blanking_off,
	.write = vncd_write,
	.get_capabilities = vncd_get_capabilities,
	.set_pixel_format = vncd_set_pixel_format,
};

#define VNCD_DEFINE(n)							\
	DEVICE_DT_INST_DEFINE(n, vncd_init, NULL, NULL, NULL,		\
			      POST_KERNEL, CONFIG_DISPLAY_INIT_PRIORITY,\
			      &vncd_api);

DT_INST_FOREACH_STATUS_OKAY(VNCD_DEFINE)
