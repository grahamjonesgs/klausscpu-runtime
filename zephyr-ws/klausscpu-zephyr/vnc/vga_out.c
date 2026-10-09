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

/* Indexed (8-bit) double buffer, allocated on first use. */
static uint8_t *idx_buf[2];
static int idx_back;            /* buffer the next frame goes into */
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

int vga_out_show_indexed(const uint8_t *src, int w, int h, const uint32_t *pal_rgb888)
{
	if ((w != 320 && w != 640) || h <= 0 || h * (w == 320 ? 2 : 1) > VGA_SCREEN_H) {
		return -EINVAL;
	}
	if (idx_buf[0] == NULL || w != idx_w || h != idx_h) {
		for (int i = 0; i < 2; i++) {
			if (idx_buf[i] != NULL) {
				k_free(idx_buf[i]);
			}
			/* VGA needs 32 B-aligned frames; w is a multiple of 32 */
			idx_buf[i] = k_aligned_alloc(32, (size_t)w * h);
			if (idx_buf[i] == NULL) {
				LOG_ERR("no memory for the %dx%d indexed VGA buffers", w, h);
				return -ENOMEM;
			}
		}
		idx_w = w;
		idx_h = h;
		idx_active = false;
	}

	/* The back buffer may still be on screen if the previous flip has not
	 * reached its vblank yet (only when frames come faster than 60 Hz). */
	uint8_t *back = idx_buf[idx_back];

	if (idx_active) {
		while (REG_VGA_FB_ACTIVE == (uint32_t)(uintptr_t)back) {
			k_yield();
		}
	}

	memcpy(back, src, (size_t)w * h);
	if (pal_rgb888 != NULL) {
		/* Immediate; at worst the frame still on screen shows the new
		 * palette for < 1 frame (doom's palette flashes are frame-scale). */
		vga_set_palette_rgb888(0, 256, pal_rgb888);
	}
	vga_cache_flush();

	if (!idx_active) {
		vga_set_mode_centred(VGA_FMT_INDEX8, (uint32_t)(uintptr_t)back, w, h);
		idx_active = true;
	} else {
		vga_present((uint32_t)(uintptr_t)back);
	}
	idx_back ^= 1;
	return 0;
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
