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
# up to that point is identical.
#
# A program the base run stopped at the instruction cap (InstructionCap in
# stop.tsv: one that idles or waits for a peripheral the emulator does not
# model) prints more or less before the cap, and in a different interleaving,
# as the code gets faster or slower.  Such a program is compared on its first
# 12 filtered lines (in any order: "ORDER <prog>" is listed but does not
# fail), plus its self-check verdicts anywhere in the output
# (verdicts in common.sh: the distinct lines with a PASS or FAIL word or
# "Result:", digits ignored): every verdict of the base must appear in the new
# run ("VERDICT <prog> lost: ..."; e.g. test_rtos's "Result: PASS", which
# comes on line 39), and a new verdict that says FAIL must appear in the base
# ("VERDICT <prog> new: ...").  A new verdict without FAIL (a faster run now
# reaches a later check) is listed as "VERDICT <prog> extra" but does not
# fail.  Every other program is compared on its whole output.
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
# shellcheck disable=SC2034  # read by common.sh
VALIDATE_TEXT_ONLY=1
. "$(dirname "$0")/common.sh"
[ $# -eq 2 ] || die "usage: compare.sh <base> <new>"
B=$1; N=$2; rc=0; ndiff=0; nstop=0; nprog=0; nmiss=0
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
  if [ ! -f "$N/$n.out" ]; then echo "MISSING $n"; rc=1; nmiss=$((nmiss + 1)); continue; fi
  s0=$(base_stop "$n" "$f"); s1=$(stop_of "$N/$n.out")
  if [ -n "$s0" ] && [ "$s0" != "$s1" ]; then
    echo "STOP $n: $s0 -> $s1"; rc=1; nstop=$((nstop + 1))
  fi
  if [ $ext = txt ]; then text0() { cat "$f"; }; else text0() { uart_filter "$f"; }; fi
  lim=100000; [ "$s0" = InstructionCap ] && lim=12
  d=$(diff <(text0 | head -"$lim") <(uart_filter "$N/$n.out" | head -"$lim"))
  if [ "$s0" = InstructionCap ]; then
    # The same lines in another order: preemptive tasks interleave by timer
    # ticks, so test_rtos's task lines move with the code's speed.
    if [ -n "$d" ] && diff -q <(text0 | head -"$lim" | LC_ALL=C sort) \
        <(uart_filter "$N/$n.out" | head -"$lim" | LC_ALL=C sort) >/dev/null; then
      echo "ORDER $n: the first $lim lines are the same lines in another order (not a failure)"; d=
    fi
    # comm needs both inputs in the same collation as verdicts' sort.
    v0=$(text0 | verdicts); v1=$(uart_filter "$N/$n.out" | verdicts)
    lost=$(LC_ALL=C comm -23 <(printf '%s\n' "$v0") <(printf '%s\n' "$v1") | grep .)
    new=$(LC_ALL=C comm -13 <(printf '%s\n' "$v0") <(printf '%s\n' "$v1") | grep .)
    newfail=$(printf '%s\n' "$new" | grep -aE '(^|[^A-Za-z])FAIL')
    extra=$(printf '%s\n' "$new" | grep -avE '(^|[^A-Za-z])FAIL' | grep .)
    [ -n "$lost" ] && d+=$'\n'$(printf '%s\n' "$lost" | sed "s/^/VERDICT $n lost: /")
    [ -n "$newfail" ] && d+=$'\n'$(printf '%s\n' "$newfail" | sed "s/^/VERDICT $n new: /")
    [ -n "$extra" ] && printf '%s\n' "$extra" | sed "s/^/VERDICT $n extra (not a failure): /"
    d=$(printf '%s\n' "$d" | grep .)
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
echo "uart: $nprog programs, $ndiff differ, $nstop stop differently, $nmiss missing"
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
