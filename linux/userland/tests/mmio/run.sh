#!/usr/bin/env bash
# linux/userland/tests/mmio/run.sh [outdir] — build mmio-test, the test of
# /dev/klausscpu-mmio (arch/klausscpu/kernel/klausscpu_mmio.c), for the
# board: <outdir>/mmio-test (default linux-ws/mmio-test), static, so it runs
# on the SD system and in the busybox initramfs alike.
#
# Board: scp it over and run it as root; it ends with "mmio-test: PASS".
# The LED bar shows 0x5A5A for a moment and is restored.
# Emulator: tools/validate/linux.sh <out> busybox builds and runs it.
#
#   KLAUSSCPU_LLVM_BIN  toolchain (default ../klausscpu-llvm/build/bin)
#   KLAUSSCPU_SYSROOT   musl sysroot (default linux-ws/dyn/sysroot)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../../.." && pwd)
OUT=${1:-$RT/linux-ws/mmio-test}
mkdir -p "$OUT"
# -idirafter: <asm/klausscpu-mmio.h> from the kernel tree, without letting
# the arch's other uapi headers shadow the sysroot's.
"$RT/linux/rustc/klausscpu-clang" -O2 -Wall -Werror -static \
  -idirafter "$RT/linux/arch/klausscpu/include/uapi" \
  "$HERE/mmio-test.c" -o "$OUT/mmio-test"
echo "built $OUT/mmio-test"
