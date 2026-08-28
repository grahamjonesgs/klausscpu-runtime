/*
 * framebuffer.c — in-RAM RGB565 framebuffer + dirty-rectangle tracking.
 * See framebuffer.h for the model.
 */

#include <zephyr/kernel.h>
#include <string.h>

#include "framebuffer.h"

/* ~600 KiB in SRAM (the board has ~128 MiB, so this is negligible). */
/* 8-byte aligned: AMP core 2 reads the fb through its uncached DDR window
 * with 64-bit loads (vnc_c2.c fb_fetch) — one burst per 4 px instead of 4. */
static uint16_t fb[FB_WIDTH * FB_HEIGHT] __attribute__((aligned(8)));

/* Guards both the pixels and the dirty box below. */
static K_MUTEX_DEFINE(fb_mutex);

/* Dirty bounding box, inclusive-exclusive [x0,x1) x [y0,y1).  Empty when
 * x0 >= x1 (nothing to send). */
static int dirty_x0, dirty_y0, dirty_x1, dirty_y1;

void fb_lock(void)   { k_mutex_lock(&fb_mutex, K_FOREVER); }
void fb_unlock(void) { k_mutex_unlock(&fb_mutex); }

uint16_t *fb_pixels(void) { return fb; }

/* Caller holds the lock.  Expand the dirty box to cover [x,x+w) x [y,y+h). */
static void dirty_add_locked(int x, int y, int w, int h)
{
	int x1 = x + w;
	int y1 = y + h;

	if (x < 0)          { x = 0; }
	if (y < 0)          { y = 0; }
	if (x1 > FB_WIDTH)  { x1 = FB_WIDTH; }
	if (y1 > FB_HEIGHT) { y1 = FB_HEIGHT; }
	if (x >= x1 || y >= y1) {
		return;
	}

	if (dirty_x0 >= dirty_x1) {       /* box was empty */
		dirty_x0 = x;  dirty_y0 = y;
		dirty_x1 = x1; dirty_y1 = y1;
		return;
	}
	if (x  < dirty_x0) { dirty_x0 = x;  }
	if (y  < dirty_y0) { dirty_y0 = y;  }
	if (x1 > dirty_x1) { dirty_x1 = x1; }
	if (y1 > dirty_y1) { dirty_y1 = y1; }
}

void fb_mark_dirty(int x, int y, int w, int h)
{
	fb_lock();
	dirty_add_locked(x, y, w, h);
	fb_unlock();
}

bool fb_take_dirty(int *x, int *y, int *w, int *h)
{
	bool dirty;

	fb_lock();
	dirty = (dirty_x0 < dirty_x1);
	if (dirty) {
		*x = dirty_x0;
		*y = dirty_y0;
		*w = dirty_x1 - dirty_x0;
		*h = dirty_y1 - dirty_y0;
		dirty_x0 = dirty_x1 = 0;   /* clear */
	}
	fb_unlock();
	return dirty;
}

void fb_clear(uint16_t color)
{
	fb_lock();
	for (int i = 0; i < FB_WIDTH * FB_HEIGHT; i++) {
		fb[i] = color;
	}
	dirty_add_locked(0, 0, FB_WIDTH, FB_HEIGHT);
	fb_unlock();
}

void fb_put_pixel(int x, int y, uint16_t color)
{
	if ((unsigned)x >= FB_WIDTH || (unsigned)y >= FB_HEIGHT) {
		return;
	}
	fb_lock();
	fb[y * FB_WIDTH + x] = color;
	dirty_add_locked(x, y, 1, 1);
	fb_unlock();
}

void fb_fill_rect(int x, int y, int w, int h, uint16_t color)
{
	fb_lock();
	int x1 = x + w, y1 = y + h;

	if (x < 0)          { x = 0; }
	if (y < 0)          { y = 0; }
	if (x1 > FB_WIDTH)  { x1 = FB_WIDTH; }
	if (y1 > FB_HEIGHT) { y1 = FB_HEIGHT; }

	for (int yy = y; yy < y1; yy++) {
		for (int xx = x; xx < x1; xx++) {
			fb[yy * FB_WIDTH + xx] = color;
		}
	}
	dirty_add_locked(x, y, x1 - x, y1 - y);
	fb_unlock();
}

/* RGB565 as a constant expression (so the bar table can be static const). */
#define RGB565(r, g, b) \
	((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

/* Eight SMPTE-style vertical colour bars over the top 2/3, a grey ramp across
 * the bottom 1/3.  The bars make wrong channel masks/shifts in the VNC pixel
 * conversion obvious at a glance. */
static const uint16_t bars[8] = {
	RGB565(255, 255, 255),   /* white   */
	RGB565(255, 255, 0),     /* yellow  */
	RGB565(0,   255, 255),   /* cyan    */
	RGB565(0,   255, 0),     /* green   */
	RGB565(255, 0,   255),   /* magenta */
	RGB565(255, 0,   0),     /* red     */
	RGB565(0,   0,   255),   /* blue    */
	RGB565(0,   0,   0),     /* black   */
};

static uint16_t pattern_pixel(int x, int y)
{
	if (y < (FB_HEIGHT * 2) / 3) {
		int b = x / (FB_WIDTH / 8);

		return bars[b > 7 ? 7 : b];
	}
	uint8_t g = (uint8_t)(x * 255 / (FB_WIDTH - 1));

	return fb_rgb(g, g, g);
}

void fb_draw_pattern(int x, int y, int w, int h)
{
	int x1 = x + w, y1 = y + h;

	if (x < 0)          { x = 0; }
	if (y < 0)          { y = 0; }
	if (x1 > FB_WIDTH)  { x1 = FB_WIDTH; }
	if (y1 > FB_HEIGHT) { y1 = FB_HEIGHT; }

	fb_lock();
	for (int yy = y; yy < y1; yy++) {
		for (int xx = x; xx < x1; xx++) {
			fb[yy * FB_WIDTH + xx] = pattern_pixel(xx, yy);
		}
	}
	dirty_add_locked(x, y, x1 - x, y1 - y);
	fb_unlock();
}

void fb_test_pattern(void)
{
	fb_draw_pattern(0, 0, FB_WIDTH, FB_HEIGHT);
}
