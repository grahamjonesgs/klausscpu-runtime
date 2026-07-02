/* blit_selftest — deterministic blitter COPY/FILL diagnostic + geometry sweep.
 *
 * The 64x8 aligned case is clean, so the post-DDR-retiming corruption is
 * geometry/scale-triggered. This sweeps width, height and stride independently
 * (fixed DDR buffers at realistic addresses) so one run isolates the trigger;
 * the position-encoded source (pixel = (x&0xFF)|((y&0xFF)<<8)) makes any
 * misplacement legible in the row dump.
 *
 * Sequence mirrors the driver: CPU fill + cache FLUSH (push to DDR), blitter op,
 * poll BUSY, ack DONE, cache INVALIDATE (drop stale dst), read back.
 */
#include <stdint.h>
#include <stdio.h>
#include "../../mmio.h"

#define BLIT_BASE        0xF00E0000u
#define BR(o)            (*(volatile uint64_t *)(unsigned long)(BLIT_BASE + (o)))
#define BLIT_CTRL        0x00u
#define BLIT_STATUS      0x08u
#define BLIT_DST_ADDR    0x10u
#define BLIT_DST_STRIDE  0x18u
#define BLIT_SRC_ADDR    0x20u
#define BLIT_SRC_STRIDE  0x28u
#define BLIT_WIDTH       0x30u
#define BLIT_HEIGHT      0x38u
#define BLIT_COLOR       0x40u
#define BLIT_CYCLES      0x60u
#define ST_BUSY          1u
#define ST_DONE          2u
#define OP_FILL          0u
#define OP_COPY          1u
#define CACHE_FLUSH      0x2u
#define CACHE_INVAL      0x4u

/* fixed DDR buffers, well clear of program (low) and stack (~128 MiB top) */
#define A_ADDR  0x02000000u    /* src */
#define B_ADDR  0x03000000u    /* dst */
#define AP      ((volatile uint16_t *)(unsigned long)A_ADDR)
#define BP      ((volatile uint16_t *)(unsigned long)B_ADDR)

static inline uint16_t enc(unsigned x, unsigned y) {
    return (uint16_t)((x & 0xFFu) | ((y & 0xFFu) << 8));
}

static void run_blit(unsigned op, uint64_t dst, uint64_t src,
                     unsigned w, unsigned h, unsigned stride, uint16_t color) {
    BR(BLIT_DST_ADDR)   = dst;
    BR(BLIT_DST_STRIDE) = stride;
    BR(BLIT_SRC_ADDR)   = src;
    BR(BLIT_SRC_STRIDE) = stride;
    BR(BLIT_WIDTH)      = w;
    BR(BLIT_HEIGHT)     = h;
    BR(BLIT_COLOR)      = color;
    BR(BLIT_CTRL)       = 1u | (op << 1);
    while (BR(BLIT_STATUS) & ST_BUSY) { }
    BR(BLIT_STATUS)     = ST_DONE;
}

/* COPY test: returns #wrong; dumps the head of row 2 on failure. */
static unsigned copy_test(const char *name, unsigned w, unsigned h, unsigned stride) {
    unsigned pitch = stride / 2u;                 /* uint16 per row */
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) AP[y * pitch + x] = enc(x, y);
    REG_CACHE_CTRL = CACHE_FLUSH;
    run_blit(OP_COPY, B_ADDR, A_ADDR, w, h, stride, 0);
    REG_CACHE_CTRL = CACHE_INVAL;

    unsigned bad = 0, first_x = 0, first_y = 0; uint16_t fg = 0, fw = 0;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            uint16_t got = BP[y * pitch + x], want = enc(x, y);
            if (got != want) {
                if (!bad) { first_x = x; first_y = y; fg = got; fw = want; }
                bad++;
            }
        }
    printf("COPY %-9s %ux%u str%u: %s %u/%u wrong CYC=%lu",
           name, w, h, stride, bad ? "FAIL" : "PASS", bad, w * h,
           (unsigned long)BR(BLIT_CYCLES));
    if (bad) {
        printf("  first(%u,%u) got=%04x want=%04x\n", first_x, first_y, fg, fw);
        unsigned dn = w < 24 ? w : 24, ry = h > 2 ? 2 : 0;
        printf("  r%u got:", ry);
        for (unsigned x = 0; x < dn; x++) printf(" %04x", BP[ry * pitch + x]);
        printf("\n  r%u exp:", ry);
        for (unsigned x = 0; x < dn; x++) printf(" %04x", enc(x, ry));
        printf("\n");
    } else printf("\n");
    return bad;
}

