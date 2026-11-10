#!/usr/bin/env bash
# linux/userland/doom.sh — DOOM for the KlaussCPU Linux port: doomgeneric
# (ozkl/doomgeneric, pinned below; GPLv2) with an fbdev backend,
# linux/userland/doom/doomgeneric_klausscpu.c, built static against
# linux/musl's sysroot into linux-ws/doom-build/doom.
#   - the engine is cloned into linux-ws/doomgeneric and checked out at
#     $COMMIT; linux/userland/doom/patches/*.patch are applied on top (the
#     same 64-bit game timer fix as the Zephyr port);
#   - 320x200, 8-bit frames (CMAP256), no sound, no music;
#   - no WAD ships: doom finds DOOM1.WAD at the SD card's root (/mnt).
# build-rootfs.sh installs linux-ws/doom-build/doom as /usr/bin/doom when
# it exists.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
SRC=$WS/doomgeneric
OUT=$WS/doom-build
COMMIT=e257a09527062bfb986b6e5e0780839b6cbd5b71   # master, 2026-10-11 (as the Zephyr port)
[ -f "$WS/sysroot/lib/libc.a" ] || { echo "no musl sysroot: run linux/musl/build.sh" >&2; exit 1; }
[ -d "$SRC/.git" ] || git clone -q https://github.com/ozkl/doomgeneric.git "$SRC"
git -C "$SRC" checkout -q -f "$COMMIT"
git -C "$SRC" clean -q -fd
for p in "$HERE"/doom/patches/*.patch; do
  git -C "$SRC" apply "$p"
done
rm -rf "$OUT"; mkdir -p "$OUT/obj"
CC=("$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot")
CFLAGS=(-O2 -ffunction-sections -fdata-sections -Wno-unused-command-line-argument
        -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE -DCMAP256
        -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -I"$SRC/doomgeneric")
ENGINE="am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode
  d_net f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick
  i_scale i_sound i_system i_timer memio m_argv m_bbox m_cheat m_config
  m_controls m_fixed m_menu m_misc m_random p_ceilng p_doors p_enemy p_floor
  p_inter p_lights p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight
  p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main r_plane
  r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables
  v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input
  i_video doomgeneric dummy"
jobs=${JOBS:-$(nproc)}
# The engine is old C: its warnings are not ours.
for s in $ENGINE; do echo "$s"; done | xargs -P "$jobs" -I{} \
  "${CC[@]}" "${CFLAGS[@]}" -w -c "$SRC/doomgeneric/{}.c" -o "$OUT/obj/{}.o"
"${CC[@]}" "${CFLAGS[@]}" -Wall -Wextra -c "$HERE/doom/doomgeneric_klausscpu.c" \
  -o "$OUT/obj/doomgeneric_klausscpu.o"
"${CC[@]}" -static -Wl,--gc-sections -o "$OUT/doom" "$OUT"/obj/*.o -lm
echo "doom: $OUT/doom ($(stat -c %s "$OUT/doom") bytes, doomgeneric ${COMMIT:0:7})"
