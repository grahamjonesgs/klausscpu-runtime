#!/usr/bin/env bash
# compare.sh <base> <new>
#
# Compare the UART output of every program in <new> (an emu_suite.sh or
# board_suite.sh output dir) with <base>, which is either another output dir
# (<prog>.out captures) or a committed baseline dir (<prog>.txt, already
# filtered, and stop.tsv; see baselines/).  Lines that legitimately change
# with code generation or timing are filtered out first (uart_filter in
# common.sh).
#
# How each run ended must match too (stop_of in common.sh: Halt, the
# emulator's InstructionCap, Trap, ...): a program that used to halt and now
# hangs or traps fails with "STOP <prog>: Halt -> <new>", even when its output
# up to that point is identical.  A program the base run stopped at the
# instruction cap (one that never halts on the emulator: the network demos,
# adventure) is compared on its first 12 filtered lines; every other program
# on its whole output.
#
# Then it reports the emulator instruction count and .text size of every
# program whose numbers changed (stats.tsv in both), the totals over the
# programs that halt, and WARNs about any program that grew by more than
# STATS_TOL percent (default 2).
#
# Exit status: 1 if any output or stop reason differs, or a program is missing
# from either side, else 0 (size and instruction-count changes do not fail the
# comparison).
set -uo pipefail
. "$(dirname "$0")/common.sh"
[ $# -eq 2 ] || die "usage: compare.sh <base> <new>"
B=$1; N=$2; rc=0; ndiff=0; nstop=0; nprog=0
TOL=${STATS_TOL:-2}
# base_stop <prog> <base-file> -- how the base run of <prog> ended: from the
# capture itself, from a baseline's stop.tsv, or (older baselines) from
# stats.tsv, where a numeric instruction count means it halted.  Empty when
# the base does not say (then only the text is compared).
base_stop() {
  case $2 in
    *.out) stop_of "$2" ;;
    *) if [ -f "$B/stop.tsv" ]; then awk -F'\t' -v p="$1" '$1==p {print $2}' "$B/stop.tsv"
       elif [ -f "$B/stats.tsv" ]; then
         awk -F'\t' -v p="$1" '$1==p {print ($2 ~ /^[0-9]+$/) ? "Halt" : "InstructionCap"}' "$B/stats.tsv"
       fi ;;
  esac
}
# A run dir holds <prog>.out captures (and bookkeeping .txt files: summary,
# repeat, build_failed, ...); a baseline dir holds <prog>.txt.
if compgen -G "$B/*.out" >/dev/null; then ext=out; else ext=txt; fi
for f in "$B"/*."$ext"; do
  [ -e "$f" ] || continue
  n=$(basename "$f" ."$ext")
  case $n in summary|build_errors|build_failed|repeat|toolchain|stats|stop) continue ;; esac
  nprog=$((nprog + 1))
  if [ ! -f "$N/$n.out" ]; then echo "MISSING $n"; rc=1; continue; fi
  s0=$(base_stop "$n" "$f"); s1=$(stop_of "$N/$n.out")
  if [ -n "$s0" ] && [ "$s0" != "$s1" ]; then
    echo "STOP $n: $s0 -> $s1"; rc=1; nstop=$((nstop + 1))
  fi
  lim=100000; [ "$s0" = InstructionCap ] && lim=12
  if [ $ext = txt ]; then
    d=$(diff <(head -"$lim" "$f") <(uart_filter "$N/$n.out" | head -"$lim"))
  else
    d=$(diff <(uart_filter "$f" | head -"$lim") <(uart_filter "$N/$n.out" | head -"$lim"))
  fi
  if [ -n "$d" ]; then echo "=== DIFF $n"; echo "$d" | head -20; rc=1; ndiff=$((ndiff + 1)); fi
done
for f in "$N"/*.out; do
  [ -e "$f" ] || continue
  n=$(basename "$f" .out)
  if [ ! -e "$B/$n.out" ] && [ ! -e "$B/$n.txt" ]; then
    echo "NOT IN BASE $n (a new program: update the baseline)"; rc=1
  fi
done
echo "uart: $nprog programs, $ndiff differ, $nstop stop differently"
if [ -f "$B/stats.tsv" ] && [ -f "$N/stats.tsv" ]; then
  awk -F'\t' -v tol="$TOL" '
    NR==FNR { bi[$1]=$2; bt[$1]=$3; next }
    ($1 in bi) {
      line = ""
      if (bi[$1] != $2) line = line sprintf(" instr %s -> %s", bi[$1], $2)
      if (bt[$1] != $3) line = line sprintf(" .text %s -> %s", bt[$1], $3)
      w = ""
      num0 = (bi[$1] ~ /^[0-9]+$/); num1 = ($2 ~ /^[0-9]+$/)
      if (num0 && num1 && bi[$1] > 0 && $2 > bi[$1] * (1 + tol/100)) w = w " WARN:instr+" sprintf("%.2f%%", ($2/bi[$1]-1)*100)
      if (bt[$1] > 0 && $3 > bt[$1] * (1 + tol/100)) w = w " WARN:text+" sprintf("%.2f%%", ($3/bt[$1]-1)*100)
      if (line != "") print "  " $1 line w
      if (num0 && num1) { si0 += bi[$1]; si1 += $2 }
      st0 += bt[$1]; st1 += $3
    }
    END {
      if (si0 > 0) printf "instructions (halting programs): %.0f -> %.0f (%+.2f%%)\n", si0, si1, (si1/si0-1)*100
      if (st0 > 0) printf ".text total: %.0f -> %.0f (%+.2f%%)\n", st0, st1, (st1/st0-1)*100
    }' "$B/stats.tsv" "$N/stats.tsv"
fi
if [ -f "$B/build_errors.txt" ] && [ -f "$N/build_errors.txt" ]; then
  echo "build errors: base $(cat "$B/build_errors.txt") new $(cat "$N/build_errors.txt")"
  [ "$(cat "$N/build_errors.txt")" = 0 ] || rc=1
fi
exit $rc
