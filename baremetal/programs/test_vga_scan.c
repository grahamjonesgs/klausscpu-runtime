// test_vga_scan — board test for VGA DDR scanout (KlaussCPU VGA_PLAN.md Phase 2).
//
// Each step shows an image for a few seconds and reports the UNDERFLOW count
// (display lines whose DDR fetch did not finish in time — should be 0).
//
//  S1  320x240 RGB565, doubled to 640x480: gradient, colour bars, white
//      1-pixel frame (2 px on screen) on all four edges, checkerboard.
//  S2  Page flipping: a square bouncing over S1's image, double-buffered,
//      one flip per vsync, full-cache flush every frame (VGA must get the bus
//      during flush walks).
//  S3  S1 under DDR stress: blitter COPYs + CPU memcpy + cache flushes.
//  S4  320x200 doubled (doom's shape), letterboxed at VSTART=40, blue border.
//  S5  640x480 8-bit palette, native (no doubling).
//  S6  640x480 RGB565 native — 40 wide reads per line, the heaviest mode —
//      idle, then under memcpy stress.
//  M   memcpy throughput with scanout off / 320x240 / 640x480x16.
// The program ends showing S1.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../mmio.h"

#define FB_A    0x02000000u     // 320x240x2 = 150 KB
#define FB_B    0x02040000u
#define FB_LB   0x02080000u     // 320x200x2
#define FB_8    0x020C0000u     // 640x480x1 = 300 KB
#define FB_16   0x02200000u     // 640x480x2 = 600 KB
#define BUF_SRC 0x02400000u     // memcpy / blit scratch (1 MB each)
#define BUF_DST 0x02500000u

#define BLIT_BASE        0xF00E0000u
#define BR(o)            (*(volatile uint64_t *)(uintptr_t)(BLIT_BASE + (o)))

static int pass, fail;
static void check(const char *name, int ok) {
    printf("%-46s %s\n", name, ok ? "PASS" : "FAIL");
    if (ok) pass++; else fail++;
}

static void wait_ms(uint32_t ms) {
    uint64_t t0 = REG_CLOCK_MS;
    while (REG_CLOCK_MS - t0 < ms) {}
}

static void flush(void) {
    REG_CACHE_CTRL = CACHE_CTRL_FLUSH;
    while (REG_CACHE_STATUS & CACHE_STATUS_MNT_BUSY) {}
}

static void wait_vsync(void) {             // next start of vblank
    REG_VGA_STATUS = VGA_STATUS_VSYNC;
    while (!(REG_VGA_STATUS & VGA_STATUS_VSYNC)) {}
}

static uint32_t underflows(void) { return VGA_STATUS_UNDERFLOW(REG_VGA_STATUS); }
static void clear_underflows(void) { REG_VGA_STATUS = VGA_STATUS_UNDERFLOW_CLR; }

// Program a mode; it takes effect at the next vblank — wait two so the first
// full frame in the new mode has been fetched before counting.
static void set_mode(uint32_t ctrl, uint32_t base, uint32_t stride, uint32_t height,
                     uint32_t vstart) {
    REG_VGA_FB_BASE = base;
    REG_VGA_STRIDE  = stride;
    REG_VGA_HEIGHT  = height;
    REG_VGA_VSTART  = vstart;
    REG_VGA_CTRL    = ctrl;
    wait_vsync();
    wait_vsync();
    clear_underflows();
}

static inline uint16_t rgb565(uint32_t r5, uint32_t g6, uint32_t b5) {
    return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// S1 image: 320x240 RGB565.
static void draw_test_320(uint32_t fb, uint32_t h) {
    volatile uint16_t *p = (volatile uint16_t *)(uintptr_t)fb;
    static const uint16_t bars[8] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000};
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < 320; x++) {
            uint16_t c;
            if (x == 0 || x == 319 || y == 0 || y == h - 1)
                c = 0xFFFF;
            else if (y < 40)
                c = bars[x / 40];
            else if (x >= 128 && x < 192 && y >= 88 && y < 152)
                c = (((x >> 3) ^ (y >> 3)) & 1) ? 0xFFFF : 0x0000;
            else
                c = rgb565(x * 31 / 319, y * 63 / (h - 1), 31 - x * 31 / 319);
            p[y * 320 + x] = c;
        }
    }
}

