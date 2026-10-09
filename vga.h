/*
 * vga.h — KlaussCPU VGA output helpers (header-only; baremetal and Zephyr).
 *
 * Thin layer over the VGA registers in mmio.h (device 0xF011_xxxx; RTL and
 * full register description: KlaussCPU VGA_PLAN.md / MMIO_MAP.md).  The
 * display is fixed at 640x480 @ 59.5 Hz and scans a framebuffer straight out
 * of DDR:
 *
 *   vga_set_mode(VGA_FMT_RGB565, 1, fb, 640, 240, 0);   // 320x240 doubled
 *   ... draw into fb ...
 *   vga_cache_flush();                                   // CPU writes -> DDR
 *   vga_present(fb);  vga_wait_vsync();                  // tear-free flip
 *
 * Rules (enforced by hardware, not checked here):
 *   - fb and stride are 32-byte aligned; width is 320 (doubled) or 640 px;
 *   - CTRL scanout bits / FB_BASE / STRIDE / HEIGHT / VSTART take effect at
 *     the next vblank;
 *   - the CPU draws through a write-back cache: flush before presenting
 *     (blitter output needs no flush — it writes DDR directly).
 */
#ifndef KLAUSSCPU_VGA_H_
#define KLAUSSCPU_VGA_H_

#include <stdint.h>
#include "mmio.h"

enum vga_fmt {
    VGA_FMT_RGB565 = 0,     /* 2 B/pixel, little-endian; top 4 bits per channel shown */
    VGA_FMT_INDEX8 = 1,     /* 1 B/pixel, looked up in the 256-entry palette */
};

#define VGA_SCREEN_W 640
#define VGA_SCREEN_H 480

/* Write back every dirty cache line to DDR (full-cache walk; synchronous). */
static inline void vga_cache_flush(void)
{
    REG_CACHE_CTRL = CACHE_CTRL_FLUSH;
    while (REG_CACHE_STATUS & CACHE_STATUS_MNT_BUSY) {}
}

/* Busy-wait for the next start of vblank (raster line 480). */
static inline void vga_wait_vsync(void)
{
    REG_VGA_STATUS = VGA_STATUS_VSYNC;
    while (!(REG_VGA_STATUS & VGA_STATUS_VSYNC)) {}
}

/* Scan out `height` lines of a framebuffer at `fb`, `stride` bytes apart.
 * double_px: 320-pixel source lines, every pixel and line shown twice.
 * vstart:    first display line (centre: (480 - height*(double_px?2:1)) / 2).
 * Takes effect at the next vblank. */
static inline void vga_set_mode(enum vga_fmt fmt, int double_px, uint32_t fb,
                                uint32_t stride, uint32_t height, uint32_t vstart)
{
    REG_VGA_FB_BASE = fb;
    REG_VGA_STRIDE  = stride;
    REG_VGA_HEIGHT  = height;
    REG_VGA_VSTART  = vstart;
    REG_VGA_CTRL    = VGA_CTRL_SCANOUT_EN |
                      (double_px ? VGA_CTRL_DOUBLE : 0u) |
                      (fmt == VGA_FMT_INDEX8 ? VGA_CTRL_BPP8 : 0u);
}

/* Centred mode for a w x h buffer (w = 320 doubled or 640 native). */
static inline void vga_set_mode_centred(enum vga_fmt fmt, uint32_t fb,
                                        uint32_t w, uint32_t h)
{
    int dbl = (w <= 320);
    uint32_t bpp = (fmt == VGA_FMT_INDEX8) ? 1u : 2u;
    uint32_t shown = dbl ? 2u * h : h;

    vga_set_mode(fmt, dbl, fb, w * bpp, h,
                 shown < VGA_SCREEN_H ? (VGA_SCREEN_H - shown) / 2u : 0u);
}

/* Switch scanout off (border colour only) — takes effect at the next vblank. */
static inline void vga_off(void) { REG_VGA_CTRL = 0; }

/* Flip to another framebuffer (same geometry) at the next vblank. */
static inline void vga_present(uint32_t fb) { REG_VGA_FB_BASE = fb; }

/* Border / letterbox colour, 0xRGB (4 bits per channel). */
static inline void vga_set_border(uint32_t rgb444) { REG_VGA_BORDER = rgb444 & 0xFFFu; }

/* 8-bit-to-4-bit channel conversion with rounding (0..255 -> 0..15). */
static inline uint32_t vga_rgb888_to_444(uint32_t r, uint32_t g, uint32_t b)
{
    r = (r * 15u + 127u) / 255u;
    g = (g * 15u + 127u) / 255u;
    b = (b * 15u + 127u) / 255u;
    return (r << 8) | (g << 4) | b;
}

/* Load palette entries [first, first+n) from 0x00RRGGBB values.  Changes are
 * immediate — load during vblank (after vga_wait_vsync) to avoid a visible
 * mid-frame switch. */
static inline void vga_set_palette_rgb888(uint32_t first, uint32_t n, const uint32_t *rgb)
{
    for (uint32_t i = 0; i < n && first + i < 256u; i++) {
        uint32_t c = rgb[i];
        REG_VGA_PALETTE(first + i) = vga_rgb888_to_444((c >> 16) & 0xFFu, (c >> 8) & 0xFFu,
                                                       c & 0xFFu);
    }
}

/* Display lines whose DDR fetch was late since the last clear (should be 0). */
static inline uint32_t vga_underflows(void) { return VGA_STATUS_UNDERFLOW(REG_VGA_STATUS); }
static inline void vga_clear_underflows(void) { REG_VGA_STATUS = VGA_STATUS_UNDERFLOW_CLR; }

#endif /* KLAUSSCPU_VGA_H_ */