/* Thrash buffer: bigger than L1 so striding reads always miss → DDR traffic. */
#define THRASH_ADDR 0x04000000u
#define THRASH_N    (256u * 1024u)         /* 512 KiB of uint16 */
#define TP          ((volatile uint16_t *)(unsigned long)THRASH_ADDR)
static volatile uint32_t g_sink;
static volatile uint16_t g_warm;   /* sink for the flush-wait cache-missing read */

/* COPY with the CPU hammering DDR (cache misses) during the blit — models the
 * LVGL timer-ISR DDR traffic that the isolated sweep lacks. */
static unsigned copy_test_contended(const char *name, unsigned w, unsigned h, unsigned stride) {
    unsigned pitch = stride / 2u;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) AP[y * pitch + x] = enc(x, y);
    REG_CACHE_CTRL = CACHE_FLUSH;

    BR(BLIT_DST_ADDR)   = B_ADDR;  BR(BLIT_DST_STRIDE) = stride;
    BR(BLIT_SRC_ADDR)   = A_ADDR;  BR(BLIT_SRC_STRIDE) = stride;
    BR(BLIT_WIDTH)      = w;        BR(BLIT_HEIGHT)     = h;
    BR(BLIT_CTRL)       = 1u | (OP_COPY << 1);
    /* contended spin: continuous cache-missing DDR reads while the blitter runs */
    unsigned idx = 0; uint32_t acc = 0;
    while (BR(BLIT_STATUS) & ST_BUSY) {
        acc += TP[idx];                        /* read miss  → DDR read   */
        TP[idx] = (uint16_t)(acc + idx);       /* dirty line → DDR evict  */
        idx += 32u; if (idx >= THRASH_N) idx = 0;
    }
    g_sink = acc;
    BR(BLIT_STATUS) = ST_DONE;
    REG_CACHE_CTRL = CACHE_INVAL;

    unsigned bad = 0, fx = 0, fy = 0; uint16_t fg = 0, fw = 0;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            uint16_t got = BP[y * pitch + x], want = enc(x, y);
            if (got != want) { if (!bad){fx=x;fy=y;fg=got;fw=want;} bad++; }
        }
    printf("COPY %-9s %ux%u str%u: %s %u/%u wrong CYC=%lu",
           name, w, h, stride, bad ? "FAIL" : "PASS", bad, w * h,
           (unsigned long)BR(BLIT_CYCLES));
    if (bad) {
        printf("  first(%u,%u) got=%04x want=%04x\n", fx, fy, fg, fw);
        unsigned dn = w < 24 ? w : 24, ry = h > 2 ? 2 : 0;
        printf("  r%u got:", ry);
        for (unsigned x = 0; x < dn; x++) printf(" %04x", BP[ry * pitch + x]);
        printf("\n  r%u exp:", ry);
        for (unsigned x = 0; x < dn; x++) printf(" %04x", enc(x, ry));
        printf("\n");
    } else printf("\n");
    return bad;
}

