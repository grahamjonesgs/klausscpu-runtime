# Streaming a framebuffer over VNC on KlaussCPU — performance notes

Generalised findings from driving the in-RAM framebuffer + RFB server hard (the
`apps/doom` case study). Read this before trying to make anything stream
"smoothly" over the VNC server — several intuitions are wrong on this platform.

## Bottom line

On the KlaussCPU soft core, full-screen VNC streaming is **CPU-bound on a single
core, not bandwidth-bound.** Each displayed frame costs, serially on one CPU:

```
render  +  pixel-convert  +  network-send   (all compete for the one core)
```

For a 320×200 RGB565 frame we measured roughly `85 + 90 + 125 ms ≈ 300 ms`, i.e.
a **~3 fps hardware ceiling**, ~1–2 fps once the client's request round-trip is
included. Going faster means **doing less CPU work per frame** (or offloading to
the FPGA fabric) — not tuning the network.

## Measure first — and beware the local tools

We "optimised the network" three times (send batching, TCP window) before
measuring, and none of it helped: the real hot loop was **pixel conversion**.
Always instrument per-stage (render / convert / send) before changing anything.

Two platform gotchas that corrupted our first measurements:

- **`k_cycle_get_32()` was broken here until 2026-08** — the timer driver
  returned the MMIO timer's *per-tick* count register, which wraps every tick
  (hence 26 s "frame times").  It now reads the free-running `PERF_CYCLES`
  counter (0xF00D_0008, 100 MHz; `k_cycle_get_64()` also works), so
  cycle-resolution profiling is usable.  One caveat: a perf-counter clear
  (`PERF_CTRL[0]`) zeroes it, invalidating in-flight deltas.  For frame-scale
  timing `k_uptime_get()` (ms) remains the simple choice.
- **`printk`/cbprintf garbles multi-argument lines** on this core (e.g. a 6-arg
  line prints the later fields as scrambled/zero). Keep profiling prints to 1–2
  args, or trust only the **first** field of a multi-arg line.

## What helped vs what didn't

Ranked by measured impact when streaming a changing full-screen image:

**Helped a lot**
1. **Native-format fast path (memcpy, no conversion).** When the client's pixel
   format equals the framebuffer's (RGB565 little-endian *is* the standard VNC
   16bpp format, so this nearly always holds), copy rows out verbatim instead of
   converting per pixel. This cut convert from ~1024 ms → ~356 ms at 640×400.
2. **Lower resolution.** Rendering 320×200 instead of upscaling to 640×400 is ¼
   the pixels → ¼ of render, convert *and* send. The single biggest lever short
   of changing format.
3. **Per-channel conversion LUTs** (for the non-native path). The original
   `convert_pixel` did **3 divides per pixel**; this core has **no hardware
   divide**, so 256K pixels × 3 software divides ≈ 0.5 s. Precompute per-channel
   tables when the client format is set → table lookups, no divides.

4. **2D blitter for the send-path copy** (`CONFIG_KLAUSSCPU_VNC_BLIT_SEND`). On the
   native path, `send_rect`'s framebuffer→`sendbuf` copy is a plain RGB565→RGB565
   strided rectangle, so the blitter can do it: **convert 22 ms → 8 ms** per
   125 KB update. Honest caveat: it did **not** change frame rate — convert was
   only ~6% of the frame. Default n; it also costs two blocking whole-cache walks
   whose INVALIDATE evicts the renderer's working set.
5. **32-bit slot-SRAM copies in the eth driver** (`CONFIG_ETH_KLAUSSCPU_WIDE_SRAM`,
   default y). See "The byte-loop trap" below. **~17 ms/update** (doom frame time
   336 → 319 ms).

**Did NOT help (don't bother — all measured on doom @320×200, full-frame VNC)**
- **Batching sends** (400 per-row `zsock_send` → ~16 chunked): ~9% only. Per-send
  syscall overhead was not the bottleneck.
- **TCP window size** (`CONFIG_NET_TCP_MAX_SEND_WINDOW_SIZE` 0 → 32 KB): no
  change. We were not stop-and-wait / round-trip-limited.
- **Bigger net buffers**: no throughput gain, and an aggressive 4–8× bump
  (`NET_PKT/BUF_*`) mysteriously broke the TCP handshake. Keep them modest.
- **Word-wise libc memcpy** (`CONFIG_MINIMAL_LIBC_OPTIMIZE_STRING_FOR_SIZE=n`):
  **zero** measurable change to `send` (173 → 174 ms). The send path is not
  memcpy-bound, so don't cargo-cult this knob. (`apps/gui_lvgl` sets it because
  its *flush* copy was memcpy-bound — but that copy now goes through the blitter,
  so even there the original justification is stale.)
