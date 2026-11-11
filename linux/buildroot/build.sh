#!/usr/bin/env bash
# linux/buildroot/build.sh [make targets]: the packages of
# linux/buildroot/external/configs/klausscpu_defconfig, built by Buildroot
# (linux/buildroot/fetch.sh's tree) with our clang (toolchain.sh) into
# linux-ws/br-output; the result is linux-ws/br-output/images/rootfs.tar,
# which build-rootfs.sh merges into the system image with
# ROOTFS_BUILDROOT=1.  Downloads go to linux-ws/br-dl.
# BR_SHARED=1 (DYNLINK_PLAN M7): the same packages with shared libraries,
# in linux-ws/br-output-dyn with linux-ws/br-toolchain-dyn (toolchain.sh):
# the defconfig with BR2_SHARED_LIBS instead of BR2_STATIC_LIBS, plus
# Python 3.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=$(realpath -m "${LINUX_WS:-$RT/linux-ws}")
export KLAUSSCPU_WS=$WS
[ -f "$WS/buildroot/Makefile" ] || "$HERE/fetch.sh"
if [ "${BR_SHARED:-0}" = 1 ]; then
  TC=$WS/br-toolchain-dyn BO=$WS/br-output-dyn
else
  TC=$WS/br-toolchain BO=$WS/br-output
fi
[ -x "$TC/bin/klausscpu-linux-musl-gcc" ] || "$HERE/toolchain.sh"
mkdir -p "$BO" "$WS/br-dl"
MK=(make -C "$WS/buildroot" O="$BO" BR2_EXTERNAL="$HERE/external")
if [ ! -f "$BO/.config" ]; then
  "${MK[@]}" klausscpu_defconfig
  if [ "${BR_SHARED:-0}" = 1 ]; then
    sed -i -e 's|^BR2_STATIC_LIBS=y|# BR2_STATIC_LIBS is not set|' \
      -e 's|^# BR2_SHARED_LIBS is not set|BR2_SHARED_LIBS=y|' \
      -e 's|^BR2_TOOLCHAIN_EXTERNAL_PATH=.*|BR2_TOOLCHAIN_EXTERNAL_PATH="$(KLAUSSCPU_WS)/br-toolchain-dyn"|' "$BO/.config"
    # Packages that refuse a static-only build (DYNLINK_PLAN Goal).
    # Its optional modules whose libraries the image has anyway (zlib,
    # OpenSSL, SQLite, bzip2, xz, readline/ncurses).
    printf '%s\n' BR2_PACKAGE_PYTHON3=y BR2_PACKAGE_PYTHON3_ZLIB=y BR2_PACKAGE_PYTHON3_SSL=y \
      BR2_PACKAGE_PYTHON3_SQLITE=y BR2_PACKAGE_PYTHON3_BZIP2=y BR2_PACKAGE_PYTHON3_XZ=y \
      BR2_PACKAGE_PYTHON3_READLINE=y BR2_PACKAGE_PYTHON3_CURSES=y >>"$BO/.config"
    "${MK[@]}" olddefconfig
    grep -q '^BR2_PACKAGE_PYTHON3=y' "$BO/.config" || { echo "build.sh: python3 not selectable" >&2; exit 1; }
    grep -q '^BR2_SHARED_LIBS=y' "$BO/.config" || { echo "build.sh: BR2_SHARED_LIBS did not stick" >&2; exit 1; }
  fi
fi
"${MK[@]}" "${@:-all}"