static void restore_rect(uint32_t fb, uint32_t clean, int x0, int y0, int w, int h) {
    volatile uint16_t *d = (volatile uint16_t *)(uintptr_t)fb;
    const volatile uint16_t *s = (const volatile uint16_t *)(uintptr_t)clean;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            d[y * 320 + x] = s[y * 320 + x];
}

static void fill_rect(uint32_t fb, int x0, int y0, int w, int h, uint16_t c) {
    volatile uint16_t *p = (volatile uint16_t *)(uintptr_t)fb;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            p[y * 320 + x] = c;
}

static void blit_copy(uint32_t dst, uint32_t src, uint32_t w, uint32_t h, uint32_t stride) {
    BR(0x10) = dst; BR(0x18) = stride;
    BR(0x20) = src; BR(0x28) = stride;
    BR(0x30) = w;   BR(0x38) = h;
    BR(0x00) = 1u | (1u << 1);            // START, op COPY
    while (BR(0x08) & 1u) {}
    BR(0x08) = 2u;                        // W1C DONE
}

// memcpy MB/s over ~1 s (copies 1 MB repeatedly)
static uint32_t memcpy_rate(void) {
    uint64_t t0 = REG_CLOCK_MS;
    uint32_t mb = 0;
    while (REG_CLOCK_MS - t0 < 1000) {
        memcpy((void *)(uintptr_t)BUF_DST, (const void *)(uintptr_t)BUF_SRC, 1u << 20);
        mb++;
    }
    return (uint32_t)((uint64_t)mb * 1000u / (uint32_t)(REG_CLOCK_MS - t0));
}