- **TCP checksum offload**: not worth chasing. `net_calc_chksum` is a word-wise
  sum ≈ **10–15 ms** of a ~175 ms send. The driver declares no
  `ETHERNET_HW_TX_CHKSUM_OFFLOAD`, so it is software — but it is not the cost.
- **8bpp *truecolour*** (client asks for `bpp=8`): already works via the LUT path
  with no code change, and it is a **wash** — send halves (180 → 90 ms) but
  convert *quadruples* (22 → 85 ms), netting ~27 ms and no fps change. Clients
  also pick ugly formats for it (one offered RGB222 — 64 colours). See the
  pixel-format note under "Levers".

## Why `send` is what it is (and what it is not)

For a 125 KB update, `send ≈ 175 ms` = **86 segments at ~2 ms each**. That is
**Zephyr's TCP stack** (net_pkt alloc, header build, checksum, retransmit queue,
window management) — *not* copies. Three copy-based theories were tested and all
rejected: libc memcpy (0 ms), checksum (10–15 ms), driver byte loop (17 ms, real
but minor). **Copy optimisation is exhausted.**

The consequence: send cost scales with **byte count** (segment count), not with
copy speed. The only remaining levers are fewer bytes, or moving convert+send off
the CPU entirely (lever 6 below).

**Measurement trap — `send=` is wall-clock, not CPU.** It wraps `send_all()`,
which *blocks* on TCP flow control. Making the copy cheaper frees CPU **during**
that block without shortening it, so `send=` stays flat while the win shows up in
the *renderer's* frame time. Judge send-path work by the consumer's frame time
(`doom: tick=`), never by `send=` alone.

**Measurement trap — let the workload reach steady state.** doom's first profile
line (`tick=125ms`, 8 fps) is the *title screen*; the real steady state is
`tick≈177ms` (~5.6 fps). A "ceiling" derived from the first sample is wrong by
40%. Likewise, take VNC samples well after connect: the first update is larger
(172 KB) and the next is distorted by TCP slow-start.

## The byte-loop trap (bit us twice, in opposite directions)

`eth_klausscpu.c` / `src/eth.c` copy frames to the MAC's slot SRAM through
hand-rolled loops that exist **only because `memcpy()` won't take a `volatile`
pointer** — never for a hardware reason. But `volatile` also stops the compiler
coalescing, so a byte loop emits one uncached MMIO transaction *per byte*: ~4×
more round trips than needed on every transmitted frame. Widening to 32-bit
accesses is worth ~17 ms/update.

The trap on the way out: **do not reach for `memcpy()` to fix it.** Minimal libc
defaults to `OPTIMIZE_STRING_FOR_SIZE=y` (a byte-at-a-time memcpy) and at `-Os`
the compiler emits a **call** to it — so a 4-byte memcpy per word costs *more*
than the byte loop it replaced (measured: send 175 → **212 ms**, a regression).
Assemble the word with explicit shifts: no call, no libc dependency, always
inlined.

## Reference numbers (Nexys A7 soft core, 100 Mbit LiteEth)

- Effective TCP send throughput: **~1 MB/s** (≈1.4 ms per ~1460-byte segment).
  So bytes-on-wire matter *proportionally* — half the bytes ≈ half the send time.
- `memcpy` of the framebuffer is slow (~1.4 MB/s when done **row-by-row** with
  per-row call overhead). A **contiguous** copy/region is much faster — see below.
- Ethernet TX itself is fast (~120 µs/frame); the MAC is never the limit.

## Levers for the future, in order

1. **Keep the client on the native framebuffer format** so the send path is a
   memcpy (the server already detects RGB565-LE and does this).
2. **Lowest acceptable resolution.** ¼ pixels = ¼ everything.
3. **Make the streamed region contiguous.** A 320-wide image inside a 640-wide
   framebuffer is non-contiguous → row-by-row copies. If the framebuffer width
   equals the streamed width, the whole region is one block → a single fast
   memcpy, or a true **zero-copy send straight from the framebuffer** (accept
   minor tearing, or double-buffer).

   *IMPLEMENTED (2026-08):* `CONFIG_KLAUSSCPU_VNC_FB_WIDTH/HEIGHT` size the
   framebuffer to the content (doom's prj.conf now sets 320×200 — ¼ the pixels
   AND contiguous), and full-width native-format updates go out **zero-copy**
   (`CONFIG_KLAUSSCPU_VNC_ZERO_COPY`, default y): no staging copy, no blit, no
   cache walks, no lock held across the send (frame tearing accepted — the
   next dirty update repaints).  The whole `convert=` stage disappears for
   full-frame native streaming.
