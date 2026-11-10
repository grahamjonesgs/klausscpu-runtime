#!/usr/bin/env bash
# linux/userland/coremark.sh — EEMBC CoreMark for the KlaussCPU Linux port
# (static, musl, the posix port), into linux-ws/coremark-build/coremark;
# build-rootfs.sh puts it in the system image.
#
# Iterations: the default, 0, makes CoreMark find a count that runs for
# at least 10 s (a valid result), which is the right thing on the board:
#   coremark                      # performance run, validated seeds
#   coremark 0x0 0x0 0x66 N       # N iterations
#   coremark 0x3415 0x3415 0x66 N # validation run
# The emulator check runs a few iterations, which validates the CRCs but
# is too short to be a valid score.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
REV=1f483d5b8316753a742cbf5590caf5bd0a4e4777
SRC=$WS/coremark
OUT=$WS/coremark-build
OPT=${COREMARK_OPT:--O2}
[ -f "$WS/sysroot/lib/libc.a" ] || { echo "no musl sysroot: run linux/musl/build.sh" >&2; exit 1; }
if [ "$(git -C "$SRC" rev-parse HEAD 2>/dev/null)" != "$REV" ]; then
  rm -rf "$SRC"; mkdir -p "$SRC"
  git -C "$SRC" init -q
  git -C "$SRC" fetch -q --depth 1 https://github.com/eembc/coremark.git "$REV"
  git -C "$SRC" checkout -q FETCH_HEAD
fi
rm -rf "$OUT"; mkdir -p "$OUT"
"$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot" -static $OPT \
  -I"$SRC" -I"$SRC/posix" -DPERFORMANCE_RUN=1 -DITERATIONS=0 -DUSE_CLOCK=1 \
  -DFLAGS_STR="\"$OPT -static (clang klausscpu, musl)\"" \
  "$SRC"/core_list_join.c "$SRC"/core_main.c "$SRC"/core_matrix.c "$SRC"/core_state.c \
  "$SRC"/core_util.c "$SRC"/posix/core_portme.c -o "$OUT/coremark"
echo "coremark: $OUT/coremark ($OPT, CoreMark $REV)"
