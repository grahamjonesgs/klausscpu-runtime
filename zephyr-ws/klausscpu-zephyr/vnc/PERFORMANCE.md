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

- **`k_cycle_get_32()` is unreliable here** — it does not behave as a clean
  100 MHz free-running counter (gave 26 s "frame times"). **Use `k_uptime_get()`
  (ms).** Frames are >1 ms so ms resolution is fine.
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

**Did NOT help (don't bother)**
- **Batching sends** (400 per-row `zsock_send` → ~16 chunked): ~9% only. Per-send
  syscall overhead was not the bottleneck.
- **TCP window size** (`CONFIG_NET_TCP_MAX_SEND_WINDOW_SIZE` 0 → 32 KB): no
  change. We were not stop-and-wait / round-trip-limited.
- **Bigger net buffers**: no throughput gain, and an aggressive 4–8× bump
  (`NET_PKT/BUF_*`) mysteriously broke the TCP handshake. Keep them modest.

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
4. **8bpp palette mode** (`SetColourMapEntries`). Halves bytes vs 16bpp and, for
   natively-paletted content (e.g. Doom), removes conversion entirely. Not
   implemented in the server yet — this is the next software win.
5. **Compression encoding (Hextile / RRE)** — pure C, no deps; good for flat UI,
   modest for noisy 3D. Trades CPU for bytes, so only worth it once genuinely
   bandwidth-bound (which, per above, we were not).
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

- ProtocolVersion 3.8, security type **None**, **Raw** encoding only.
- Honours the client's `SetPixelFormat` (LUT conversion; native-format memcpy
  fast path). `KeyEvent`/`PointerEvent` are parsed but currently discarded
  (input plumbing is the next feature). No auth, no TLS — trusted LAN only.
