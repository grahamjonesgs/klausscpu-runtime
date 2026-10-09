/*
 * vga_out.c — framebuffer -> KlaussCPU VGA port.  See vga_out.h.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>

#include "framebuffer.h"
#include "vga_out.h"
#include "vga.h"            /* runtime root: register helpers over mmio.h */

LOG_MODULE_REGISTER(vga_out, LOG_LEVEL_INF);

BUILD_ASSERT(FB_WIDTH == 320 || FB_WIDTH == 640,
	     "VGA scans 320 (doubled) or 640 pixel lines");
BUILD_ASSERT(FB_HEIGHT * (FB_WIDTH == 320 ? 2 : 1) <= VGA_SCREEN_H,
	     "framebuffer taller than the 640x480 screen");

/* fb dirty since the last flush */
static atomic_t fb_pending;

/* Indexed (8-bit) frames: a pool of three 32 B-aligned buffers, allocated on
 * first use.  At any time one is on screen (FB_ACTIVE), one may be presented
 * but not yet latched (it flips at the next vblank), and the third is free to
 * render into — so a renderer never writes a visible frame, at any rate. */
#define IDX_NBUF 3
static uint8_t *idx_buf[IDX_NBUF];
static uint8_t *idx_last;       /* most recently presented */
static int idx_w, idx_h;
static bool idx_active;
uint32_t vga_out_underflows(void)
{
	return vga_underflows();
}

void vga_out_kick(void)
{
	atomic_set(&fb_pending, 1);
}

static void show_fb_mode(void)
{
	vga_set_mode_centred(VGA_FMT_RGB565, (uint32_t)(uintptr_t)fb_pixels(),
			     FB_WIDTH, FB_HEIGHT);
}

void vga_out_show_fb(void)
{
	idx_active = false;
	vga_cache_flush();
	show_fb_mode();
}

static int idx_alloc(int w, int h)
{
	if ((w != 320 && w != 640) || h <= 0 || h * (w == 320 ? 2 : 1) > VGA_SCREEN_H) {
		return -EINVAL;
	}
	if (idx_buf[0] != NULL && w == idx_w && h == idx_h) {
		return 0;
	}
	if (idx_active) {               /* geometry change: stop scanning old buffers */
		vga_out_show_fb();
	}
	for (int i = 0; i < IDX_NBUF; i++) {
		if (idx_buf[i] != NULL) {
			k_free(idx_buf[i]);
		}
		/* VGA needs 32 B-aligned frames; w is a multiple of 32 */
		idx_buf[i] = k_aligned_alloc(32, (size_t)w * h);
		if (idx_buf[i] == NULL) {
			LOG_ERR("no memory for the %dx%d indexed VGA buffers", w, h);
			idx_buf[0] = NULL;
			return -ENOMEM;
		}
	}
	idx_w = w;
	idx_h = h;
	idx_last = NULL;
	return 0;
}

uint8_t *vga_out_indexed_buffer(int w, int h)
{
	if (idx_alloc(w, h) != 0) {
		return NULL;
	}
	uint32_t on_screen = (uint32_t)REG_VGA_FB_ACTIVE;

	for (int i = 0; i < IDX_NBUF; i++) {
		uint8_t *b = idx_buf[i];

		if (b != idx_last && (!idx_active || (uint32_t)(uintptr_t)b != on_screen)) {
			return b;
		}
	}
	return idx_buf[0];              /* unreachable with 3 buffers */
}

int vga_out_present_indexed(uint8_t *buf, const uint32_t *pal_rgb888)
{
	bool ours = false;

	for (int i = 0; i < IDX_NBUF; i++) {
		ours |= (buf != NULL && buf == idx_buf[i]);
	}
	if (!ours) {
		return -EINVAL;
	}
	if (pal_rgb888 != NULL) {
		/* Immediate; at worst the frame still on screen shows the new
		 * palette for < 1 frame (doom's palette flashes are frame-scale). */
		vga_set_palette_rgb888(0, 256, pal_rgb888);
	}
	vga_cache_flush();              /* the CPU's writes -> DDR */

	if (!idx_active) {
		vga_set_mode_centred(VGA_FMT_INDEX8, (uint32_t)(uintptr_t)buf, idx_w, idx_h);
		idx_active = true;
	} else {
		vga_present((uint32_t)(uintptr_t)buf);
	}
	idx_last = buf;
	return 0;
}

int vga_out_show_indexed(const uint8_t *src, int w, int h, const uint32_t *pal_rgb888)
{
	uint8_t *buf = vga_out_indexed_buffer(w, h);

	if (buf == NULL) {
		return idx_buf[0] == NULL ? -ENOMEM : -EINVAL;
	}
	memcpy(buf, src, (size_t)w * h);
	return vga_out_present_indexed(buf, pal_rgb888);
}

/* Once per frame: push a dirty RGB565 framebuffer to DDR.  Tearing is
 * possible (single buffer, like the VNC path), never stale pixels for more
 * than a frame. */
static void vga_flush_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	for (;;) {
		k_msleep(16);
		if (atomic_set(&fb_pending, 0) && !idx_active) {
			vga_cache_flush();
		}
	}
}

K_THREAD_DEFINE(vga_flush_tid, 2048, vga_flush_thread, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO - 1, 0, 0);

static int vga_out_init(void)
{
	vga_set_border(0x000);
	vga_clear_underflows();
	vga_cache_flush();
	show_fb_mode();
	LOG_INF("VGA: %dx%d RGB565 framebuffer%s at %p", FB_WIDTH, FB_HEIGHT,
		FB_WIDTH == 320 ? " (doubled)" : "", (void *)fb_pixels());
	return 0;
}

SYS_INIT(vga_out_init, APPLICATION, 90);
