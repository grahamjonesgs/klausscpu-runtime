#!/usr/bin/env bash
# linux/build.sh [make targets...] — build the kernel out of tree in
# linux-ws/build with klausscpu-llvm.  Default target: vmlinux.
#   KLAUSSCPU_LLVM_BIN  toolchain bin dir (default ../klausscpu-llvm/build/bin)
#   JOBS                parallel jobs (default nproc)
#   LINUX_BUILD         output directory (default linux-ws/build)
#   INITRAMFS_PROFILE   m3 | musltest | busybox (linux/initramfs/build.sh)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
OUT=${LINUX_BUILD:-$WS/build}
mkdir -p "$OUT"
KLAUSSCPU_LLVM_BIN="$B" "$HERE/initramfs/build.sh" "$OUT" >/dev/null
exec make -C "$WS/linux" O="$OUT" ARCH=klausscpu LLVM="$B/" LLVM_IAS=1 \
  HOSTCC=gcc HOSTCXX=g++ HOSTLD=ld HOSTAR=ar \
  YACC="$HERE/tools/prebuilt-parser.py" LEX="$HERE/tools/prebuilt-parser.py" \
  -j"${JOBS:-$(nproc)}" "${@:-vmlinux}"
