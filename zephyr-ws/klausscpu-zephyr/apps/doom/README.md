# Doom on KlaussCPU (over VNC)

A port of [doomgeneric](https://github.com/ozkl/doomgeneric) to KlaussCPU/Zephyr
that renders into the VNC module's in-RAM framebuffer — so Doom is playable over
a VNC client with no VGA hardware.

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

The first bring-up target is **attract mode**: with no input wired yet, Doom
auto-plays its title-screen demos, which exercises the whole WAD→render→VNC
pipeline. Keyboard input (VNC `KeyEvent` → `DG_GetKey`) is the next step.

## Notes / limits

- Engine is the bare-metal "soso" source set: no sound (generic dummy
  `i_sound.c`), no music, no SDL.
- The WAD path is passed explicitly (`-iwad /SD:/doom1.wad`), so the IWAD
  directory search (which would need `opendir`/`stat`) is bypassed — those are
  stubbed.
- Framerate is bounded by the soft core + full-frame Raw VNC updates; expect
  low FPS. It runs; it's not a fragfest.
- GPLv2 (Doom engine). The shareware IWAD is freely redistributable.
