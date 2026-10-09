// test_vga — board test for the VGA block (KlaussCPU VGA_PLAN.md Phase 1).
//
//  T1  Register readback (CTRL, FB_BASE, STRIDE, BORDER, VSTART).
//  T2  Palette: all 256 entries write + read back.
//  T3  STATUS: frame counter runs at 59.5 Hz; the raster line field moves.
//  T4  Vsync IRQ (source 3): ~119 dispatches in 2 s, W1C leaves INT_PENDING[3]
//      clear, none while VSYNC_IRQ_EN is off.
//  T5  FB_BASE written mid-picture reaches FB_ACTIVE only at the next vblank.
//
// Visual (watch the monitor):
//  V1  3 s of border colours: red, green, blue (whole screen one colour each).
//  V2  Palette view, 16x16 grid: red rises top->bottom, green rises
//      left->right, blue falls top->bottom.
//  V3  5 s of palette cycling, one step per vsync — smooth diagonal motion.
//  The program ends leaving V2's static grid on screen.
#include <stdint.h>
#include <stdio.h>
#include "../../mmio.h"

static int pass, fail;
static void check(const char *name, int ok) {
    printf("%-44s %s\n", name, ok ? "PASS" : "FAIL");
    if (ok) pass++; else fail++;
}

static void wait_ms(uint32_t ms) {
    uint64_t t0 = REG_CLOCK_MS;
    while (REG_CLOCK_MS - t0 < ms) {}
}

// ── vsync ISR ──────────────────────────────────────────────────────────────
volatile uint32_t vsyncs;

void vga_isr_c(void) {
    REG_VGA_STATUS = VGA_STATUS_VSYNC;      // W1C before IRET (level source)
    vsyncs++;
}

// Vector target: save the caller-saved registers, run the C handler, IRET
// (which restores PC, flags and INT_MASK).
__asm__(
    "  .text\n  .p2align 3\n  .globl vga_isr\n"
    "vga_isr:\n"
    "  push r0\n  push r1\n  push r2\n  push r3\n  push r8\n  push r9\n"
    "  push r10\n  push r11\n  push r12\n  push r13\n  push r14\n"
    "  call vga_isr_c\n"
    "  pop r14\n  pop r13\n  pop r12\n  pop r11\n  pop r10\n  pop r9\n"
    "  pop r8\n  pop r3\n  pop r2\n  pop r1\n  pop r0\n"
    "  iret\n");
extern void vga_isr(void);

// Grid palette: entry i = (row r, col c) = (i >> 4, i & 15).
static uint32_t grid_colour(uint32_t i) {
    uint32_t r = i >> 4, g = i & 15u, b = 15u - (i >> 4);
    return (r << 8) | (g << 4) | b;
}

static void load_grid_palette(uint32_t shift) {
    for (uint32_t i = 0; i < 256; i++) REG_VGA_PALETTE(i) = grid_colour((i + shift) & 255u);
}

