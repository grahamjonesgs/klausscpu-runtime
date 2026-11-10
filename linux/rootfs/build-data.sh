#!/usr/bin/env bash
# linux/rootfs/build-data.sh <out.img> [size MiB, default 256]
#
# The persistent data image for the SD card, /linux/data.img: ext4 with a
# journal (the board is reset without a clean shutdown), mounted read-write
# at /data by the initramfs's sdinit.  /root and /home of the read-only
# system image link into it, and /data/etc holds local settings:
#   network   sourced by rcS: HOSTNAME, and IP/NETMASK/GATEWAY/DNS for a
#             static address instead of DHCP
#   rc.local  sourced at the end of rcS
# Made once and put on the card with install-data, which won't overwrite an
# existing data.img; install-rootfs never touches it.  Errors make the file
# system read-only (-e remount-ro) rather than continue.
set -euo pipefail
umask 022
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
OUTIMG=$(realpath -m "$1")
SIZE=${2:-256}
ST=$WS/data-staging

rm -rf "$ST"
mkdir -p "$ST"/{root,home,etc}
chmod 700 "$ST/root"
cat >"$ST/README" <<'EOF'
KlaussCPU Linux: persistent data (/linux/data.img on the SD card, at /data)

The system image (/linux/rootfs.img) is read-only and replaced whole by
install-rootfs; what is here survives that:
  root/   the root user's home (/root links here)
  home/   other homes (/home links here)
  etc/    local settings, read at boot by /etc/init.d/rcS:
            network   shell variables (see network.example)
            rc.local  a shell script sourced at the end of rcS
Everything else written at run time (/tmp, /run, /var) is in RAM.
EOF
cat >"$ST/etc/network.example" <<'EOF'
# Copy to /data/etc/network to use.  Sourced by /etc/init.d/rcS.
#HOSTNAME=klausscpu
# A static address instead of DHCP:
#IP=192.168.68.59
#NETMASK=255.255.255.0
#GATEWAY=192.168.68.1
#DNS="192.168.68.1"
EOF
cat >"$ST/etc/rc.local.example" <<'EOF'
# Copy to /data/etc/rc.local to use: sourced (not run) at the end of
# /etc/init.d/rcS, as root, after the network and Dropbear are up.
#echo "rc.local: hello"
EOF

tar --owner=0 --group=0 --numeric-owner -C "$ST" -cf "$WS/data.tar" .
rm -f "$OUTIMG"
mke2fs -q -t ext4 -L klausscpu-data -m 0 -e remount-ro -d "$WS/data.tar" "$OUTIMG" "${SIZE}M"
e2fsck -fn "$OUTIMG" >/dev/null 2>&1 || { echo "e2fsck: $OUTIMG is not clean" >&2; exit 1; }
echo "data: $OUTIMG (${SIZE} MiB)"
