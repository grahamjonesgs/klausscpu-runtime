# Blitter COPY corruption after DDR retiming — FPGA handoff

**Date:** 2026-06-23
**Severity:** blitter unusable for graphics (software falls back to CPU memcpy).
**Owner:** FPGA / HDL session.

## TL;DR

After the DDR clock / CDC-removal change (the same one that cut CPU stall% ~half
and sped the blit from ~39 ms → ~27 ms for a 640×480 copy), the **2D DMA blitter
`COPY` op now writes corrupted pixels** — every feature is smeared/replicated
**horizontally** across each row. The CPU memcpy path is pixel-perfect on the
same bitstream. Bisection isolates the fault to the **blitter's direct-to-DDR
data path** (or the blit-only cache FLUSH/INVALIDATE), introduced by the DDR
retiming. The CPU core retiming itself looks good (CPI/cache counters sane).

Software already has a clean fallback (memcpy via `CONFIG_KLAUSSCPU_VNC_BLITTER=n`
or the runtime presence-probe), so the GUI is usable while this is fixed.

## Evidence — the bisection

Same bitstream, three software builds of the LVGL-over-VNC app, viewed over VNC:

| flush path | wait | result |
|---|---|---|
| CPU memcpy (blitter compiled out) | n/a | **clean** — crisp text/buttons/slider |
| blitter `COPY` | busy-spin on `BUSY` | **blurred** (horizontal smear) |
| blitter `COPY` | sleep-poll (CPU yielded) | **blurred** (identical) |

Conclusions:
- **memcpy clean ⇒** DDR storage, the cache read/write path, LVGL render, the
  VNC send, and the framebuffer layout are all correct.
- **both blitter builds blurred (spin == sleep) ⇒** it is **not** a CPU/blitter
  concurrency or cache-coherency-vs-CPU race introduced by software; it is the
  blitter's own output. The sleep/concurrency change is exonerated.
- The corruption appeared with the **DDR-retimed bitstream**, and the blit time
  changed under it (39.3 → 27.1 ms), i.e. the blitter's DDR timing moved.

## Symptom detail (diagnostic)

- **Horizontal** smearing/ghosting: vertical edges become ragged and replicated
  rightward; fine detail (glyphs) is shifted/duplicated along the scanline.
- **Uniform across the whole screen**, including **static** content (the title
  is drawn once and never re-flushed, yet it is smeared) — so pixels are written
  wrong **at blit time**, not torn by a later concurrent read.
- Occurs on a **full-screen, fully-aligned** copy (see operands below): 16-byte
  aligned start, width a whole number of 128-bit words, src stride == dst stride.
  So this is **not** an unaligned-edge / byte-mask bug — it's in the main
  aligned-word copy path.

## Exactly what the software does

Full-screen flush (the common case; `vnc/blitter.h`, `vnc/display_vnc.c`):

```
REG_CACHE_CTRL   = 0x2          // FLUSH: push CPU-rendered source (VDB) to DDR (blocks)
BLIT_DST_ADDR    = &framebuffer // DDR addr, RAM based at 0x0 so CPU ptr == bus addr
BLIT_DST_STRIDE  = 1280         // bytes  (640 px * 2 B, RGB565)
BLIT_SRC_ADDR    = &vdb         // LVGL draw buffer in DDR
BLIT_SRC_STRIDE  = 1280         // bytes
BLIT_WIDTH       = 640          // pixels
BLIT_HEIGHT      = 480          // rows
BLIT_CTRL        = START | (COPY<<1)   // OP=1
  poll: while (BLIT_STATUS & BUSY) {}
BLIT_STATUS      = 0x2          // W1C DONE
REG_CACHE_CTRL   = 0x4          // INVALIDATE: drop stale cached dst so CPU/VNC re-read DDR
```

- RGB565, little-endian, 2 B/pixel. 640×480×2 = 600 KB per full-screen copy.
- For this case src_stride == dst_stride == 1280 B and the rect is 16-byte
  aligned, so every DDR access is a full aligned 128-bit word — no partial-word
  byte-masking is exercised, yet it still corrupts.

## Ranked suspects (HDL)

1. **Blitter read-data capture timing at the new DDR clock.** The CDC removal
   most likely changed the read-latency / `read_valid` handshake on the
   blitter's DDR master. If the blitter latches returned read data one cycle
   early/late, each beat's pixels land at the wrong column → exactly this
   horizontal shift/replication. This is the same class of issue the MMIO map
   flags for the MMIO read-return FF (`r_mmio_read_data`) — check the analogous
   register/valid alignment on the blitter↔DDR read path.
2. **Write-side beat / byte-enable phase.** If write data and byte-enables are no
   longer phase-aligned to the DDR write strobe after retiming, bytes get
   duplicated/dropped within a 128-bit word → horizontal artifacts. (Even though
   masking isn't *needed* on aligned words, the write datapath still runs.)
3. **Cache FLUSH/INVALIDATE at the new clock** (`0xF005`). Only the blit path
   uses these (memcpy doesn't), so a regression here is consistent with
   "memcpy clean, blitter broken." If FLUSH reports done before the source VDB
   has actually drained to DDR, the blitter reads partially-stale source. Less
   likely to produce *uniform* horizontal smear (would look more like stale
   blocks), so ranked below the datapath items — but cheap to verify.
4. **CPU/blitter DDR arbitration at the new clock.** Lower likelihood: in the
   busy-spin build the CPU is hammering MMIO (not DDR) during the blit and it
   still corrupts, so contention isn't required to trigger it.

## Suggested HW repro / self-test (no LVGL needed)

Deterministic, CPU-checkable, isolates the blitter from all software:

1. CPU writes a known pattern into DDR buffer A — a **1-px-wide vertical-line
   grid** (e.g. every 8th column set) makes horizontal misplacement obvious.
2. `FLUSH`, then blitter `COPY` A→B (same strides/width as above), wait `BUSY`,
   `INVALIDATE`.
3. CPU reads back B and compares to A. A horizontal-shift error shows as the
   lines landing in the wrong columns / smearing.
4. **Confirm the correlation:** if feasible, rebuild the blitter against the
   *old* DDR timing — if the copy goes clean, the retiming is conclusively the
   cause. Also sweep `BLIT_CYCLES` (it's ~8.8 cyc/px now; the retiming shaved
   latency but may have skewed a capture phase).

A `FILL` self-test (no read path) vs `COPY` (read + write) also splits read-side
vs write-side: if `FILL` is clean but `COPY` smears, it's the **read** capture
(suspect #1).

## Software status / re-enable

- Fallback is automatic: the driver probes the blitter at init and falls back to
  memcpy if absent; `CONFIG_KLAUSSCPU_VNC_BLITTER=n` forces memcpy at build time.
  Builds on hand: `build_lvgl_mc` (memcpy, clean), `build_lvgl_spin` (blitter,
  busy-spin), `build_lvgl` (blitter, sleep).
- Once the HDL fix lands, flip `CONFIG_KLAUSSCPU_VNC_BLITTER=y` (default) and
  re-run the LVGL render benchmark — the `copy = blit + cache` split line will
  confirm correctness (and the blit µs).
- Unrelated but open: the blitter still **isn't bursting** (~8.8 cyc/px ≈ one DDR
  access at a time). Burst transfers remain the separate copy-throughput lever
  (see `blitter-fpga-handoff.md`); fix the corruption first.