/* fully parameterized COPY at arbitrary addresses + independent strides */
static unsigned copy_at(const char *name, unsigned w, unsigned h,
                        unsigned src_str, unsigned dst_str,
                        uint32_t ba, uint32_t bb) {
    volatile uint16_t *a = (volatile uint16_t *)(unsigned long)ba;
    volatile uint16_t *b = (volatile uint16_t *)(unsigned long)bb;
    unsigned sp = src_str / 2u, dp = dst_str / 2u;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) a[y * sp + x] = enc(x, y);
    REG_CACHE_CTRL = CACHE_FLUSH;
    BR(BLIT_DST_ADDR) = bb; BR(BLIT_DST_STRIDE) = dst_str;
    BR(BLIT_SRC_ADDR) = ba; BR(BLIT_SRC_STRIDE) = src_str;
    BR(BLIT_WIDTH) = w;     BR(BLIT_HEIGHT) = h;
    BR(BLIT_CTRL) = 1u | (OP_COPY << 1);
    while (BR(BLIT_STATUS) & ST_BUSY) { }
    BR(BLIT_STATUS) = ST_DONE;
    REG_CACHE_CTRL = CACHE_INVAL;
    unsigned bad = 0, fx = 0, fy = 0; uint16_t fg = 0, fw = 0;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            uint16_t got = b[y * dp + x], want = enc(x, y);
            if (got != want) { if (!bad){fx=x;fy=y;fg=got;fw=want;} bad++; }
        }
    printf("COPY %-8s %ux%u s%u/d%u @%08x: %s %u wrong CYC=%lu",
           name, w, h, src_str, dst_str, (unsigned)bb,
           bad ? "FAIL" : "PASS", bad, (unsigned long)BR(BLIT_CYCLES));
    if (bad) {
        printf("  first(%u,%u) got=%04x want=%04x\n", fx, fy, fg, fw);
        unsigned dn = w < 24 ? w : 24;
        printf("  r0 got:");
        for (unsigned x = 0; x < dn; x++) printf(" %04x", b[x]);
        printf("\n  r0 exp:");
        for (unsigned x = 0; x < dn; x++) printf(" %04x", enc(x, 0));
        printf("\n");
    } else printf("\n");
    return bad;
}

/* ---- software-vs-hardware discriminator -------------------------------------
 * Runs the unaligned COPY that corrupts, with two probes:
 *   flushwait=1 : after CACHE_FLUSH, do a cache-MISSING read (far addr) before
 *                 triggering the blit. Per mem_read_write.v the CPU's next cached
 *                 access stalls until the flush walk completes, so this guarantees
 *                 src is committed to DDR before the blitter reads it. If this
 *                 alone makes read1 PASS -> the flush->blit race was the cause (SW).
 *   read2       : after the first readback, spin-delay + INVAL + re-read. If read1
 *                 FAILs but read2 PASSes -> the blitter's write committed late and
 *                 software read too early (completion race, SW fix).
 * If read1 AND read2 both FAIL even with flushwait=1 -> the DDR genuinely holds
 * wrong data -> hardware data path -> ILA.
 * ---------------------------------------------------------------------------- */
#define MISS_ADDR 0x05000000u   /* far DDR addr, untouched by any buffer/thrash */
static void copy_diag(const char *name, unsigned w, unsigned h, unsigned stride,
                      uint32_t ba, uint32_t bb, int flushwait) {
    volatile uint16_t *a = (volatile uint16_t *)(unsigned long)ba;
    volatile uint16_t *b = (volatile uint16_t *)(unsigned long)bb;
    unsigned p = stride / 2u;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) a[y * p + x] = enc(x, y);
    REG_CACHE_CTRL = CACHE_FLUSH;
    if (flushwait)
        g_warm = *(volatile uint16_t *)(unsigned long)MISS_ADDR; /* miss: stalls till flush done */

    BR(BLIT_DST_ADDR) = bb; BR(BLIT_DST_STRIDE) = stride;
    BR(BLIT_SRC_ADDR) = ba; BR(BLIT_SRC_STRIDE) = stride;
    BR(BLIT_WIDTH) = w;     BR(BLIT_HEIGHT) = h;
    BR(BLIT_CTRL) = 1u | (OP_COPY << 1);
    while (BR(BLIT_STATUS) & ST_BUSY) { }
    BR(BLIT_STATUS) = ST_DONE;

    REG_CACHE_CTRL = CACHE_INVAL;                            /* read1 (immediate) */
    unsigned bad1 = 0, fx1 = 0; uint16_t fg1 = 0, fw1 = 0;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            uint16_t got = b[y * p + x], want = enc(x, y);
            if (got != want) { if (!bad1) { fx1 = x; fg1 = got; fw1 = want; } bad1++; }
        }

    for (volatile unsigned d = 0; d < 300000u; d++) { }      /* let any in-flight write commit */
    REG_CACHE_CTRL = CACHE_INVAL;                            /* read2 (delayed) */
    unsigned bad2 = 0;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++)
            if (b[y * p + x] != enc(x, y)) bad2++;

    printf("DIAG %-6s @%08x fwait=%d: read1 %s %u  read2(delayed) %s %u",
           name, (unsigned)bb, flushwait,
           bad1 ? "FAIL" : "PASS", bad1, bad2 ? "FAIL" : "PASS", bad2);
    if (bad1) printf("  first(x=%u) got=%04x want=%04x", fx1, fg1, fw1);
    printf("\n");
}

