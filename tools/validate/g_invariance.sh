#!/usr/bin/env bash
# g_invariance.sh <first-seed> <count> <outdir> [level ...]
#
# Debug info must not change code: compile each Csmith program at each level
# (default -Oz -Os+-moutline; a level's extra flags are joined by '+', as in
# csmith_run.sh) with -g and with -g0 and compare the two objects' .text.
# The MachineOutliner is where this broke before (-Oz: -g's DBG_VALUEs
# renumbered the outliner's instruction mapping and changed which of two
# equally good candidates it took); a single lit case cannot cover that.
# Nothing runs: no emulator, no host build.
#
# Writes <outdir>/results.tsv (seed, level, same|DIFF|cc-fail, the number
# of OUTLINED_FUNCTION symbols at -g0) and keeps the source and both objects
# of every DIFF in <outdir>/fail/<seed><level>/.  Prints the counts; exits 1
# if any seed differs or fails to compile.
set -uo pipefail
. "$(dirname "$0")/common.sh"
. "$(dirname "$0")/csmith_lib.sh"
[ $# -ge 3 ] || die "usage: g_invariance.sh <first-seed> <count> <outdir> [level ...]"
FIRST=$1; COUNT=$2; OUT=$(realpath -m "$3"); shift 3
LEVELS=${*:-"-Oz -Os+-moutline"}
mkdir -p "$OUT/work" "$OUT/fail"
one() {
  local seed=$1 W=$OUT/work/$1 L O g n st
  mkdir -p "$W"; cd "$W" || return
  "$CSMITH" --no-packed-struct --seed "$seed" >t.c 2>/dev/null || { echo "$seed	-	gen-fail	-"; return; }
  for L in $LEVELS; do
    O=${L//+/ }
    st=same
    for g in -g -g0; do
      # shellcheck disable=SC2086
      "$LLVM_BIN/clang" $CFLAGS -I"$CSI" $O $g -c t.c -o "t$g.o" 2>"cc$g.log" &&
        "$LLVM_BIN/llvm-objcopy" -O binary --only-section=.text "t$g.o" "t$g.bin" ||
        st=cc-fail
    done
    [ $st = same ] && ! cmp -s t-g.bin t-g0.bin && st=DIFF
    n=$("$LLVM_BIN/llvm-nm" t-g0.o 2>/dev/null | grep -c OUTLINED_FUNCTION)
    if [ $st != same ]; then
      mkdir -p "$OUT/fail/$seed$L"; cp t.c t-g.o t-g0.o cc-g.log cc-g0.log "$OUT/fail/$seed$L/" 2>/dev/null
    fi
    printf '%s\t%s\t%s\t%s\n' "$seed" "$L" "$st" "$n"
  done
  cd "$OUT" && rm -rf "$W"
}
export -f one; export OUT LEVELS CSMITH CSI CFLAGS LLVM_BIN
seq "$FIRST" $((FIRST + COUNT - 1)) | xargs -P "$JOBS" -I{} bash -c 'one {}' >>"$OUT/results.tsv"
rm -rf "$OUT/work"
awk -F'\t' '{c[$3]++; o+=$4} END {for (k in c) printf "%s %d, ", k, c[k]; printf "%d outlined functions at -g0\n", o}' "$OUT/results.tsv"
! grep -qP '\t(DIFF|cc-fail)\t' "$OUT/results.tsv"
