#!/usr/bin/env bash
# linux/userland/busybox.sh — static busybox 1.37.0 for the KlaussCPU Linux
# port, against linux/musl/build.sh's sysroot: allnoconfig plus
# busybox.config, built out of tree in linux-ws/busybox-build; and bbsuid
# (busybox-suid.config: su and passwd, installed setuid root) in
# linux-ws/busybox-suid-build.  The
# initramfs profile "busybox" (linux/initramfs/build.sh) uses the binary.
#
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
SRC=$WS/busybox
[ -d "$SRC/.git" ] || git clone -q --depth 1 --branch 1_37_0 https://git.busybox.net/busybox "$SRC"
[ -f "$WS/sysroot/lib/libc.a" ] || { echo "no musl sysroot: run linux/musl/build.sh" >&2; exit 1; }
# build OUT CONFIG: allnoconfig plus CONFIG's lines, built out of tree in OUT.
build() {
OUT=$1
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
done <"$2"
{ yes "" || true; } | "${MK[@]}" oldconfig >>"$OUT/config.log" 2>&1
"${MK[@]}" -j"${JOBS:-$(nproc)}" busybox busybox.links >"$OUT/build.log" 2>&1 || { tail -30 "$OUT/build.log"; exit 1; }
echo "busybox: $OUT/busybox"
}
build "$WS/busybox-build" "$HERE/busybox.config"
# bbsuid: su and passwd only, installed setuid root (busybox-suid.config).
build "$WS/busybox-suid-build" "$HERE/busybox-suid.config"
# It must hold nothing but su and passwd.
sort "$WS/busybox-suid-build/busybox.links" | tr '\n' ' ' | grep -qx '/bin/su /usr/bin/passwd ' ||
  { echo "busybox.sh: bbsuid has more than su and passwd: $(cat "$WS/busybox-suid-build/busybox.links")" >&2; exit 1; }

