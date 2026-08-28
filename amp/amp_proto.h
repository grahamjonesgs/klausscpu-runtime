/*
 * amp_proto.h — THE contract between core 1 and AMP core 2 (compiled into
 * both images).  Both cores see DDR at the same addresses, so shared state
 * is a plain struct at an agreed address.  Cache rule: core 1 writes fields
 * then CACHE-FLUSHes (the whole descriptor sits in one 32 B line, so a
 * single flush publishes it atomically); core 1 INVALIDATEs before reading
 * fields core 2 wrote.  Core 2's accesses are uncached — no maintenance.
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
} amp_fb_desc_t;                  /* 64 B = exactly two cache lines          */

#define AMP_FB_DESC  ((amp_fb_desc_t *)(uintptr_t)AMP_SHM_BASE)

#endif /* AMP_PROTO_H */