int main(void) {
    printf("\ntest_vga_scan: VGA Phase 2 (DDR scanout) board test\n");
    REG_INT_MASK = 0;
    REG_VGA_BORDER = 0x000;

    // ── S1 ──
    draw_test_320(FB_A, 240);
    flush();
    set_mode(VGA_CTRL_SCANOUT_EN | VGA_CTRL_DOUBLE, FB_A, 640, 240, 0);
    wait_ms(3000);
    uint32_t u1 = underflows();
    printf("   S1 underflows in 3 s: %lu\n", (unsigned long)u1);
    check("S1 320x240 RGB565 doubled, no underflow", u1 == 0);

    // ── S2 ──
    memcpy((void *)(uintptr_t)FB_B,  (const void *)(uintptr_t)FB_A, 320 * 240 * 2);
    memcpy((void *)(uintptr_t)FB_16, (const void *)(uintptr_t)FB_A, 320 * 240 * 2);  // clean copy
    flush();
    clear_underflows();
    {
        uint32_t fb[2] = {FB_A, FB_B};
        int bx[2] = {40, 40}, by[2] = {50, 50}; // last square drawn in each buffer
        int x = 40, y = 50, dx = 3, dy = 2;
        uint32_t flips = 0, f0 = VGA_STATUS_FRAME(REG_VGA_STATUS);
        for (int i = 0; i < 180; i++) {
            int b = i & 1;
            // erase this buffer's previous square from the clean copy
            restore_rect(fb[b], FB_16, bx[b], by[b], 24, 24);
            fill_rect(fb[b], x, y, 24, 24, rgb565(31, 32, 0));
            bx[b] = x; by[b] = y;
            flush();
            REG_VGA_FB_BASE = fb[b];
            wait_vsync();
            flips++;
            x += dx; y += dy;
            if (x < 2 || x > 320 - 26) { dx = -dx; x += 2 * dx; }
            if (y < 42 || y > 240 - 26) { dy = -dy; y += 2 * dy; }
        }
        uint32_t frames = (VGA_STATUS_FRAME(REG_VGA_STATUS) - f0) & 0xFFFFu;
        uint32_t u2 = underflows();
        printf("   S2 %lu flips over %lu frames, underflows %lu\n",
               (unsigned long)flips, (unsigned long)frames, (unsigned long)u2);
        check("S2 page flip + flush per frame, no underflow", u2 == 0);
    }

    // ── S3 ──
    draw_test_320(FB_A, 240);
    flush();
    set_mode(VGA_CTRL_SCANOUT_EN | VGA_CTRL_DOUBLE, FB_A, 640, 240, 0);
    {
        uint64_t t0 = REG_CLOCK_MS;
        uint32_t blits = 0, copies = 0;
        while (REG_CLOCK_MS - t0 < 3000) {
            blit_copy(BUF_DST, BUF_SRC, 320, 240, 640);   blits++;
            memcpy((void *)(uintptr_t)(BUF_DST + 0x80000u),
                   (const void *)(uintptr_t)(BUF_SRC + 0x80000u), 256u << 10);
            copies++;
            REG_CACHE_CTRL = CACHE_CTRL_FLUSH;              // dirty-cache walk
            while (REG_CACHE_STATUS & CACHE_STATUS_MNT_BUSY) {}
        }
        uint32_t u3 = underflows();
        printf("   S3 %lu blits + %lu x 256 KB memcpy + flushes in 3 s, underflows %lu\n",
               (unsigned long)blits, (unsigned long)copies, (unsigned long)u3);
        check("S3 320x240 under DDR stress, no underflow", u3 == 0);
    }

    // ── S4 ──
    draw_test_320(FB_LB, 200);
    flush();
    REG_VGA_BORDER = 0x008;
    set_mode(VGA_CTRL_SCANOUT_EN | VGA_CTRL_DOUBLE, FB_LB, 640, 200, 40);
    wait_ms(3000);
    uint32_t u4 = underflows();
    printf("   S4 underflows in 3 s: %lu\n", (unsigned long)u4);
    check("S4 320x200 letterboxed (blue bars), no underflow", u4 == 0);
    REG_VGA_BORDER = 0x000;

    // ── S5 ──
    for (uint32_t i = 0; i < 256; i++)
        REG_VGA_PALETTE(i) = ((i >> 4) << 8) | ((i & 15u) << 4) | (15u - (i >> 4));
    {
        volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)FB_8;
        for (uint32_t y = 0; y < 480; y++)
            for (uint32_t x = 0; x < 640; x++)
                p[y * 640 + x] = (x == 0 || x == 639 || y == 0 || y == 479) ? 0xFF
                                 : (uint8_t)(((y * 16 / 480) << 4) | (x * 16 / 640));
    }
    flush();
    set_mode(VGA_CTRL_SCANOUT_EN | VGA_CTRL_BPP8, FB_8, 640, 480, 0);
    wait_ms(3000);
    uint32_t u5 = underflows();
    printf("   S5 underflows in 3 s: %lu\n", (unsigned long)u5);
    check("S5 640x480 8-bit palette native, no underflow", u5 == 0);

    // ── S6 ──
    {
        volatile uint16_t *p = (volatile uint16_t *)(uintptr_t)FB_16;
        for (uint32_t y = 0; y < 480; y++)
            for (uint32_t x = 0; x < 640; x++)
                p[y * 640 + x] = (x == 0 || x == 639 || y == 0 || y == 479) ? 0xFFFF
                                 : rgb565(x * 31 / 639, y * 63 / 479, 31 - x * 31 / 639);
    }
    flush();
    set_mode(VGA_CTRL_SCANOUT_EN, FB_16, 1280, 480, 0);
    wait_ms(3000);
    uint32_t u6 = underflows();
    clear_underflows();
    uint64_t t0 = REG_CLOCK_MS;
    while (REG_CLOCK_MS - t0 < 3000)
        memcpy((void *)(uintptr_t)BUF_DST, (const void *)(uintptr_t)BUF_SRC, 1u << 20);
    uint32_t u6s = underflows();
    printf("   S6 underflows: %lu idle (3 s), %lu under memcpy (3 s)\n",
           (unsigned long)u6, (unsigned long)u6s);
    check("S6 640x480 RGB565 native, idle, no underflow", u6 == 0);
    check("S6 640x480 RGB565 native, memcpy, no underflow", u6s == 0);

    // ── M: cost to the CPU ──
    REG_VGA_CTRL = 0;  wait_vsync(); wait_vsync();
    uint32_t m_off = memcpy_rate();
    set_mode(VGA_CTRL_SCANOUT_EN | VGA_CTRL_DOUBLE, FB_A, 640, 240, 0);
    uint32_t m_320 = memcpy_rate();
    set_mode(VGA_CTRL_SCANOUT_EN, FB_16, 1280, 480, 0);
    uint32_t m_640 = memcpy_rate();
    printf("   memcpy MB/s: scanout off %lu, 320x240x16 %lu, 640x480x16 %lu\n",
           (unsigned long)m_off, (unsigned long)m_320, (unsigned long)m_640);

    // leave S1 on screen
    draw_test_320(FB_A, 240);
    flush();
    set_mode(VGA_CTRL_SCANOUT_EN | VGA_CTRL_DOUBLE, FB_A, 640, 240, 0);

    printf("test_vga_scan: %d passed, %d failed\n", pass, fail);
    return fail;
}
