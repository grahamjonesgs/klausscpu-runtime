#!/usr/bin/env bash
# csmith_run.sh <first-seed> <count> <outdir> [opt ...]
#
# Differential test on the emulator: the checksum of each Csmith program built
# by host gcc -O0 against KlaussCPU clang at each optimisation level (default
# -O0 -O1 -O2 -Os), run on the klausscc emulator.  See csmith_lib.sh for the
# undefined-behaviour screen and the environment variables.
#
# Appends to <outdir>/results.tsv one line per seed and level:
#   seed  opt  status  host-checksum  klausscpu-checksum
# status: ok, MISMATCH, no-output, cc-fail, ld-fail, instr-cap (the program
# ran past the instruction cap: not a failure), or, for the whole seed,
# gen-fail, host-cc-fail, host-timeout, host-ub (discarded).  The source and
# outputs of every seed with a failure are kept in <outdir>/fail/<seed>/.
# Prints the status counts; exits 1 if any seed failed.
set -uo pipefail
. "$(dirname "$0")/common.sh"
. "$(dirname "$0")/csmith_lib.sh"
[ $# -ge 3 ] || die "usage: csmith_run.sh <first-seed> <count> <outdir> [opt ...]"
FIRST=$1; COUNT=$2; OUT=$(realpath -m "$3"); shift 3
OPTS=${*:-"-O0 -O1 -O2 -Os"}
MAXI=${EMU_MAX_INSTRUCTIONS:-2000000000}
mkdir -p "$OUT/fail" "$OUT/work"
csmith_rt "$OUT"
one() {
  local seed=$1 W=$OUT/work/$1 bad=0 O got
  mkdir -p "$W"; cd "$W" || return
  if csmith_gen "$seed"; then
    for O in $OPTS; do
      got=-
      if csmith_build "$O"; then
        emu "$MAXI" "t$O.elf" "emu$O.out"
        got=$(checksum_of "emu$O.out")
        if [ -z "$got" ] && grep -q 'stop = InstructionCap' "emu$O.out"; then ST=instr-cap; got=-
        elif [ -z "$got" ]; then ST=no-output; got=-
        elif [ "$got" = "$REF" ]; then ST=ok
        else ST=MISMATCH; fi
      fi
      case $ST in ok|instr-cap) ;; *) bad=1 ;; esac
      printf '%s\t%s\t%s\t%s\t%s\n' "$seed" "$O" "$ST" "$REF" "$got"
    done
  fi
  if [ $bad = 1 ]; then mkdir -p "$OUT/fail/$seed"; cp t.c ./*.err emu*.out "$OUT/fail/$seed/" 2>/dev/null; fi
  cd "$OUT" && rm -rf "$W"
}
export -f one csmith_gen csmith_build checksum_of emu
export OUT OPTS MAXI LLVM_BIN RT KLAUSSCC CSMITH CSI CFLAGS RTOBJS PICO CSMITH_OPTS
seq "$FIRST" $((FIRST + COUNT - 1)) | xargs -P "$JOBS" -I{} bash -c 'one {}' >>"$OUT/results.tsv"
awk -F'\t' '{c[$3]++} END{for (k in c) print k, c[k]}' "$OUT/results.tsv" | sort
! awk -F'\t' '$3!="ok" && $3!="instr-cap" && $3!="host-timeout" && $3!="host-ub" && $3!="gen-fail" && $3!="host-cc-fail"' "$OUT/results.tsv" | grep -q .
