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
#   BOARD-EMU     board != emulator (or the board printed no checksum, or did
#                 not halt, while the emulator halted): an RTL
#                 or emulator bug.  The ELF, source and both outputs are kept
#                 in <outdir>/board-emu/<seed><opt>/.
#   COMPILER      emulator = board != host: a compiler (or runtime) bug, kept in
#                 <outdir>/compiler/<seed><opt>/.
#   EMU-HOST      board = host != emulator: an emulator bug, kept with BOARD-EMU.
#   skip-long     the emulator ran past EMU_MAX_INSTRUCTIONS (default 300M):
#                 too long for a board run, not run there.  When another level
#                 of the same seed was all-equal, the emulator run is repeated
#                 with the larger EMU_RETRY_MAX cap (csmith_recheck in
#                 csmith_lib.sh) and must halt with the host's checksum
#                 (skip-long-ok); otherwise the level is HANG (still running,
#                 kept in compiler/), or COMPILER / no-output / stop-<reason>.
#   stop-<reason> the emulator run ended other than by HALT (a trap, an
#                 invalid opcode, ...), kept in compiler/.
#   skip-big      .text larger than MAX_TEXT bytes (default 262144) and the
#                 emulator halted with the host's checksum: not run on the
#                 board.  A big program whose emulator checksum is wrong is
#                 COMPILER (without a board run).
#   no-output, cc-fail, ld-fail   the emulator printed no checksum / the build
#                 failed (kept in compiler/).
#   gen-fail, host-cc-fail, host-timeout, host-ub   seed discarded.
# Exits 1 if any seed was BOARD-EMU, EMU-HOST, COMPILER, HANG, stop-<reason> or
# no-output, or failed to build.
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
  local seed=$1 W=$OUT/work/$1 O emu_ck board_ck n text t keep i good=0
  local -a opt st eck bck cnt txt
  mkdir -p "$W"; cd "$W" || return
  if csmith_gen "$seed"; then
    i=0
    for O in $OPTS; do
      emu_ck=-; board_ck=-; n=-; text=-
      if csmith_build "$O"; then
        text=$("$LLVM_BIN/llvm-size" -A "t$O.elf" | awk '$1==".text"{print $2}')
        emu "$MAXI" "t$O.elf" "emu$O.out" 900
        csmith_classify "emu$O.out"; n=$NI; emu_ck=$G
        if [ "$ST" = instr-cap ]; then ST=skip-long
        elif [ "$ST" != ok ] && [ "$ST" != MISMATCH ]; then :   # no-output, stop-<reason>
        elif [ "$text" -gt "$MAX_TEXT" ]; then
          # Too big for the board; the emulator's verdict still counts.
          if [ "$ST" = ok ]; then ST=skip-big; else ST=COMPILER; fi
        else
          # 100 MHz and a CPI below 10, plus the load.
          t=$(( 20 + ${n:-0} / 10000000 ))
          board "$t" "t$O.elf" "board$O.out"
          board_ck=$(checksum_of "board$O.out"); board_ck=${board_ck:--}
          if [ "$(stop_of "board$O.out")" != Halt ]; then ST=BOARD-EMU   # the emulator halted
          elif [ "$board_ck" = "$emu_ck" ] && [ "$emu_ck" = "$REF" ]; then ST=all-equal; good=1
          elif [ "$board_ck" != "$emu_ck" ] && [ "$board_ck" = "$REF" ]; then ST=EMU-HOST
          elif [ "$board_ck" != "$emu_ck" ]; then ST=BOARD-EMU
          else ST=COMPILER; fi
        fi
      fi
      opt[i]=$O; st[i]=$ST; eck[i]=$emu_ck; bck[i]=$board_ck; cnt[i]=$n; txt[i]=$text; i=$((i + 1))
    done
    for i in "${!opt[@]}"; do
      O=${opt[i]}
      if [ "${st[i]}" = skip-long ] && [ $good = 1 ]; then
        csmith_recheck "$O"; eck[i]=$G; cnt[i]=$NI
        case $ST in ok-long) st[i]=skip-long-ok ;; MISMATCH) st[i]=COMPILER ;; *) st[i]=$ST ;; esac
      fi
      case ${st[i]} in
        all-equal|skip-long|skip-long-ok|skip-big) keep= ;;
        BOARD-EMU|EMU-HOST) keep=board-emu ;;
        *) keep=compiler ;;
      esac
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$seed" "$O" "${st[i]}" "$REF" "${eck[i]}" "${bck[i]}" "${cnt[i]}" "${txt[i]}"
      if [ -n "$keep" ]; then
        mkdir -p "$OUT/$keep/$seed$O"
        cp t.c ./*"$O".* "$OUT/$keep/$seed$O/" 2>/dev/null
        printf 'host %s\nemulator %s\nboard %s\n' "$REF" "${eck[i]}" "${bck[i]}" >"$OUT/$keep/$seed$O/checksums.txt"
      fi
    done
  fi
  cd "$OUT" && rm -rf "$W"
}
export -f one csmith_gen csmith_build csmith_classify csmith_recheck checksum_of emu emu_instructions stop_of board
export OUT OPTS MAXI MAX_TEXT LLVM_BIN RT KLAUSSCC CSMITH CSI CFLAGS RTOBJS PICO CSMITH_OPTS BOARD_LOCK
seq "$FIRST" $((FIRST + COUNT - 1)) | xargs -P "$JOBS" -I{} bash -c 'one {}' >>"$OUT/results.tsv"
awk -F'\t' '{c[$2 " " $3]++} END{for (k in c) print k, c[k]}' "$OUT/results.tsv" | sort
[ -s "$OUT/no-strict-aliasing.txt" ] && echo "no-strict-aliasing $(sort -u "$OUT/no-strict-aliasing.txt" | wc -l)"
! awk -F'\t' '$3 !~ /^(all-equal|skip-long|skip-long-ok|skip-big|host-timeout|host-ub|gen-fail|host-cc-fail)$/' "$OUT/results.tsv" | grep -q .