4. **8bpp palette mode** (`SetColourMapEntries`) — *re-assessed; read before
   starting.* The bytes-halving half of this is **already available with no code**:
   the LUT path is format-agnostic, so if the client requests `bpp=8` truecolour
   the server sends 1 byte/pixel today. Measured, it is a **wash** (send 180→90 ms
   but convert 22→85 ms) and clients choose ugly formats (one picked RGB222 — 64
   colours, visibly bad).

   A colour *map* would add the other half — sending Doom's native palette indices
   to skip conversion — but it is **gated on the client**, not on us: per RFB the
   client dictates the format via `SetPixelFormat`, the server only *advertises* a
   preference, and real clients request `truecolour=1` (the server already logs
   `client requested palette mode (unsupported)` for the other case). It would also
   need an 8bpp framebuffer path plus tapping `I_VideoBuffer`+palette inside the
   gitignored doom engine. **Verify your client will actually request
   `truecolour=0` before writing any of it.**
5. **Compression encoding (Hextile / RRE)** — pure C, no deps; good for flat UI,
   modest for noisy 3D. Trades CPU for bytes. Note the nuance: `send` is CPU-bound
   *in the TCP stack, proportional to segment count*, so fewer bytes **does** cut
   CPU here — but Doom's noisy textured view compresses poorly, so the CPU traded
   may exceed the CPU saved. Measure on the real content.

   *IMPLEMENTED (2026-08):* **Hextile** (`CONFIG_KLAUSSCPU_VNC_HEXTILE`,
   default y), used when the client lists encoding 5 in SetEncodings.  16×16
   tiles: solid → 3 bytes, two-colour (text / flat UI) → bg + fg + per-row
   runs, anything else → raw (worst case = Raw + 1 byte/tile ≈ 0.2%).  In the
   host-side harness a solid 640×480 frame encodes to **3.6 KB vs 614 KB Raw
   (170×)**.  The doom caveat above is handled *adaptively at runtime*: an
   update that shrinks by less than ~12% switches the server back to Raw (and
   the zero-copy path) for the next 32 updates before re-probing, so noisy
   content pays the tile scan on ~3% of frames only.  Encoder verified by a
   200k-tile fuzz against a reference RFC 6143 decoder (all client formats,
   all tile sizes): `tests/host/test_hextile.c` compiles the real
   vnc_server.c against stub Zephyr headers and runs on any dev host —
   `cd vnc/tests/host && ./run.sh`.
6. **FPGA offload (the real path to smooth, 10+ fps).** Move convert + send off
   the CPU: a fabric block that DMAs the framebuffer (with palette/format
   conversion and TCP checksum) into LiteEth, so the core only renders. This is
   the only way past the single-core ~3–6 fps software ceiling.

## Design rules that bit us

- **Never hold the framebuffer lock across a network send.** Copy the region out
  under the lock, unlock, then send (a long send under the lock stalls the
  producer / causes watchdog-class delays).
- **The send thread starves the producer on this single core.** Equal priority
  caused the *VNC connect to hang* (producer never yielded to the net stack);
  lower-priority producer gets starved *during* sends. There is no free lunch on
  one core — reducing per-frame CPU is the actual fix.
- A large static framebuffer/heap is **materialised in the image** by this SoC's
  linker (single LOAD segment, `FileSiz == MemSiz`). Keep the framebuffer modest,
  and use `CONFIG_COMMON_LIBC_MALLOC_ARENA_SIZE=-1` for big runtime heaps so they
  cost no image space.

## When to skip VNC entirely

If a local display is acceptable, **VGA scanout** (the Nexys A7 has VGA pins) is
dramatically better than VNC for moving content: a fabric module reads the
framebuffer and drives the monitor at full frame rate, bypassing the CPU
convert+send path entirely. VNC only wins when the display must be remote.

## RFB subset implemented (vnc_server.c)

- ProtocolVersion 3.8, security type **None**; **Raw** always, **Hextile**
  when the client offers it (adaptive fallback to Raw for poorly-compressing
  content; full-width native Raw updates are zero-copy).
- Honours the client's `SetPixelFormat` (LUT conversion; native-format memcpy
  fast path). `KeyEvent`/`PointerEvent` are forwarded to handlers registered
  via `vnc_register_input()`. No auth, no TLS — trusted LAN only.
