#!/usr/bin/env bash
# bench.sh <outdir> [OPT ...]   (default: -O2 -Os)
#
# CoreMark and Embench-IoT on the board: baremetal/benchmarks/run-benchmarks.sh
# in a staged copy of baremetal/ (every board command under flock
# $BOARD_LOCK), results copied to <outdir>/<OPT without the dash>/
# (coremark.csv, embench.csv, summary.txt, the UART captures).  The upstream
# sources are linked from the checkout's baremetal/benchmarks/ when fetched
# there, and fetched into the copy otherwise (get-benchmarks.sh).
# Exits with run-benchmarks.sh's status: 1 unless every CoreMark CRC and
# every Embench verification passed.
set -uo pipefail
. "$(dirname "$0")/common.sh"
[ $# -ge 1 ] || die "usage: bench.sh <outdir> [OPT ...]"
OUT=$(realpath -m "$1"); shift
OPTS=("$@"); [ ${#OPTS[@]} -eq 0 ] && OPTS=(-O2 -Os)
rm -rf "$OUT"; mkdir -p "$OUT"
stage_baremetal "$OUT"
if [ ! -e "$STAGED_BM/benchmarks/coremark/coremark.h" ] || [ ! -d "$STAGED_BM/benchmarks/embench-iot/src" ]; then
  "$STAGED_BM/benchmarks/get-benchmarks.sh" >"$OUT/fetch.log" 2>&1 || die "get-benchmarks.sh failed (see $OUT/fetch.log)"
fi
"$STAGED_BM/benchmarks/run-benchmarks.sh" "${OPTS[@]}" >"$OUT/run.log" 2>&1; rc=$?
for O in "${OPTS[@]}"; do
  D=$(echo "$O" | tr ' ' _ | tr -d -)
  cp -r "$STAGED_BM/bench-results/$D" "$OUT/$D" 2>/dev/null
  mkdir -p "$OUT/$D/elf"
  cp "$STAGED_BM/bench-build/$D"/*.elf "$OUT/$D/elf/" 2>/dev/null
  cp "$STAGED_BM/bench-build/$D"/embench/*.elf "$OUT/$D/elf/" 2>/dev/null
done
rm -rf "$OUT/tree"
for O in "${OPTS[@]}"; do
  D=$(echo "$O" | tr ' ' _ | tr -d -)
  cm=$(grep -ao 'CoreMark 1.0 : [0-9.]*' "$OUT/$D/summary.txt" 2>/dev/null | awk '{printf "%.2f", $4}')
  eb=$(grep -ho 'speed geomean [0-9.]*\|size geomean [0-9.]*' "$OUT/$D/summary.txt" 2>/dev/null | tr '\n' ' ')
  np=$(grep -c ',PASS,' "$OUT/$D/embench.csv" 2>/dev/null)
  echo "$O: CoreMark ${cm:-?} ($(grep -c ',PASS$' "$OUT/$D/coremark.csv" 2>/dev/null)/2 PASS), Embench ${eb}($np/19 PASS)"
done
exit $rc
