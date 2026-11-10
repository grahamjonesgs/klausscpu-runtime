#!/usr/bin/env bash
# linux/buildroot/build.sh [make targets]: the packages of
# linux/buildroot/external/configs/klausscpu_defconfig, built by Buildroot
# (linux/buildroot/fetch.sh's tree) with our clang (toolchain.sh) into
# linux-ws/br-output; the result is linux-ws/br-output/images/rootfs.tar,
# which build-rootfs.sh merges into the system image with
# ROOTFS_BUILDROOT=1.  Downloads go to linux-ws/br-dl.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=$(realpath -m "${LINUX_WS:-$RT/linux-ws}")
export KLAUSSCPU_WS=$WS
[ -f "$WS/buildroot/Makefile" ] || "$HERE/fetch.sh"
[ -x "$WS/br-toolchain/bin/klausscpu-linux-musl-gcc" ] || "$HERE/toolchain.sh"
mkdir -p "$WS/br-output" "$WS/br-dl"
MK=(make -C "$WS/buildroot" O="$WS/br-output" BR2_EXTERNAL="$HERE/external")
[ -f "$WS/br-output/.config" ] || "${MK[@]}" klausscpu_defconfig
"${MK[@]}" "${@:-all}"
