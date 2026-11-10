#!/usr/bin/env bash
# linux/rootfs/build-rootfs.sh <out.img> [size MiB, default 96]
#
# The system image for the SD card: an ext4 image (file) that the
# initramfs's sdinit loop-mounts READ-ONLY from the card's FAT partition as
# /linux/rootfs.img (replaced whole by install-rootfs).  Run-time state
# lives elsewhere:
#   /root, /home      -> /data/root, /data/home (data.img, build-data.sh)
#   /var/run, /var/log, /var/lock -> /run/...; /var/tmp -> /tmp
#   /etc/resolv.conf  -> /run/resolv.conf;  /etc/dropbear -> /run/dropbear
#   /etc/mtab         -> /proc/mounts
# (/tmp and /run are tmpfs, mounted by rcS).  No journal: it is never
# written.  Staged from the builds in linux-ws/ (busybox, Dropbear if
# built, musl programs compiled here) and the /etc of linux/initramfs/;
# packed as a tarball with root ownership and made with mke2fs -d.
# KLAUSSCPU_ROOTFS_TAG is appended to /etc/klausscpu-rootfs if set.
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
mkdir -p "$ST"/{bin,sbin,usr/bin,usr/sbin,usr/share/udhcpc,etc/init.d,var,www,proc,sys,dev,tmp,run,mnt,data}
chmod 1777 "$ST/tmp"
ln -s data/root "$ST/root"
ln -s data/home "$ST/home"
ln -s ../run "$ST/var/run"
ln -s ../run/log "$ST/var/log"
ln -s ../run/lock "$ST/var/lock"
ln -s ../tmp "$ST/var/tmp"
ln -s ../run/resolv.conf "$ST/etc/resolv.conf"
ln -s ../run/dropbear "$ST/etc/dropbear"
ln -s ../proc/mounts "$ST/etc/mtab"
CC=("$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot" -Os)
cp "$BB" "$ST/bin/busybox"
ln -s busybox "$ST/bin/sh"
ln -s ../bin/busybox "$ST/sbin/init"
ln -s ../bin/busybox "$ST/sbin/mdev"
# A link for every applet (busybox.links: /bin/ls, /usr/bin/awk, ...), so
# programs that look commands up on PATH find them, not only ash's
# standalone shell.  Existing files (sh, init, mdev, Dropbear's) win.
if [ -f "$WS/busybox-build/busybox.links" ]; then
  while IFS= read -r l; do
    [ -n "$l" ] || continue
    d=$ST$(dirname "$l"); mkdir -p "$d"
    [ -e "$ST$l" ] || [ -L "$ST$l" ] || ln -s "$(realpath -m --relative-to="$(dirname "$l")" /bin/busybox)" "$ST$l"
  done <"$WS/busybox-build/busybox.links"
fi
"${CC[@]}" "$RT/linux/userland/keystore/kskey.c" -o "$ST/usr/bin/kskey"
"${CC[@]}" "$RT/linux/userland/musltest.c" -o "$ST/usr/bin/musltest"
"${CC[@]}" "$RT/linux/userland/minish.c" -o "$ST/usr/bin/minish"
cp "$RT/linux/userland/install-rootfs" "$RT/linux/userland/install-data" "$ST/usr/bin/"
if [ -x "$DB" ]; then
  cp "$DB" "$ST/usr/sbin/dropbearmulti"
  ln -s dropbearmulti "$ST/usr/sbin/dropbear"
  for p in dropbearkey dbclient scp dropbearconvert; do
    ln -s ../sbin/dropbearmulti "$ST/usr/bin/$p"
  done
fi
# fbtest (/dev/fb0 checks and screenshots), and DOOM (linux/userland/doom.sh)
# when built; its WAD stays on the card (/mnt/DOOM1.WAD).
"${CC[@]}" "$RT/linux/userland/fbtest.c" -o "$ST/usr/bin/fbtest"
if [ -x "$WS/doom-build/doom" ]; then
  cp "$WS/doom-build/doom" "$ST/usr/bin/doom"
fi
# Ports (linux/userland/<pkg>.sh), each when built, and their self-tests in
# /usr/share/klausscpu/tests (linux/userland/tests; run them on the board).
T=$ST/usr/share/klausscpu/tests
mkdir -p "$T"
if [ -x "$WS/lua-build/lua" ]; then
  cp "$WS/lua-build/lua" "$WS/lua-build/luac" "$ST/usr/bin/"
  cp -r "$WS/lua-tests" "$T/lua"
  rm -rf "$T/lua/libs" "$T/lua/ltests"
  install -m 755 "$RT/linux/userland/tests/lua-tests.sh" "$T/"
  cp "$RT/linux/userland/tests/lua-selftest.lua" "$T/"
fi
if [ -x "$WS/sqlite-build/sqlite3" ]; then
  cp "$WS/sqlite-build/sqlite3" "$ST/usr/bin/"
  install -m 755 "$RT/linux/userland/tests/sqlite-test.sh" "$T/"
  cp "$RT/linux/userland/tests/sqlite-test.sql" "$RT/linux/userland/tests/sqlite-test.expected" "$T/"
