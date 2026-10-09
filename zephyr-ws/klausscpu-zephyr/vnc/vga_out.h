/*
 * vga_out.h — show the framebuffer on the KlaussCPU VGA port (CONFIG_KLAUSSCPU_VGA).
 *
 * With CONFIG_KLAUSSCPU_VGA the VGA hardware scans the RGB565 framebuffer
 * (framebuffer.c) straight out of DDR from boot: 320-wide buffers are shown
 * pixel-doubled, and the image is centred vertically (doom's 320x200 is
 * letterboxed).  Apps keep drawing into fb_pixels() + fb_mark_dirty() as for
 * VNC; a low-priority thread flushes the CPU cache to DDR once per frame
 * whenever the framebuffer was marked dirty, so VGA follows with no app
 * changes (gui_demo, mandel, LVGL via display_vnc, doom over VNC).
 *
 * Apps that already render 8-bit palette indices (doom) can instead hand the
 * frame to vga_out_show_indexed(): it is double-buffered into the VGA's 8-bit
 * palette mode and flipped at vblank (no tearing, no RGB565 conversion).
 */
#ifndef KLAUSSCPU_VGA_OUT_H_
#define KLAUSSCPU_VGA_OUT_H_

#include <stdint.h>

/* The framebuffer changed: flush the cache on the next frame tick.  Cheap and
 * lock-free; framebuffer.c calls it from fb_mark_dirty(). */
void vga_out_kick(void);

/* Show a w x h frame of 8-bit palette indices (w = 320 shown doubled, or 640;
 * rows tightly packed).  The pixels are copied, so `src` may be reused at
 * once.  `pal_rgb888` (256 x 0x00RRGGBB) updates the palette when non-NULL.
 * Returns 0, or -EINVAL / -ENOMEM.  The first call switches VGA to this mode. */
int vga_out_show_indexed(const uint8_t *src, int w, int h, const uint32_t *pal_rgb888);

/* Go back to scanning the RGB565 framebuffer. */
void vga_out_show_fb(void);

/* Display lines whose DDR fetch was late since boot (should stay 0). */
uint32_t vga_out_underflows(void);

#endif /* KLAUSSCPU_VGA_OUT_H_ */
