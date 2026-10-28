#!/usr/bin/env bash
# csmith_board.sh <first-seed> <count> <outdir> [opt ...]
#
# Csmith on the board: compares the RTL with the emulator (and both with host
# gcc) on random programs.  For each seed and level (default -O1 -O2) it
# builds ONE ELF with KlaussCPU clang, runs that same ELF on the klausscc
# emulator and on the board (under flock $BOARD_LOCK), and compares the three
# checksums.  See csmith_lib.sh for the undefined-behaviour screen.
#
# results.tsv, one line per seed and level:
#   seed  opt  status  host  emulator  board  instructions  text-bytes
# status:
#   all-equal     host = emulator = board
#   BOARD-EMU     board != emulator (or the board printed no checksum): an RTL
#                 or emulator bug.  The ELF, source and both outputs are kept
#                 in <outdir>/board-emu/<seed><opt>/.
#   COMPILER      emulator = board != host: a compiler (or runtime) bug, kept in
#                 <outdir>/compiler/<seed><opt>/.
#   EMU-HOST      board = host != emulator: an emulator bug, kept with BOARD-EMU.
#   skip-long     the emulator ran past EMU_MAX_INSTRUCTIONS (default 300M):
#                 too long for a board run, not run there.
#   skip-big      .text larger than MAX_TEXT bytes (default 262144).
#   no-output, cc-fail, ld-fail   the emulator printed no checksum / the build
#                 failed (kept in compiler/).
#   gen-fail, host-cc-fail, host-timeout, host-ub   seed discarded.
# Exits 1 if any seed was BOARD-EMU, EMU-HOST or COMPILER (or failed to build).
set -uo pipefail
. "$(dirname "$0")/common.sh"
. "$(dirname "$0")/csmith_lib.sh"
[ $# -ge 3 ] || die "usage: csmith_board.sh <first-seed> <count> <outdir> [opt ...]"
FIRST=$1; COUNT=$2; OUT=$(realpath -m "$3"); shift 3
OPTS=${*:-"-O1 -O2"}
MAXI=${EMU_MAX_INSTRUCTIONS:-300000000}
MAX_TEXT=${MAX_TEXT:-262144}
mkdir -p "$OUT/work" "$OUT/board-emu" "$OUT/compiler"
csmith_rt "$OUT"
one() {
  local seed=$1 W=$OUT/work/$1 O emu_ck board_ck n text t keep
  mkdir -p "$W"; cd "$W" || return
  if csmith_gen "$seed"; then
    for O in $OPTS; do
      emu_ck=-; board_ck=-; n=-; text=-; keep=
      if csmith_build "$O"; then
        text=$("$LLVM_BIN/llvm-size" -A "t$O.elf" | awk '$1==".text"{print $2}')
        emu "$MAXI" "t$O.elf" "emu$O.out" 900
        n=$(emu_instructions "emu$O.out")
        emu_ck=$(checksum_of "emu$O.out"); emu_ck=${emu_ck:--}
        if grep -q 'stop = InstructionCap' "emu$O.out"; then ST=skip-long
        elif [ "$text" -gt "$MAX_TEXT" ]; then ST=skip-big
        elif [ "$emu_ck" = - ]; then ST=no-output; keep=compiler
        else
          # 100 MHz and a CPI below 10, plus the load.
          t=$(( 20 + ${n:-0} / 10000000 ))
          board "$t" "t$O.elf" "board$O.out"
          board_ck=$(checksum_of "board$O.out"); board_ck=${board_ck:--}
          if [ "$board_ck" = "$emu_ck" ] && [ "$emu_ck" = "$REF" ]; then ST=all-equal
          elif [ "$board_ck" != "$emu_ck" ] && [ "$board_ck" = "$REF" ]; then ST=EMU-HOST; keep=board-emu
          elif [ "$board_ck" != "$emu_ck" ]; then ST=BOARD-EMU; keep=board-emu
          else ST=COMPILER; keep=compiler; fi
        fi
      else
        keep=compiler
      fi
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$seed" "$O" "$ST" "$REF" "$emu_ck" "$board_ck" "$n" "$text"
      if [ -n "$keep" ]; then
        mkdir -p "$OUT/$keep/$seed$O"
        cp t.c ./*"$O".* "$OUT/$keep/$seed$O/" 2>/dev/null
        printf 'host %s\nemulator %s\nboard %s\n' "$REF" "$emu_ck" "$board_ck" >"$OUT/$keep/$seed$O/checksums.txt"
      fi
    done
  fi
  cd "$OUT" && rm -rf "$W"
}
export -f one csmith_gen csmith_build checksum_of emu emu_instructions board
export OUT OPTS MAXI MAX_TEXT LLVM_BIN RT KLAUSSCC CSMITH CSI CFLAGS RTOBJS PICO CSMITH_OPTS BOARD_LOCK
seq "$FIRST" $((FIRST + COUNT - 1)) | xargs -P "$JOBS" -I{} bash -c 'one {}' >>"$OUT/results.tsv"
awk -F'\t' '{c[$2 " " $3]++} END{for (k in c) print k, c[k]}' "$OUT/results.tsv" | sort
! awk -F'\t' '$3=="BOARD-EMU" || $3=="EMU-HOST" || $3=="COMPILER" || $3=="no-output" || $3=="cc-fail" || $3=="ld-fail"' "$OUT/results.tsv" | grep -q .