int main(void) {
    printf("\n=== blit_selftest sweep ===\n");

    /* write-path control at scale */
    {
        unsigned w = 640, h = 480, pitch = 1280 / 2;
        run_blit(OP_FILL, B_ADDR, 0, w, h, 1280, 0xABCD);
        REG_CACHE_CTRL = CACHE_INVAL;
        unsigned bad = 0;
        for (unsigned y = 0; y < h; y++)
            for (unsigned x = 0; x < w; x++) if (BP[y * pitch + x] != 0xABCD) bad++;
        printf("FILL  640x480   str1280: %s %u/%u wrong CYC=%lu\n",
               bad ? "FAIL" : "PASS", bad, w * h, (unsigned long)BR(BLIT_CYCLES));
    }

    /* COPY geometry sweep — isolates width / height / stride */
    copy_test("ctrl",   64,  8,  128);     /* known-good control            */
    copy_test("wide",  640,  8, 1280);     /* full width + real stride      */
    copy_test("tall",   64, 480,  128);    /* many rows, narrow             */
    copy_test("strideW",64,  8, 1280);     /* narrow data, big row-gap      */
    copy_test("real",  640, 480, 1280);    /* exact LVGL full-screen case   */

    /* the discriminator: same copy, but CPU pounds DDR during the blit */
    copy_test_contended("real+ddr", 640, 480, 1280);
    copy_test_contended("wide+ddr", 640,   8, 1280);

    /* UNALIGNED-DST is the trigger. Sweep the byte offset within a 16-byte word
       to characterize: aligned control, then +2..+14, plus an unaligned SRC. */
    copy_at("dst+0",  320, 8, 1280, 1280, 0x02000000u, 0x03000000u);  /* control */
    copy_at("dst+2",  320, 8, 1280, 1280, 0x02000000u, 0x03000002u);
    copy_at("dst+4",  320, 8, 1280, 1280, 0x02000000u, 0x03000004u);
    copy_at("dst+6",  320, 8, 1280, 1280, 0x02000000u, 0x03000006u);
    copy_at("dst+8",  320, 8, 1280, 1280, 0x02000000u, 0x03000008u);
    copy_at("dst+10", 320, 8, 1280, 1280, 0x02000000u, 0x0300000Au);
    copy_at("dst+14", 320, 8, 1280, 1280, 0x02000000u, 0x0300000Eu);
    copy_at("src+2",  320, 8, 1280, 1280, 0x02000002u, 0x03000000u);  /* unaligned SRC */

    /* ===== software-vs-hardware discriminators on the unaligned trigger ===== */
    printf("--- discriminators on unaligned dst+2 ---\n");
    copy_diag("base",  320, 8, 1280, 0x02000000u, 0x03000002u, 0);  /* reproduce, no flush-wait */
    copy_diag("fwait", 320, 8, 1280, 0x02000000u, 0x03000002u, 1);  /* + flush-wait before blit */
    printf("legend: fwait read1 PASS => flush->blit race (SW fix). "
           "base read1 FAIL & read2 PASS => read-too-early (SW fix). "
           "both FAIL => hardware data path (=> ILA).\n");

    printf("=== blit_selftest done ===\n");
    return 0;
}
