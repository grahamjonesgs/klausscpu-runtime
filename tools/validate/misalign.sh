#!/usr/bin/env bash
# misalign.sh <outdir> emu|board
#
# The misaligned-access trap (klaussCPU 3508f87): programs/test_misalign.c
# built for CASE=0 (nothing may trap: byte, MEMGET32 and 64-bit accesses at
# every misalignment) and for each trapping CASE (a 16-bit access at an odd
# address, a 32-bit one not 4-aligned, loads, stores, signed, indexed), at
# -O1 and -O2.  Each trapping program must
#   - emulator: stop with Misaligned (not Halt);
#   - board:    print a crash dump with ERR=0A, PC inside level3, and the
#               call chain in F1 (inside level2) and F3 (inside level1); the
#               program must print no "unexpected:" line (the access did not
#               retire);
# and CASE=0 must print "=== ALL PASS ===" with the same output on both.
# Exits 1 on any failure.
set -uo pipefail
. "$(dirname "$0")/common.sh"
[ $# -ge 2 ] || die "usage: misalign.sh <outdir> emu|board"
OUT=$(realpath -m "$1"); WHERE=$2
CASES=${MISALIGN_CASES:-"1 2 3 4 5 6 7 8 9 10"}
rm -rf "$OUT"; mkdir -p "$OUT"
stage_baremetal "$OUT"
fail=0
bad() { echo "FAIL $*"; fail=$((fail + 1)); }
hex() { printf '%d' "0x${1:-0}"; }
for O in -O1 -O2; do
  for c in 0 $CASES; do
    rm -f "$STAGED_BM"/test_misalign.{o,elf}
    make -C "$STAGED_BM" test_misalign.elf OPT="$O" CASEFLAGS="-DCASE=$c" >"$OUT/build$O.$c.log" 2>&1 ||
      { bad "case $c $O: build failed"; continue; }
    elf=$OUT/m$O.$c.elf; cp "$STAGED_BM/test_misalign.elf" "$elf"
    emu 20000000 "$elf" "$OUT/emu$O.$c.out" 60
    if [ "$c" = 0 ]; then
      grep -aq '=== ALL PASS ===' "$OUT/emu$O.$c.out" && grep -aq 'stop = Halt' "$OUT/emu$O.$c.out" ||
        bad "case 0 $O: emulator did not pass"
      if [ "$WHERE" = board ]; then
        board 40 "$elf" "$OUT/board$O.$c.out"
        diff <(tr -d "\r" <"$OUT/emu$O.$c.out" | grep -a -E "^(hash|FAIL|=== )") <(tr -d "\r" <"$OUT/board$O.$c.out" | grep -a -E "^(hash|FAIL|=== )") >/dev/null ||
          bad "case 0 $O: board output differs from the emulator"
        grep -aq 'CPU halted' "$OUT/board$O.$c.out" || bad "case 0 $O: board did not halt"
      fi
      continue
    fi
    grep -aq 'stop = Misaligned' "$OUT/emu$O.$c.out" || bad "case $c $O: emulator did not stop with Misaligned"
    grep -aq 'unexpected:' "$OUT/emu$O.$c.out" && bad "case $c $O: emulator retired the access"
    [ "$WHERE" = board ] || continue
    for r in 1 2; do
      board 30 "$elf" "$OUT/board$O.$c.$r.out"
      f=$OUT/board$O.$c.$r.out; tr -d '\r' <"$f" >"$f.txt"; f=$f.txt
      grep -aq 'unexpected:' "$f" && bad "case $c $O run $r: the access retired"
      grep -aq '^ERR=0A ' "$f" || { bad "case $c $O run $r: no ERR=0A crash dump"; continue; }
      read -r l1 l2 l3 < <(grep -a -o 'level1=0x[0-9a-f]* level2=0x[0-9a-f]* level3=0x[0-9a-f]*' "$f" | head -1 | sed 's/level.=0x//g')
      pc=$(sed -n 's/^ERR=0A PC=\([0-9A-F]*\).*/\1/p' "$f" | head -1)
      f1=$(sed -n 's/^F1=0*\([0-9A-F]*\)$/\1/p' "$f" | head -1)
      f3=$(sed -n 's/^F3=0*\([0-9A-F]*\)$/\1/p' "$f" | head -1)
      p=$(hex "$pc"); a1=$(hex "$f1"); a3=$(hex "$f3")
      b1=$(hex "$l1"); b2=$(hex "$l2"); b3=$(hex "$l3")
      # level1 < level2 < level3 in address order (defined in that order).
      [ "$p" -ge "$b3" ] || bad "case $c $O run $r: PC $pc not in level3"
      [ "$a1" -gt "$b2" ] && [ "$a1" -le "$b3" ] || bad "case $c $O run $r: F1 $f1 not a return into level2"
      [ "$a3" -gt "$b1" ] && [ "$a3" -le "$b2" ] || bad "case $c $O run $r: F3 $f3 not a return into level1"
    done
  done
done
rm -rf "$OUT/tree"
if [ "$fail" = 0 ]; then echo "misalign ($WHERE): all cases trap as expected, CASE=0 clean"; else echo "misalign ($WHERE): $fail failures"; exit 1; fi
