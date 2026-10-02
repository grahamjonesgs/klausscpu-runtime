/*
 * amp_proto.h — THE contract between core 1 and AMP core 2 (compiled into
 * both images).  Both cores see DDR at the same addresses, so shared state
 * is a plain struct at an agreed address.  Cache rule: core 1 writes fields
 * then CACHE-FLUSHes (the whole descriptor sits in one 32 B line, so a
 * single flush publishes it atomically); core 1 drops its cached copy before
 * reading fields core 2 wrote (INVALIDATE, or the eviction read in
 * amp_host.c).  Core 2's accesses are uncached — no maintenance.
 *
 * Shared block: 0x07D0_0000 (below the core-2 text window at 0x07E0_0000,
 * above anything core 1's baremetal/Zephyr images or heaps reach).
 */
#ifndef AMP_PROTO_H
#define AMP_PROTO_H
#include <stdint.h>

#define AMP_SHM_BASE   0x07D00000u
#define AMP_FB_MAGIC   0x414D5046u          /* 'AMPF' */

/* Frame descriptor: core 1 = producer (renderer), core 2 = consumer (VNC).
 *
 * LAYOUT RULE (learned the hard way in P4a): fields written by DIFFERENT
 * cores must never share a 32 B cache line.  Core 1's FLUSH writes back its
 * whole line, so a consumer field in the producer's line gets clobbered
 * with core 1's stale copy on every frame (that was `ack` reading 0).
 *   line 0 (0..31)  : producer-written (magic, seq, geometry, dirty rect)
 *   line 1 (32..63) : consumer-written (ack + stats)                      */
typedef struct {
    /* ---- line 0: written by core 1 only ---- */
    volatile uint32_t magic;      /* AMP_FB_MAGIC once fb_base/geometry valid */
    volatile uint32_t seq;        /* core 1: ++ after each posted frame       */
    volatile uint32_t fb_base;    /* DDR byte address of pixel (0,0), RGB565 LE
                                     — MUST be >= C2_LRAM_SIZE (core 2 can't
                                     see DDR below its local BRAM shadow)     */
    volatile uint32_t stride;     /* bytes per row                            */
    volatile uint16_t width;      /* pixels                                   */
    volatile uint16_t height;
    volatile uint16_t dx, dy;     /* dirty rect of the posted frame (px)      */
    volatile uint16_t dw, dh;
    volatile uint32_t pad0;
    /* ---- line 1: written by core 2 only ---- */
    volatile uint32_t ack;        /* core 2: seq of the last frame it served  */
    volatile uint32_t clients;    /* core 2: VNC clients connected (stats)    */
    volatile uint32_t updates;    /* core 2: FramebufferUpdates sent (stats)  */
    volatile uint32_t bytes_lo;   /* core 2: bytes sent (stats)               */
    volatile uint32_t prof_enc_ms;/* core 2: ms in encode (fb fetch + tiles)  */
    volatile uint32_t prof_tx_ms; /* core 2: ms in tcp_write/tcp_output       */
    volatile uint32_t prof_rx_ms; /* core 2: ms in ethernetif_input+timeouts  */
    volatile uint32_t pad1;
    /* ---- line 2: written by core 1 only — optional 8-bit indexed source ----
     * A producer that renders palette-indexed pixels (doom) publishes them
     * here so core 2 can serve VNC clients in 8-bit colour-map format (half
     * the bytes, no conversion anywhere).  idx_base == 0: not available.     */
    volatile uint32_t idx_base;   /* DDR address of index (0,0), 1 B/pixel,
                                     same width/height/dirty rect as fb      */
    volatile uint32_t idx_stride; /* bytes per row                            */
    volatile uint32_t pal_base;   /* DDR address of 256 x u32 0x00RRGGBB      */
    volatile uint32_t pal_seq;    /* core 1: ++ whenever the palette changes  */
    volatile uint32_t pad2[4];
    /* ---- line 3: written by core 2 only ----
     * want_rgb565 != 0 while a client that needs the RGB565 framebuffer is
     * connected; a producer with an indexed source may skip filling fb
     * otherwise.  Core 1 never writes this line, so its cached copy stays
     * clean and can be dropped by eviction (see amp_host.c).                */
    volatile uint32_t want_rgb565;
    volatile uint32_t pad3[7];
} amp_fb_desc_t;                  /* 128 B = exactly four cache lines        */

#define AMP_FB_DESC  ((amp_fb_desc_t *)(uintptr_t)AMP_SHM_BASE)

/* 256 x u32 palette for the indexed source, right after the descriptor. */
#define AMP_PALETTE  ((volatile uint32_t *)(uintptr_t)(AMP_SHM_BASE + 0x100u))

#endif /* AMP_PROTO_H */