fi
if [ -x "$WS/coremark-build/coremark" ] && [ -x "$WS/lmbench-build/tlbmiss" ]; then
  cp "$WS/coremark-build/coremark" "$WS/lmbench-build/tlbmiss" "$ST/usr/bin/"
  mkdir -p "$ST/usr/libexec/lmbench"
  for b in lat_syscall lat_proc lat_ctx lat_pagefault bw_mem lat_mem_rd tlb hello; do
    cp "$WS/lmbench-build/$b" "$ST/usr/libexec/lmbench/"
  done
  install -m 755 "$RT/linux/userland/tests/bench.sh" "$T/"
fi
if [ -x "$WS/iperf3-build/iperf3" ]; then
  cp "$WS/iperf3-build/iperf3" "$ST/usr/bin/"
  install -m 755 "$RT/linux/userland/tests/iperf3-test.sh" "$T/"
fi
if [ -x "$WS/curl-build/curl" ]; then
  cp "$WS/curl-build/curl" "$ST/usr/bin/"
  mkdir -p "$ST/etc/ssl/certs" "$ST/usr/libexec"
  install -m 644 "$WS/curl-build/ca-certificates.crt" "$ST/etc/ssl/certs/ca-certificates.crt"
  cp "$WS/mbedtls-build/mbedtls-selftest" "$ST/usr/libexec/"
  install -m 755 "$RT/linux/userland/tests/curl-test.sh" "$T/"
fi
cp "$INI/inittab" "$INI/profile" "$ST/etc/"
cp "$INI/rcS" "$INI/hostkey" "$ST/etc/init.d/"
cp "$INI/etc/passwd" "$INI/etc/group" "$ST/etc/"
install -m 600 "$INI/etc/shadow" "$ST/etc/shadow"
cp "$INI/default.script" "$ST/usr/share/udhcpc/default.script"
cp "$INI/etc/index.html" "$ST/www/index.html"
echo "klausscpu-rootfs $(date -u +%Y-%m-%dT%H:%MZ) from runtime $(git -C "$RT" rev-parse --short HEAD)${KLAUSSCPU_ROOTFS_TAG:+ $KLAUSSCPU_ROOTFS_TAG}" \
  >"$ST/etc/klausscpu-rootfs"

# Buildroot's packages (linux/buildroot/build.sh), with ROOTFS_BUILDROOT=1:
# bin, sbin and usr of its rootfs.tar.  A package's file replaces a busybox
# applet link (a real less over the applet), never one of our own files;
# its /etc (Buildroot's skeleton), headers and static libraries stay out,
# so our init, rcS and layout are untouched.
if [ "${ROOTFS_BUILDROOT:-0}" = 1 ]; then
  BRT=$WS/br-output/images/rootfs.tar
  [ -f "$BRT" ] || { echo "no $BRT: run linux/buildroot/build.sh" >&2; exit 1; }
  X=$WS/br-merge
  rm -rf "$X"; mkdir -p "$X"
  tar -xf "$BRT" -C "$X"
  (cd "$X" && find bin sbin usr \( -type f -o -type l \) 2>/dev/null) | while IFS= read -r f; do
    case $f in usr/include/*|*.a|*.la|usr/lib/pkgconfig/*|usr/share/aclocal/*|usr/share/man/*|usr/share/doc/*) continue ;; esac
    d=$ST/$f
    if [ -e "$d" ] && [ ! -L "$d" ]; then
      echo "buildroot: keeping our /$f" >&2
      continue
    fi
    mkdir -p "$(dirname "$d")"
    rm -f "$d"
    cp -a "$X/$f" "$d"
    # The package's program replaces busybox's applet of that name in the
    # other bin directories too, which may come first in PATH (/bin/less).
    case $f in
    bin/*|sbin/*|usr/bin/*|usr/sbin/*)
      for b in bin sbin usr/bin usr/sbin; do
        o=$ST/$b/${f##*/}
        if [ "$o" != "$d" ] && [ -L "$o" ]; then
          case $(readlink "$o") in *busybox) rm -f "$o" ;; esac
        fi
      done ;;
    esac
  done
  echo "buildroot: merged $(tar -tf "$BRT" | grep -c -E '^\./(usr/)?s?bin/[^/]+$') programs from $BRT"
  install -m 755 "$RT/linux/userland/tests/buildroot-test.sh" "$T/"
fi

tar --owner=0 --group=0 --numeric-owner -C "$ST" -cf "$WS/rootfs.tar" .
rm -f "$OUTIMG"
mke2fs -q -t ext4 -O ^has_journal -L klausscpu-root -m 0 -d "$WS/rootfs.tar" "$OUTIMG" "${SIZE}M"
e2fsck -fn "$OUTIMG" >/dev/null
echo "rootfs: $OUTIMG (${SIZE} MiB, sha256 $(sha256sum "$OUTIMG" | cut -d' ' -f1))"
