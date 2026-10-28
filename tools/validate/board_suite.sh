#!/usr/bin/env bash
# board_suite.sh <outdir> [OPT]
#
# Build the self-checking bare-metal programs at OPT (default -O1) in a staged
# copy, plus Dhrystone again with -fno-builtin (dhrystone_nobuiltin: the
# like-for-like figure, since strcpy/strcmp became compiler built-ins), and
# run each on the FPGA board over the UART, every run under flock $BOARD_LOCK.
# Each program runs $BOARD_RUNS times (default 2); the runs must print the
# same filtered output, and their cycle counts agree to within a few cycles
# per kernel (perf.py checks the spread).
#   <outdir>/<prog>.out          first run's capture
#   <outdir>/run<N>/<prog>.out   later runs
#   <outdir>/summary.txt         "<prog> rc=<status> halted=<0|1> <bytes>B"
#   <outdir>/repeat.txt          differences between the runs (empty if none)
#   <outdir>/elf/                the ELFs that ran
# perf.py turns the captures into numbers; compare.sh compares the UART text.
set -uo pipefail
. "$(dirname "$0")/common.sh"
[ $# -ge 1 ] || die "usage: board_suite.sh <outdir> [OPT]"
OUT=$(realpath -m "$1"); OPT=${2:--O1}
RUNS=${BOARD_RUNS:-2}
PROGS=${BOARD_PROGS:-"hello test_64bit test_varargs test_printf test_fp test_switch test_asm queens bst expr test_big test_cache dhrystone perf_baseline perf_haz crypto_selftest test_rtos test_sync test_atomic test_divshadow test_flags test_unaligned"}
# Programs that never halt on the board (they idle after printing the result).
timeout_for() {
  case $1 in
    test_rtos|test_sync) echo 12 ;;
    perf_baseline|perf_haz) echo 60 ;;
    *) echo "${BOARD_TIMEOUT:-40}" ;;
  esac
}
rm -rf "$OUT"; mkdir -p "$OUT/elf"
stage_baremetal "$OUT"
# shellcheck disable=SC2086
# shellcheck disable=SC2046
make -C "$STAGED_BM" -j"$JOBS" -k $(for p in $PROGS; do echo "$p.elf"; done) OPT="$OPT" >"$OUT/build.log" 2>&1
for p in $PROGS; do cp "$STAGED_BM/$p.elf" "$OUT/elf/" 2>/dev/null || echo "BUILD FAILED $p" >>"$OUT/build_failed.txt"; done
if [[ " $PROGS " == *" dhrystone "* ]]; then
  # Not `make clean`: that also deletes objects under ../src, which is the
  # checkout's own src/ (a symlink in the staged tree).
  rm -f "$STAGED_BM"/*.o "$STAGED_BM"/*.d "$STAGED_BM"/*.elf
  make -C "$STAGED_BM" -j"$JOBS" dhrystone.elf OPT="$OPT -fno-builtin" >"$OUT/build_nobuiltin.log" 2>&1 &&
    cp "$STAGED_BM/dhrystone.elf" "$OUT/elf/dhrystone_nobuiltin.elf"
fi
rm -rf "$OUT/tree"
echo "toolchain $(toolchain_id) OPT=$OPT" >"$OUT/toolchain.txt"
: >"$OUT/summary.txt"; : >"$OUT/repeat.txt"
for r in $(seq 1 "$RUNS"); do
  D=$OUT; [ "$r" = 1 ] || D=$OUT/run$r
  mkdir -p "$D"
  for e in "$OUT"/elf/*.elf; do
    p=$(basename "$e" .elf)
    board "$(timeout_for "${p%_nobuiltin}")" "$e" "$D/$p.out"; st=$?
    [ "$r" = 1 ] && echo "$p rc=$st halted=$(grep -ac 'CPU halted' "$D/$p.out") $(wc -c <"$D/$p.out")B" >>"$OUT/summary.txt"
    if [ "$r" != 1 ]; then
      # The runs must agree on the filtered output.  Cycle counts of kernels
      # that miss the cache vary by a few cycles (DDR refresh); perf.py
      # reports that spread.
      if ! diff -q <(uart_filter "$OUT/$p.out") <(uart_filter "$D/$p.out") >/dev/null; then
        echo "run$r $p differs from run 1" >>"$OUT/repeat.txt"
      fi
    fi
  done
done
exit 0