int main(void) {
    printf("\ntest_vga: VGA Phase 1 board test\n");
    REG_INT_MASK = 0;
    REG_INT_VEC(INT_SRC_VGA) = 0;

    // ── T1 ──
    REG_VGA_CTRL    = VGA_CTRL_TEST_PATTERN;
    REG_VGA_FB_BASE = 0x01000000u;
    REG_VGA_STRIDE  = 640;
    REG_VGA_BORDER  = 0xF00;
    REG_VGA_VSTART  = 40;
    check("T1 register readback",
          REG_VGA_CTRL == VGA_CTRL_TEST_PATTERN && REG_VGA_FB_BASE == 0x01000000u &&
          REG_VGA_STRIDE == 640 && REG_VGA_BORDER == 0xF00 && REG_VGA_VSTART == 40);

    // ── T2 ──
    for (uint32_t i = 0; i < 256; i++) REG_VGA_PALETTE(i) = (i * 0x9E5u) & 0xFFFu;
    int bad = 0;
    for (uint32_t i = 0; i < 256; i++) bad += REG_VGA_PALETTE(i) != ((i * 0x9E5u) & 0xFFFu);
    check("T2 palette write/readback (256 entries)", bad == 0);

    // ── T3 ──
    uint32_t f0 = VGA_STATUS_FRAME(REG_VGA_STATUS);
    wait_ms(1000);
    uint32_t frames = (VGA_STATUS_FRAME(REG_VGA_STATUS) - f0) & 0xFFFFu;
    uint32_t l0 = VGA_STATUS_LINE(REG_VGA_STATUS), l1 = l0;
    for (int i = 0; i < 1000 && l1 == l0; i++) l1 = VGA_STATUS_LINE(REG_VGA_STATUS);
    printf("   frames in 1000 ms = %lu, line %lu -> %lu\n",
           (unsigned long)frames, (unsigned long)l0, (unsigned long)l1);
    check("T3 frame rate 59-60 Hz, raster line moves", frames >= 59 && frames <= 60 && l1 != l0);

    // ── T4 ──
    REG_VGA_STATUS = VGA_STATUS_VSYNC;      // drop any stale pending
    vsyncs = 0;
    REG_INT_VEC(INT_SRC_VGA) = (uint32_t)(uintptr_t)vga_isr;
    REG_INT_MASK = 1u << INT_SRC_VGA;
    wait_ms(100);
    uint32_t off_count = vsyncs;            // IRQ_EN still off: expect 0
    REG_VGA_CTRL = VGA_CTRL_TEST_PATTERN | VGA_CTRL_VSYNC_IRQ_EN;
    wait_ms(2000);
    REG_VGA_CTRL = VGA_CTRL_TEST_PATTERN;
    uint32_t on_count = vsyncs;
    wait_ms(50);
    uint32_t pend = (REG_INT_PEND >> INT_SRC_VGA) & 1u;
    printf("   vsync IRQs: %lu with IRQ_EN off (100 ms), %lu in 2000 ms\n",
           (unsigned long)off_count, (unsigned long)on_count);
    check("T4 vsync IRQ: none while disabled", off_count == 0);
    check("T4 vsync IRQ: 118-120 in 2 s, pending clear", on_count >= 118 && on_count <= 120 && !pend);

    // ── T5 ──
    REG_VGA_FB_BASE = 0x01000000u;
    while (!(REG_VGA_STATUS & VGA_STATUS_IN_VBLANK)) {}      // let it latch
    while (REG_VGA_STATUS & VGA_STATUS_IN_VBLANK) {}         // now mid-picture
    REG_VGA_FB_BASE = 0x02000000u;
    int early = REG_VGA_FB_ACTIVE == 0x01000000u;
    while (!(REG_VGA_STATUS & VGA_STATUS_IN_VBLANK)) {}
    int late = REG_VGA_FB_ACTIVE == 0x02000000u;
    check("T5 FB_BASE latched only at vblank", early && late);

    // ── V1: border colours ──
    static const uint32_t cols[3] = {0xF00, 0x0F0, 0x00F};
    for (int i = 0; i < 3; i++) {
        REG_VGA_BORDER = cols[i];
        REG_VGA_CTRL   = 0;                 // border mode
        wait_ms(1000);
    }

    // ── V2/V3: palette grid, then palette cycling on vsync ──
    load_grid_palette(0);
    REG_VGA_CTRL = VGA_CTRL_PALETTE_VIEW | VGA_CTRL_VSYNC_IRQ_EN;
    wait_ms(2000);
    uint32_t seen = vsyncs;
    for (uint32_t step = 1; step <= 300; step++) {         // ~5 s at 59.5 Hz
        while (vsyncs == seen) {}
        seen = vsyncs;
        load_grid_palette(step);            // inside vblank: tear-free
    }
    load_grid_palette(0);
    REG_VGA_CTRL = VGA_CTRL_PALETTE_VIEW;
    REG_INT_MASK = 0;
    REG_INT_VEC(INT_SRC_VGA) = 0;

    printf("test_vga: %d passed, %d failed\n", pass, fail);
    return fail;
}
