/*
 * framebuffer.h — in-RAM RGB565 framebuffer for the VNC server.
 *
 * The framebuffer lives in normal SRAM (no VGA scanout hardware involved): a
 * VGA controller would read this and drive a monitor's RGB pins, but here the
 * VNC server (vnc_server.c) reads it and ships pixels over the network instead.
 *
 * Native format is RGB565, little-endian, one uint16_t per pixel.  Drawing code
 * (and, later, an FPGA framebuffer region) writes pixels; the VNC server reads
 * them.  Because both run on different threads, batch accesses under fb_lock();
 * the VNC server copies pixels out under the lock and sends them *unlocked*.
 */

#ifndef KLAUSSCPU_VNC_FRAMEBUFFER_H_
#define KLAUSSCPU_VNC_FRAMEBUFFER_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FB_WIDTH  640
#define FB_HEIGHT 480

/* Lock/unlock around a batch of reads or writes.  Never hold across a network
 * send — copy the pixels out, unlock, then send (see send_rect in
 * vnc_server.c). */
void fb_lock(void);
void fb_unlock(void);

/* Raw pixel access — caller must hold fb_lock().  Row stride is FB_WIDTH. */
uint16_t *fb_pixels(void);

/* Pack 8-bit RGB into native RGB565. */
static inline uint16_t fb_rgb(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Drawing primitives (each takes/releases the lock and marks dirty). */
void fb_clear(uint16_t color);
void fb_put_pixel(int x, int y, uint16_t color);
void fb_fill_rect(int x, int y, int w, int h, uint16_t color);

/* Dirty-rectangle tracking: writes expand a bounding box of changed pixels;
 * the VNC server reads and clears it to send incremental updates.  Milestone 5
 * uses this; milestone 4 sends full frames. */
void fb_mark_dirty(int x, int y, int w, int h);
bool fb_take_dirty(int *x, int *y, int *w, int *h);   /* false if clean */

/* Fill the framebuffer with a test pattern (colour bars + grey ramp) so the
 * VNC pipeline can be validated before any real graphics source exists. */
void fb_test_pattern(void);

/* Repaint just [x,y,w,h] with the test pattern (and mark it dirty).  Used to
 * "erase" a moving element back to the background. */
void fb_draw_pattern(int x, int y, int w, int h);

#endif /* KLAUSSCPU_VNC_FRAMEBUFFER_H_ */
