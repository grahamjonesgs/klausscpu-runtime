#!/usr/bin/env bash
# csmith_run.sh <first-seed> <count> <outdir> [opt ...]
#
# Differential test on the emulator: the checksum of each Csmith program built
# by host gcc -O0 against KlaussCPU clang at each optimisation level (default
# -O0 -O1 -O2 -Os -Oz; -Oz runs the MachineOutliner), run on the klausscc
# emulator.  A level can carry more flags joined by '+' ("-Os+-moutline").
# See csmith_lib.sh for the undefined-behaviour screen and the environment
# variables.
#
# Appends to <outdir>/results.tsv one line per seed and level:
#   seed  opt  status  host-checksum  klausscpu-checksum  instructions
# status:
#   ok            halted with the host's checksum
#   ok-long       ran past the instruction cap while another level of the same
#                 seed halted, then halted with the right checksum under the
#                 larger EMU_RETRY_MAX cap (csmith_recheck in csmith_lib.sh)
#   instr-cap     ran past the cap, and so did every other level: a long
#                 program, not a failure
#   HANG          still running at EMU_RETRY_MAX while another level halted:
#                 a likely miscompiled infinite loop
#   MISMATCH, no-output, stop-<reason> (a trap or other abnormal end, even
#                 after a good checksum), cc-fail, ld-fail
#   gen-fail, host-cc-fail, host-timeout, host-ub (for the whole seed:
#                 discarded)
# The source and outputs of every seed with a failure are kept in
# <outdir>/fail/<seed>/.  Prints the status counts; exits 1 if any seed failed.
set -uo pipefail
. "$(dirname "$0")/common.sh"
. "$(dirname "$0")/csmith_lib.sh"
[ $# -ge 3 ] || die "usage: csmith_run.sh <first-seed> <count> <outdir> [opt ...]"
FIRST=$1; COUNT=$2; OUT=$(realpath -m "$3"); shift 3
OPTS=${*:-"-O0 -O1 -O2 -Os -Oz"}
MAXI=${EMU_MAX_INSTRUCTIONS:-2000000000}
mkdir -p "$OUT/fail" "$OUT/work"
csmith_rt "$OUT"
one() {
  local seed=$1 W=$OUT/work/$1 bad=0 halted=0 O i
  local -a opt st got cnt
  mkdir -p "$W"; cd "$W" || return
  if csmith_gen "$seed"; then
    i=0
    for O in $OPTS; do
      G=-; NI=-
      if csmith_build "$O"; then
        emu "$MAXI" "t$O.elf" "emu$O.out"
        csmith_classify "emu$O.out"
        [ "$ST" = ok ] && halted=1
      fi
      opt[i]=$O; st[i]=$ST; got[i]=$G; cnt[i]=$NI; i=$((i + 1))
    done
    for i in "${!opt[@]}"; do
      if [ "${st[i]}" = instr-cap ] && [ $halted = 1 ]; then
        csmith_recheck "${opt[i]}"; st[i]=$ST; got[i]=$G; cnt[i]=$NI
      fi
      case ${st[i]} in ok|ok-long|instr-cap) ;; *) bad=1 ;; esac
      printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$seed" "${opt[i]}" "${st[i]}" "$REF" "${got[i]}" "${cnt[i]}"
    done
  fi
  if [ $bad = 1 ]; then mkdir -p "$OUT/fail/$seed"; cp t.c ./*.err emu*.out t*.elf "$OUT/fail/$seed/" 2>/dev/null; fi
  cd "$OUT" && rm -rf "$W"
}
export -f one csmith_gen csmith_build csmith_classify csmith_recheck checksum_of emu emu_instructions stop_of
export OUT OPTS MAXI LLVM_BIN RT KLAUSSCC CSMITH CSI CFLAGS RTOBJS PICO CSMITH_OPTS
seq "$FIRST" $((FIRST + COUNT - 1)) | xargs -P "$JOBS" -I{} bash -c 'one {}' >>"$OUT/results.tsv"
awk -F'\t' '{c[$3]++} END{for (k in c) print k, c[k]}' "$OUT/results.tsv" | sort
[ -s "$OUT/no-strict-aliasing.txt" ] && echo "no-strict-aliasing $(sort -u "$OUT/no-strict-aliasing.txt" | wc -l)"
! awk -F'\t' '$3 !~ /^(ok|ok-long|instr-cap|host-timeout|host-ub|gen-fail|host-cc-fail)$/' "$OUT/results.tsv" | grep -q .
