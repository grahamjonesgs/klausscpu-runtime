#!/usr/bin/env bash
# linux/userland/lua.sh — static Lua 5.4 (lua and luac) for the KlaussCPU
# Linux port, against linux/musl's sysroot.  Lua's Makefile is not used: the
# core and libraries are compiled directly (POSIX build: no dlopen, no
# readline) into linux-ws/lua-build/{lua,luac}, which build-rootfs.sh puts
# in the system image.  Also fetches Lua's own test suite (lua-*-tests) into
# linux-ws/lua-tests for tools/validate/linux.sh's ports mode.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
V=5.4.9
SHA=2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6
TSHA=7d971845f545ffc09fbb3128a86b2c6524161c70d0fdf0154a16e8c00c343fca
SRC=$WS/lua
OUT=$WS/lua-build
[ -f "$WS/sysroot/lib/libc.a" ] || { echo "no musl sysroot: run linux/musl/build.sh" >&2; exit 1; }

fetch() { # <url> <sha256> <dir>
  local tgz=$WS/$(basename "$1")
  [ -f "$tgz" ] || curl -fsSL --retry 5 -o "$tgz" "$1"
  echo "$2  $tgz" | sha256sum -c --quiet || { echo "$tgz: bad SHA-256" >&2; rm -f "$tgz"; exit 1; }
  rm -rf "$3"; mkdir -p "$3"
  tar -xzf "$tgz" -C "$3" --strip-components=1
}
[ -f "$SRC/src/lua.h" ] || fetch "https://www.lua.org/ftp/lua-$V.tar.gz" "$SHA" "$SRC"
[ -f "$WS/lua-tests/all.lua" ] || fetch "https://www.lua.org/tests/lua-$V-tests.tar.gz" "$TSHA" "$WS/lua-tests"

rm -rf "$OUT"; mkdir -p "$OUT/obj"
CC=("$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot")
CFLAGS=(-O2 -Wall -DLUA_USE_POSIX -DLUA_COMPAT_5_3 -ffunction-sections -fdata-sections)
CORE="lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes
  lparser lstate lstring ltable ltm lundump lvm lzio
  lauxlib lbaselib lcorolib ldblib liolib lmathlib loadlib loslib lstrlib
  ltablib lutf8lib linit"
for f in $CORE lua luac; do
  "${CC[@]}" "${CFLAGS[@]}" -c "$SRC/src/$f.c" -o "$OUT/obj/$f.o" &
done
wait
LIB=$(for f in $CORE; do echo "$OUT/obj/$f.o"; done)
"${CC[@]}" -static -Wl,--gc-sections -o "$OUT/lua" "$OUT/obj/lua.o" $LIB -lm
"${CC[@]}" -static -Wl,--gc-sections -o "$OUT/luac" "$OUT/obj/luac.o" $LIB -lm
echo "lua: $OUT/lua $OUT/luac (Lua $V)"
