# Doom on KlaussCPU (over VNC and VGA)

A port of [doomgeneric](https://github.com/ozkl/doomgeneric) to KlaussCPU/Zephyr
that renders into the VNC module's in-RAM framebuffer — so Doom is playable over
a VNC client — and shows on a monitor through the board's VGA port (see
"VGA output" below).

## What's tracked vs fetched

- **Tracked (this repo):** the Zephyr platform backend and build glue under
  `src/`, `compat/`, `CMakeLists.txt`, `prj.conf`.
- **Fetched (gitignored):** the doomgeneric engine sources in `doomgeneric-src/`.

The engine relies on stdio/POSIX/libm that Zephyr's **minimal libc** doesn't
provide; the port supplies the gaps itself:
- `src/zephyr_stdio.c` — `fopen/fread/fseek/ftell/fclose/feof` + POSIX fd I/O
  (`open/read/write/close/lseek`) backed by Zephyr's `fs_*`, so the WAD loads
  from `/SD:`.
- `src/compat.c` — small POSIX stubs (`stat/access/usleep/gettimeofday/...`)
  and a libm subset (`sin/cos/tan/atan/fabs`) Doom calls at renderer init.
- `compat/` — the POSIX headers minimal libc lacks (`unistd.h`, `sys/stat.h`, …).

## Build & run

```sh
# 1. Fetch the engine sources (once)
./fetch-doomgeneric.sh

# 2. Build (from zephyr-ws/, with KLAUSSCPU_LLVM_BIN set — see ../../CLAUDE.md)
MOD=$PWD/klausscpu-zephyr; ZEPHYR=$PWD/zephyr
west build -p always -b nexys_a7 "$MOD/apps/doom" --build-dir build_doom -- \
  -DZEPHYR_TOOLCHAIN_VARIANT=klausscpu-clang \
  -DKLAUSSCPU_LLVM_BIN="$KLAUSSCPU_LLVM_BIN" -DTOOLCHAIN_ROOT="$MOD" \
  "-DEXTRA_ZEPHYR_MODULES=$MOD" \
  "-DARCH_ROOT=$MOD;$ZEPHYR" "-DSOC_ROOT=$MOD;$ZEPHYR" \
  "-DBOARD_ROOT=$MOD;$ZEPHYR" "-DDTS_ROOT=$MOD;$ZEPHYR"

# 3. Put a Doom IWAD on the SD card as /SD:/doom1.wad
#    (the freely redistributable shareware doom1.wad works)

# 4. Convert + load to the FPGA (see ../../CLAUDE.md), then connect a VNC
#    client to the board's IP on :5900.
```

**Keyboard input is wired**: the VNC server delivers RFB `KeyEvent`s (X11
keysyms) to `on_key()`, which maps them to Doom key codes and queues them for
`DG_GetKey()` — so Doom is playable, not just attract-mode demos. The mapping
mirrors doomgeneric's own X11 backend (`doomgeneric_xlib.c`): arrows move, Ctrl
fires, Space uses/opens doors, Shift runs, Esc/Enter/y/n drive the menu, and
1–7 select weapons. (With no VNC client connected, Doom still falls back to its
auto-playing title-screen demos.) Mouse input is not implemented — doomgeneric's
platform API is keyboard-only (`DG_GetKey`), so there is no pointer hook.

## VGA output

Every build also drives the board's **VGA port** (`CONFIG_KLAUSSCPU_VGA=y` in
`prj.conf`; KlaussCPU `VGA_PLAN.md`). Doom's 8-bit frames go straight to the
VGA's palette mode, letterboxed 320×200 → 640×400 in the 640×480 picture.
They are triple-buffered and flipped at vblank, so there is no tearing, and
they are **zero-copy**: `DG_ScreenBuffer` is pointed at a free VGA buffer
each frame, so `I_FinishUpdate` renders straight into it. AMP builds copy
instead, because core 2 reads the buffer at a fixed address. VNC and VGA
show the same game.

For the fastest Doom, build the VGA-only variant: no networking, no VNC and
no RGB565 conversion.

```sh
west build -p always -b nexys_a7 "$MOD/apps/doom" --build-dir build_doom_vga -- \
  -DEXTRA_CONF_FILE=vga.conf <the usual -D... flags from step 2>
```

Measured on the board, averaged over the same 27 attract-mode demo windows:

| Build | Gameplay fps |
|---|---|
| `vga.conf` (zero-copy) | **24.7** |
| AMP + VNC, no VGA | 23.7 |
| AMP + VNC + VGA (copy) | 20.8 |

The VGA hand-off costs under 1 ms per frame (one cache flush), with 0
display underflows; the profile line prints `vga_underflows`. The title
screen runs at about 52 fps. **VGA-only has
no keyboard** (input arrives via VNC), so Doom plays its demos. To play, use a
VNC build; the game appears on both screens.

## Notes / limits

- Engine is the bare-metal "soso" source set: no sound (generic dummy
  `i_sound.c`), no music, no SDL.
- The WAD path is passed explicitly (`-iwad /SD:/doom1.wad`), so the IWAD
  directory search (which would need `opendir`/`stat`) is bypassed — those are
  stubbed.
- Framerate is bounded by the soft core + full-frame Raw VNC updates; expect
  low FPS. It runs; it's not a fragfest.
- GPLv2 (Doom engine). The shareware IWAD is freely redistributable.
