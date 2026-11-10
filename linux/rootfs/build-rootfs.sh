#!/usr/bin/env bash
# linux/rootfs/build-rootfs.sh <out.img> [size MiB, default 96]
#
# The root file system for the SD card: an ext4 image (file) that the
# initramfs's sdinit loop-mounts from the card's FAT partition as
# /linux/rootfs.img.  Staged from the builds in linux-ws/ (busybox, Dropbear
# if built, musl programs compiled here) and the /etc of linux/initramfs/;
# packed as a tarball with root ownership and made with mke2fs -d.
# Needs linux/musl/build.sh and linux/userland/busybox.sh first.
set -euo pipefail
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
OUTIMG=$(realpath -m "$1")
SIZE=${2:-96}
INI=$RT/linux/initramfs
ST=$WS/rootfs-staging
BB=$WS/busybox-build/busybox
DB=$WS/dropbear-build/dropbearmulti
[ -x "$BB" ] || { echo "no busybox: run linux/userland/busybox.sh" >&2; exit 1; }
[ -f "$WS/sysroot/lib/libc.a" ] || { echo "no musl sysroot: run linux/musl/build.sh" >&2; exit 1; }

rm -rf "$ST"
mkdir -p "$ST"/{bin,sbin,usr/bin,usr/sbin,usr/share/udhcpc,etc/init.d,etc/dropbear,root,var/run,var/log,www,proc,sys,dev,tmp,mnt,home}
chmod 1777 "$ST/tmp"
chmod 700 "$ST/root"
CC=("$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot" -Os)
cp "$BB" "$ST/bin/busybox"
ln -s busybox "$ST/bin/sh"
ln -s ../bin/busybox "$ST/sbin/init"
ln -s ../bin/busybox "$ST/sbin/mdev"
"${CC[@]}" "$RT/linux/userland/keystore/kskey.c" -o "$ST/usr/bin/kskey"
"${CC[@]}" "$RT/linux/userland/musltest.c" -o "$ST/usr/bin/musltest"
"${CC[@]}" "$RT/linux/userland/minish.c" -o "$ST/usr/bin/minish"
cp "$RT/linux/userland/install-rootfs" "$ST/usr/bin/install-rootfs"
if [ -x "$DB" ]; then
  cp "$DB" "$ST/usr/sbin/dropbearmulti"
  ln -s dropbearmulti "$ST/usr/sbin/dropbear"
  for p in dropbearkey dbclient scp dropbearconvert; do
    ln -s ../sbin/dropbearmulti "$ST/usr/bin/$p"
  done
fi
cp "$INI/inittab" "$INI/profile" "$ST/etc/"
cp "$INI/rcS" "$INI/hostkey" "$ST/etc/init.d/"
cp "$INI/etc/passwd" "$INI/etc/group" "$ST/etc/"
install -m 600 "$INI/etc/shadow" "$ST/etc/shadow"
cp "$INI/default.script" "$ST/usr/share/udhcpc/default.script"
cp "$INI/etc/index.html" "$ST/www/index.html"
echo "klausscpu-rootfs $(date -u +%Y-%m-%dT%H:%MZ) from runtime $(git -C "$RT" rev-parse --short HEAD)" \
  >"$ST/etc/klausscpu-rootfs"

tar --owner=0 --group=0 --numeric-owner -C "$ST" -cf "$WS/rootfs.tar" .
rm -f "$OUTIMG"
mke2fs -q -t ext4 -L klausscpu-root -m 0 -d "$WS/rootfs.tar" "$OUTIMG" "${SIZE}M"
e2fsck -fn "$OUTIMG" >/dev/null
echo "rootfs: $OUTIMG (${SIZE} MiB, sha256 $(sha256sum "$OUTIMG" | cut -d' ' -f1))"
