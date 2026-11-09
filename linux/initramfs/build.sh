#!/usr/bin/env bash
# linux/initramfs/build.sh <outdir> — the initramfs for the kernel's
# CONFIG_INITRAMFS_SOURCE ("initramfs.list", relative to the build dir).
#   INITRAMFS_PROFILE  m3 (default): the libc-free milestone-3 /init
#                      musltest: /init = linux/userland/musltest.c (musl)
#                      busybox: /init and /bin/sh from busybox (musl)
# The musl profiles need linux/musl/build.sh's sysroot (linux-ws/sysroot),
# busybox also linux/userland/busybox.sh's binary.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
OUT=$1
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
PROFILE=${INITRAMFS_PROFILE:-m3}
mkdir -p "$OUT/initramfs"
LIST=$OUT/initramfs.list
cat >"$LIST" <<LIST
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/kmsg 0600 0 0 c 1 11
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /tmp 1777 0 0
LIST
case $PROFILE in
m3)
  "$B/clang" --target=klausscpu-unknown-linux-gnu -O2 -ffreestanding -nostdlib -fno-pic \
    -fno-stack-protector -c "$HERE/init.c" -o "$OUT/initramfs/init.o"
  "$B/ld.lld" -static -z max-page-size=0x10000 -T "$HERE/user.ld" \
    "$OUT/initramfs/init.o" -o "$OUT/initramfs/init"
  echo "file /init $OUT/initramfs/init 0755 0 0" >>"$LIST"
  ;;
musltest)
  "$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot" -Os \
    "$RT/linux/userland/musltest.c" -o "$OUT/initramfs/musltest"
  echo "file /init $OUT/initramfs/musltest 0755 0 0" >>"$LIST"
  ;;
busybox)
  BB=$WS/busybox-build/busybox
  "$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot" -Os \
    "$RT/linux/userland/musltest.c" -o "$OUT/initramfs/musltest"
  cat >>"$LIST" <<LIST
dir /bin 0755 0 0
dir /sbin 0755 0 0
dir /etc 0755 0 0
dir /root 0700 0 0
file /bin/busybox $BB 0755 0 0
slink /bin/sh busybox 0777 0 0
slink /init bin/busybox 0777 0 0
file /etc/inittab $HERE/inittab 0644 0 0
file /etc/profile $HERE/profile 0644 0 0
file /bin/musltest $OUT/initramfs/musltest 0755 0 0
LIST
  ;;
*) echo "unknown INITRAMFS_PROFILE $PROFILE" >&2; exit 1 ;;
esac
echo "initramfs ($PROFILE): $LIST"
