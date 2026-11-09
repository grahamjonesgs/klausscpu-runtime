#!/usr/bin/env bash
# linux/initramfs/build.sh <outdir> — build the milestone-3 /init (static,
# ELF32, no libc) and the initramfs list the kernel's
# CONFIG_INITRAMFS_SOURCE ("initramfs.list", relative to the build dir)
# names.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$1
B=${KLAUSSCPU_LLVM_BIN:-$HERE/../../../klausscpu-llvm/build/bin}
mkdir -p "$OUT/initramfs"
"$B/clang" --target=klausscpu-unknown-linux-gnu -O2 -ffreestanding -nostdlib -fno-pic \
  -fno-stack-protector -c "$HERE/init.c" -o "$OUT/initramfs/init.o"
"$B/ld.lld" -static -z max-page-size=0x10000 -T "$HERE/user.ld" \
  "$OUT/initramfs/init.o" -o "$OUT/initramfs/init"
cat >"$OUT/initramfs.list" <<LIST
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/kmsg 0600 0 0 c 1 11
dir /proc 0755 0 0
dir /sys 0755 0 0
file /init $OUT/initramfs/init 0755 0 0
LIST
echo "initramfs: $OUT/initramfs.list"
