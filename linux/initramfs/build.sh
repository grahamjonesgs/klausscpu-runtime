#!/usr/bin/env bash
# linux/initramfs/build.sh <outdir> — the initramfs for the kernel's
# CONFIG_INITRAMFS_SOURCE ("initramfs.list", relative to the build dir).
#   INITRAMFS_PROFILE  m3 (default): the libc-free milestone-3 /init
#                      musltest: /init = linux/userland/musltest.c (musl)
#                      minish: /init = linux/userland/minish.c, a small
#                              musl shell, with /bin/musltest
#                      busybox: /init = sdinit (switches to the SD card's
#                               /linux/rootfs.img if there is one), else
#                               busybox init (rcS, mdev, eth0 up), ash,
#                               and Dropbear if linux/userland/dropbear.sh
#                               has built it (musl)
# INITRAMFS_EXTRA     a file of extra gen_init_cpio lines, appended
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
minish)
  for p in minish musltest; do
    "$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot" -Os \
      "$RT/linux/userland/$p.c" -o "$OUT/initramfs/$p"
  done
  cat >>"$LIST" <<LIST
dir /bin 0755 0 0
file /init $OUT/initramfs/minish 0755 0 0
file /bin/minish $OUT/initramfs/minish 0755 0 0
file /bin/musltest $OUT/initramfs/musltest 0755 0 0
LIST
  ;;
busybox)
  BB=$WS/busybox-build/busybox
  DB=$WS/dropbear-build/dropbearmulti
  "$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot" -Os \
    "$RT/linux/userland/musltest.c" -o "$OUT/initramfs/musltest"
  "$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot" -Os \
    "$RT/linux/userland/keystore/kskey.c" -o "$OUT/initramfs/kskey"
  "$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot" -Os \
    "$RT/linux/userland/sdinit.c" -o "$OUT/initramfs/sdinit"
  cat >>"$LIST" <<LIST
dir /bin 0755 0 0
dir /sbin 0755 0 0
dir /usr 0755 0 0
dir /usr/bin 0755 0 0
dir /usr/sbin 0755 0 0
dir /usr/share 0755 0 0
dir /usr/share/udhcpc 0755 0 0
dir /etc 0755 0 0
dir /etc/init.d 0755 0 0
dir /root 0700 0 0
dir /var 0755 0 0
dir /var/run 0755 0 0
dir /var/log 0755 0 0
dir /www 0755 0 0
file /bin/busybox $BB 0755 0 0
slink /bin/sh busybox 0777 0 0
slink /sbin/init ../bin/busybox 0777 0 0
slink /sbin/mdev ../bin/busybox 0777 0 0
file /init $OUT/initramfs/sdinit 0755 0 0
file /etc/inittab $HERE/inittab 0644 0 0
file /etc/init.d/rcS $HERE/rcS 0755 0 0
file /etc/init.d/hostkey $HERE/hostkey 0755 0 0
file /etc/profile $HERE/profile 0644 0 0
file /etc/passwd $HERE/etc/passwd 0644 0 0
file /etc/shadow $HERE/etc/shadow 0600 0 0
file /etc/group $HERE/etc/group 0644 0 0
file /usr/share/udhcpc/default.script $HERE/default.script 0755 0 0
file /www/index.html $HERE/etc/index.html 0644 0 0
file /bin/musltest $OUT/initramfs/musltest 0755 0 0
file /usr/bin/kskey $OUT/initramfs/kskey 0755 0 0
file /usr/bin/install-rootfs $RT/linux/userland/install-rootfs 0755 0 0
file /usr/bin/install-data $RT/linux/userland/install-data 0755 0 0
file /usr/bin/install-swap $RT/linux/userland/install-swap 0755 0 0
dir /mnt 0755 0 0
dir /run 0755 0 0
slink /etc/resolv.conf ../run/resolv.conf 0777 0 0
slink /etc/dropbear ../run/dropbear 0777 0 0
LIST
  # A link for every applet: ash runs commands from PATH (busybox has no
  # standalone shell).  sh, init and mdev are above.
  if [ -f "$WS/busybox-build/busybox.links" ]; then
    while IFS= read -r l; do
      case $l in ''|/bin/sh|/sbin/init|/sbin/mdev|/linuxrc) continue ;; esac
      echo "slink $l $(realpath -m -s --relative-to="$(dirname "$l")" /bin/busybox) 0777 0 0"
    done <"$WS/busybox-build/busybox.links" >>"$LIST"
  else
    echo "initramfs: no busybox.links: the busybox profile needs linux/userland/busybox.sh" >&2
    exit 1
  fi
  # Dropbear (linux/userland/dropbear.sh), if built: one multi-call binary.
  if [ -x "$DB" ]; then
    cat >>"$LIST" <<LIST
file /usr/sbin/dropbearmulti $DB 0755 0 0
slink /usr/sbin/dropbear dropbearmulti 0777 0 0
slink /usr/bin/dropbearkey ../sbin/dropbearmulti 0777 0 0
slink /usr/bin/dbclient ../sbin/dropbearmulti 0777 0 0
slink /usr/bin/scp ../sbin/dropbearmulti 0777 0 0
slink /usr/bin/dropbearconvert ../sbin/dropbearmulti 0777 0 0
LIST
  fi
  ;;
*) echo "unknown INITRAMFS_PROFILE $PROFILE" >&2; exit 1 ;;
esac
# INITRAMFS_EXTRA: a file of more gen_init_cpio lines (tests).
if [ -n "${INITRAMFS_EXTRA:-}" ]; then
  cat "$INITRAMFS_EXTRA" >>"$LIST"
fi
echo "initramfs ($PROFILE): $LIST"
