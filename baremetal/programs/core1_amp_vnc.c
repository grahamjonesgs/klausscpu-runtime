/*
 * core1_amp_vnc.c — AMP P4a demo host (baremetal, core 1).
 *
 * Boots core 2 with the embedded lwIP+VNC image (like core1_amp_host), then
 * plays "renderer": owns a 320x200 RGB565 framebuffer in shared DDR, draws a
 * colour-bar test pattern with a bouncing box, and after every frame does
 * the producer side of the AMP contract:
 *     draw -> CACHE FLUSH -> descriptor {seq++, dirty rect} (same flush)
 * Core 2 serves VNC clients from that framebuffer.  Core 1 also forwards
 * core 2's console to the UART and prints its own frame/flush stats.
 *
 * Measured from the LAN with perf/amp_p1/vnc_probe.py: fps + bytes/update
 * with hextile vs raw — the P4a numbers.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../../mmio.h"
#include "../../amp/amp_proto.h"
#include "core2_image.h"

#define FB_W        320
#define FB_H        200
#define FB_ADDR     0x01000000u         /* well above core 2's 128 KB shadow */
#define FRAME_MS    20                  /* producer cap: 50 fps              */
#define BOX         32

static uint16_t *fb = (uint16_t *)(uintptr_t)FB_ADDR;

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

static void cache_flush(void)
{
    REG_CACHE_CTRL = CACHE_CTRL_FLUSH;
    while (REG_CACHE_STATUS & 1u) {}
}
static void cache_invalidate(void)
{
    REG_CACHE_CTRL = CACHE_CTRL_INVALIDATE;
    while (REG_CACHE_STATUS & 1u) {}
}

static const uint16_t bars[8] = {
    0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000
};

static uint16_t pattern(int x, int y)
{
    if (y < (FB_H * 2) / 3) return bars[(x * 8) / FB_W];
    uint8_t g = (uint8_t)((x * 255) / (FB_W - 1));
    return rgb(g, g, g);
}

static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            fb[yy * FB_W + xx] = c;
}
static void restore_rect(int x, int y, int w, int h)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            fb[yy * FB_W + xx] = pattern(xx, yy);
}

static void drain_log(void)
{
    for (int i = 0; i < 64; i++) {
        uint64_t v = REG_C2_LOG;
        if (!(v & 0x100u)) break;
        putchar((int)(v & 0xFFu));
        REG_C2_LOG = 0;
    }
    fflush(stdout);
}

int main(void)
{
    printf("amp vnc host: core-2 image %u bytes -> 0x%08x\n",
           (unsigned)core2_image_len, (unsigned)C2_TEXT_ENTRY);
    REG_C2_CTRL = 0;
    /* Drain any stale core-2 console backlog from a previous program — the
     * 512 B log FIFO survives core-1 reloads and would otherwise replay old
     * lines (including an old "listening" marker) into this session. */
    for (int i = 0; i < 600; i++) {
        if (!(REG_C2_LOG & 0x100u)) break;
        REG_C2_LOG = 0;
    }
    memcpy((void *)(uintptr_t)C2_TEXT_ENTRY, core2_image, core2_image_len);

    /* Framebuffer + descriptor (published before core 2 starts). */
    for (int y = 0; y < FB_H; y++)
        for (int x = 0; x < FB_W; x++) fb[y * FB_W + x] = pattern(x, y);
    amp_fb_desc_t *d = AMP_FB_DESC;
    memset(d, 0, sizeof(*d));
    d->fb_base = FB_ADDR; d->stride = FB_W * 2;
    d->width = FB_W; d->height = FB_H;
    d->dx = 0; d->dy = 0; d->dw = FB_W; d->dh = FB_H;
    d->seq = 1;
    d->magic = AMP_FB_MAGIC;
    cache_flush();

    REG_C2_ETH_OWNER = 1;
    REG_C2_START_PC  = C2_TEXT_ENTRY;
    REG_C2_CTRL      = 1;
    printf("amp vnc host: core 2 running; fb %dx%d @0x%08x\n", FB_W, FB_H, FB_ADDR);

    int bx = 0, by = 0, dx = 3, dy = 2;
    uint64_t next = REG_CLOCK_MS + FRAME_MS;
    uint64_t stat_at = REG_CLOCK_MS + 5000;
    uint32_t frames = 0, flush_ms_acc = 0;

    for (;;) {
        drain_log();
        if (REG_CLOCK_MS < next) continue;
        next += FRAME_MS;

        int ox = bx, oy = by;
        bx += dx; by += dy;
        if (bx <= 0 || bx >= FB_W - BOX) dx = -dx;
        if (by <= 0 || by >= FB_H - BOX) dy = -dy;
        restore_rect(ox, oy, BOX, BOX);
        fill_rect(bx, by, BOX, BOX, 0xFFFF);
        fill_rect(bx + 2, by + 2, BOX - 4, BOX - 4, 0x0000);

        int x0 = ox < bx ? ox : bx, y0 = oy < by ? oy : by;
        int x1 = (ox > bx ? ox : bx) + BOX, y1 = (oy > by ? oy : by) + BOX;
        d->dx = (uint16_t)x0; d->dy = (uint16_t)y0;
        d->dw = (uint16_t)(x1 - x0); d->dh = (uint16_t)(y1 - y0);
        d->seq++;
        uint64_t t0 = REG_CLOCK_MS;
        cache_flush();                       /* publishes pixels + descriptor */
        flush_ms_acc += (uint32_t)(REG_CLOCK_MS - t0);
        frames++;

        if (REG_CLOCK_MS >= stat_at) {
            stat_at += 5000;
            cache_invalidate();              /* read core 2's stats fields   */
            /* <=2 args per printf: multi-arg lines garble on this platform */
            printf("host: frames=%u flush=%u ms/frame\n",
                   frames, frames ? flush_ms_acc / frames : 0);
            printf("  c2: upd=%u ack=%u\n", (unsigned)d->updates, (unsigned)d->ack);
            printf("  c2: clients=%u\n", (unsigned)d->clients);
            frames = 0; flush_ms_acc = 0;
        }
    }
    return 0;
}
