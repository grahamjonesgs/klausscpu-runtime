#!/usr/bin/env bash
# linux/build.sh [make targets...] — build the kernel out of tree in
# linux-ws/build with klausscpu-llvm.  Default target: vmlinux.
#   KLAUSSCPU_LLVM_BIN  toolchain bin dir (default ../klausscpu-llvm/build/bin)
#   JOBS                parallel jobs (default nproc)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
mkdir -p "$WS/build"
KLAUSSCPU_LLVM_BIN="$B" "$HERE/initramfs/build.sh" "$WS/build" >/dev/null
exec make -C "$WS/linux" O="$WS/build" ARCH=klausscpu LLVM="$B/" LLVM_IAS=1 \
  HOSTCC=gcc HOSTCXX=g++ HOSTLD=ld HOSTAR=ar \
  YACC="$HERE/tools/prebuilt-parser.py" LEX="$HERE/tools/prebuilt-parser.py" \
  -j"${JOBS:-$(nproc)}" "${@:-vmlinux}"
