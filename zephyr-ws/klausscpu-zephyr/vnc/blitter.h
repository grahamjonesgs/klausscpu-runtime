/*
 * blitter.h — KlaussCPU 2D DMA blitter + cache maintenance (MMIO).
 *
 * The blitter (base 0xF00E_0000) is a bus-master DMA engine that fills / copies
 * / alpha-blends RGB565 rectangles directly in DDR, concurrently with the CPU.
 * We use its COPY op to offload the LVGL flush (draw-buffer -> framebuffer), so
 * frame time tends to max(render, copy) instead of render + copy.  See
 * docs/MMIO_MAP.md ("2D DMA blitter").
 *
 * Addressing: RAM is unified DDR based at 0x0 (soc/.../linker.ld), so a CPU
 * pointer is already the blitter's bus byte address — (uint32_t)(uintptr_t)ptr
 * drops straight into BLIT_*_ADDR, no translation.
 *
 * Cache coherency: the CPU goes through a write-back cache; the blitter hits
 * DDR directly.  Every blit is bracketed with a whole-cache FLUSH before (push
 * the CPU's freshly-rendered source down to DDR) and INVALIDATE after (drop
 * stale cached copies of the destination so the CPU / VNC reader re-reads the
 * blitter's output).  Both are blocking, whole-cache walks; region-scoped
 * maintenance is a future hardware phase.
 */
#ifndef KLAUSSCPU_VNC_BLITTER_H_
#define KLAUSSCPU_VNC_BLITTER_H_

#include <stdint.h>
#include <stdbool.h>

/* ── Blitter register file (0xF00E_0000) — all accessed as 64-bit ───────── */
#define BLIT_BASE          0xF00E0000u
#define BLIT_R64(o)        (*(volatile uint64_t *)(unsigned long)(BLIT_BASE + (o)))

#define BLIT_CTRL          0x00   /* W : [0]START(self-clear) [3:1]OP [4]IRQ_EN */
#define BLIT_STATUS        0x08   /* RW: [0]BUSY(ro) [1]DONE(w1c) */
#define BLIT_DST_ADDR      0x10
#define BLIT_DST_STRIDE    0x18
#define BLIT_SRC_ADDR      0x20
#define BLIT_SRC_STRIDE    0x28
#define BLIT_WIDTH         0x30   /* pixels */
#define BLIT_HEIGHT        0x38   /* rows */
#define BLIT_COLOR         0x40   /* RGB565 fill/foreground */
#define BLIT_ALPHA         0x48   /* global alpha 0..255 */
#define BLIT_MASK_ADDR     0x50
#define BLIT_MASK_STRIDE   0x58
#define BLIT_CYCLES        0x60   /* R: i_Clk cycles of the last completed blit */

#define BLIT_OP_FILL       0u
#define BLIT_OP_COPY       1u
#define BLIT_OP_FILL_BLEND 2u
#define BLIT_OP_COPY_BLEND 3u
#define BLIT_OP_MASK_BLEND 4u

#define BLIT_CTRL_START    (1u << 0)
#define BLIT_STATUS_BUSY   (1u << 0)
#define BLIT_STATUS_DONE   (1u << 1)

/* ── Cache controller (0xF005_0000) — whole-cache maintenance ───────────── */
#define CACHE_CTRL         (*(volatile uint32_t *)(unsigned long)0xF0050000u)
#define CACHE_CTRL_CLEAR   (1u << 0)   /* zero perf counters */
#define CACHE_CTRL_FLUSH   (1u << 1)   /* write back dirty lines, keep valid (blocks) */
#define CACHE_CTRL_INVAL   (1u << 2)   /* flush dirty, then drop every line (blocks) */

/*
 * Probe whether a blitter is present in the loaded bitstream: a RW operand
 * register round-trips on real hardware, whereas an undecoded / reserved MMIO
 * region reads back 0 and drops writes.  Call once at init (no blit in flight);
 * it restores the register it scribbles on.
 */
static inline bool blit_probe(void)
{
	uint64_t saved = BLIT_R64(BLIT_DST_STRIDE);
	bool ok;

	BLIT_R64(BLIT_DST_STRIDE) = 0xA5A5A5A5u;
	ok = ((uint32_t)BLIT_R64(BLIT_DST_STRIDE) == 0xA5A5A5A5u);
	BLIT_R64(BLIT_DST_STRIDE) = 0x5A5A5A5Au;
	ok = ok && ((uint32_t)BLIT_R64(BLIT_DST_STRIDE) == 0x5A5A5A5Au);

	BLIT_R64(BLIT_DST_STRIDE) = saved;
	return ok;
}

/*
 * Async RGB565 rectangular-copy primitives.  Split so the caller can sleep or
 * yield between polls instead of busy-spinning — on this core the CPU runs the
 * blit autonomously, so a spin needlessly starves lower-priority threads (e.g.
 * the VNC send + network threads) for the whole DMA.
 *
 * Sequence: blit_start_copy() -> poll blit_busy() (sleeping between) -> read
 * blit_last_cycles() if wanted -> blit_finish().  dst/src are DDR byte
 * addresses (== CPU pointers), strides in bytes, w/h in pixels.  No hardware
 * clipping — pass a pre-clipped rect.
 */
static inline void blit_start_copy(uint32_t dst, uint32_t dst_stride,
				   uint32_t src, uint32_t src_stride,
				   uint16_t w, uint16_t h)
{
	CACHE_CTRL = CACHE_CTRL_FLUSH;           /* push rendered src to DDR (blocks) */

	BLIT_R64(BLIT_DST_ADDR)   = dst;
	BLIT_R64(BLIT_DST_STRIDE) = dst_stride;
	BLIT_R64(BLIT_SRC_ADDR)   = src;
	BLIT_R64(BLIT_SRC_STRIDE) = src_stride;
	BLIT_R64(BLIT_WIDTH)      = w;
	BLIT_R64(BLIT_HEIGHT)     = h;
	BLIT_R64(BLIT_CTRL)       = BLIT_CTRL_START | (BLIT_OP_COPY << 1);
}

static inline bool blit_busy(void)
{
	return (BLIT_R64(BLIT_STATUS) & BLIT_STATUS_BUSY) != 0;
}

/* Acknowledge DONE (W1C) and drop the CPU's now-stale cached copies of the
 * destination so subsequent reads (CPU / VNC) see the blitter's output. */
static inline void blit_finish(void)
{
	BLIT_R64(BLIT_STATUS) = BLIT_STATUS_DONE;        /* W1C */
	CACHE_CTRL = CACHE_CTRL_INVAL;           /* drop stale dst copies (blocks) */
}

/* Convenience synchronous (busy-spin) copy — for callers outside thread context
 * that cannot sleep.  Thread-context callers should use the split primitives
 * above with a sleeping poll. */
static inline void blit_copy_rect(uint32_t dst, uint32_t dst_stride,
				  uint32_t src, uint32_t src_stride,
				  uint16_t w, uint16_t h)
{
	blit_start_copy(dst, dst_stride, src, src_stride, w, h);
	while (blit_busy()) {
		/* spin */
	}
	blit_finish();
}

/* Cycle count of the most recently completed blit (profiling). */
static inline uint32_t blit_last_cycles(void)
{
	return (uint32_t)BLIT_R64(BLIT_CYCLES);
}

#endif /* KLAUSSCPU_VNC_BLITTER_H_ */
