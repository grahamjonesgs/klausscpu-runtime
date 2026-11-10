#!/usr/bin/env bash
# linux/userland/busybox.sh — static busybox 1.37.0 for the KlaussCPU Linux
# port, against linux/musl/build.sh's sysroot: allnoconfig plus
# busybox.config, built out of tree in linux-ws/busybox-build.  The
# initramfs profile "busybox" (linux/initramfs/build.sh) uses the binary.
#
# Not yet run: building busybox (it compiles and runs its own host tools)
# was refused by this session's permission classifier on 2026-11-09.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
SRC=$WS/busybox
OUT=$WS/busybox-build
[ -d "$SRC/.git" ] || git clone -q --depth 1 --branch 1_37_0 https://git.busybox.net/busybox "$SRC"
[ -f "$WS/sysroot/lib/libc.a" ] || { echo "no musl sysroot: run linux/musl/build.sh" >&2; exit 1; }
mkdir -p "$OUT"
MK=(make -C "$SRC" O="$OUT" HOSTCC=gcc
    CC="$B/clang --target=klausscpu-unknown-linux-musl --sysroot=$WS/sysroot"
    AR="$B/llvm-ar" NM="$B/llvm-nm" STRIP="$B/llvm-strip" OBJCOPY="$B/llvm-objcopy"
    OBJDUMP="$B/llvm-objdump")
"${MK[@]}" allnoconfig >"$OUT/config.log" 2>&1
while IFS= read -r line; do
  case $line in CONFIG_*=*) ;; *) continue ;; esac
  key=${line%%=*}
  sed -i "/^$key=/d; /^# $key is not set/d" "$OUT/.config"
  echo "$line" >>"$OUT/.config"
done <"$HERE/busybox.config"
{ yes "" || true; } | "${MK[@]}" oldconfig >>"$OUT/config.log" 2>&1
"${MK[@]}" -j"${JOBS:-$(nproc)}" busybox busybox.links >"$OUT/build.log" 2>&1 || { tail -30 "$OUT/build.log"; exit 1; }
echo "busybox: $OUT/busybox"
