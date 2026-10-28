#!/usr/bin/env bash
# selftest.sh <outdir> <emu-O1 run> [<board-O1 run>]
#
# Fault injection for the harness itself: checks that compare.sh and the
# Csmith scripts catch what they must.  <emu-O1 run> is an emu_suite.sh -O1
# output dir that matches baselines/emu-O1 (e.g. <quick out>/emu-O1);
# <board-O1 run> a board_suite.sh output dir (the board cases are skipped
# without it).  Nothing here touches the board.  Each case copies a run,
# tampers with one capture and expects compare.sh to pass or fail:
#   a halting program that hangs after part of its output, that traps or hits
#   an invalid opcode after all of it, or whose emulator run was killed; a
#   never-halting program that now halts or prints a different early line; a
#   PASS line that turns into FAIL; a missing program; a board run that stops
#   halting.
# Then Csmith on the emulator through a klausscc wrapper: a trap after a
# correct checksum (stop-Trap), and one level that runs past the cap while
# the others halt (ok-long with the default retry cap, HANG with a small one).
# Exits 1 if any case gave the wrong answer.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/common.sh"
[ $# -ge 2 ] || die "usage: selftest.sh <outdir> <emu-O1 run> [<board-O1 run>]"
T=$(realpath -m "$1"); E=$(realpath "$2"); BD=${3:+$(realpath "$3")}
C=$HERE/compare.sh; BASE=$HERE/baselines/emu-O1; ok=0; bad=0
rm -rf "$T"; mkdir -p "$T"
check() {   # check <want-rc> <name> <command...>
  local want=$1 name=$2 rc; shift 2
  "$@" >"$T/last.log" 2>&1; rc=$?
  if [ "$rc" = "$want" ]; then ok=$((ok + 1)); echo "ok   $name: $(grep -E '^(STOP|MISSING)|^uart:|^[a-zA-Z-]+ [0-9]+$' "$T/last.log" | tr '\n' ' ')"
  else bad=$((bad + 1)); echo "BAD  $name (rc=$rc, want $want)"; sed 's/^/     /' "$T/last.log" | head -20; fi
}
mk() { rm -rf "${T:?}/$1"; cp -r "$2" "$T/$1"; }

check 0 "emu run vs baseline" "$C" "$BASE" "$E"
check 0 "emu run vs itself" "$C" "$E" "$E"
mk hang "$E"
{ sed -n '1,/Emulator finished/p' "$E/test_big.out" | sed '$d'
  echo '--- Emulator finished: 800000000 instructions, stop = InstructionCap ---'
  sed -n '/Captured UART output/,$p' "$E/test_big.out" | head -23; } >"$T/hang/test_big.out"
sed -i 's/^test_big\t[0-9]*/test_big\tnohalt/' "$T/hang/stats.tsv"
check 1 "hang after 22 lines vs baseline" "$C" "$BASE" "$T/hang"
check 1 "hang after 22 lines vs run" "$C" "$E" "$T/hang"
mk trap "$E"; sed -i 's/stop = Halt/stop = Trap/' "$T/trap/hello.out"
check 1 "trap after the last line" "$C" "$BASE" "$T/trap"
check 1 "trap after the last line vs run" "$C" "$E" "$T/trap"
mk inv "$E"; sed -i 's/stop = Halt/stop = InvalidOpcode(1234)/' "$T/inv/queens.out"
check 1 "invalid opcode" "$C" "$BASE" "$T/inv"
mk kill "$E"; sed -i '/Emulator finished/d' "$T/kill/bst.out"
check 1 "emulator killed by its timeout" "$C" "$BASE" "$T/kill"
mk nowhalt "$E"; sed -i 's/stop = InstructionCap/stop = Halt/' "$T/nowhalt/adventure.out"
check 1 "never-halting program now halts" "$C" "$BASE" "$T/nowhalt"
mk early "$E"
awk '/Captured UART/ { print; getline; print "XX " $0; next } { print }' "$E/adventure.out" >"$T/early/adventure.out"
check 1 "never-halting program, early line changed" "$C" "$BASE" "$T/early"
mk miss "$E"; rm "$T/miss/queens.out"
check 1 "program missing" "$C" "$BASE" "$T/miss"
if [ -n "$BD" ]; then
  check 0 "board run vs itself" "$C" "$BD" "$BD"
  check 0 "board run vs baseline" "$C" "$HERE/baselines/board-O1" "$BD"
  mk bt "$BD"; sed -i 's/PASS  SHA256 two-block/FAIL  SHA256 two-block/' "$T/bt/crypto_selftest.out"
  check 1 "board PASS line mentioning bytes turns FAIL" "$C" "$HERE/baselines/board-O1" "$T/bt"
  mk bh "$BD"; sed -i '/CPU halted/d' "$T/bh/hello.out"
  check 1 "board program stops halting" "$C" "$BD" "$T/bh"
  check 1 "board program stops halting vs baseline" "$C" "$HERE/baselines/board-O1" "$T/bh"
fi

# Csmith: a wrapper around klausscc that turns 'stop = Halt' into a trap for
# the emulator runs of ELFs matching $FAKE_TRAP.
cat >"$T/klausscc-trap" <<'W'
#!/usr/bin/env bash
elf=; prev=; for a in "$@"; do [ "$prev" = -i ] && elf=$a; prev=$a; done
out=$("$REAL_KLAUSSCC" "$@" 2>&1); rc=$?
[[ $elf == *$FAKE_TRAP* ]] && out=${out//stop = Halt/stop = Trap}
printf '%s\n' "$out"; exit $rc
W
chmod +x "$T/klausscc-trap"
check 1 "csmith: trap after a good checksum (stop-Trap)" \
  env KLAUSSCC="$T/klausscc-trap" REAL_KLAUSSCC="$KLAUSSCC" FAKE_TRAP=t-O2.elf "$HERE/csmith_run.sh" 120000 1 "$T/cs-trap"
# Seed 120000 takes 332K instructions at -O0 and under 100K at -O1/-O2/-Os.
check 0 "csmith: one level over the cap, halts on retry (ok-long)" \
  env EMU_MAX_INSTRUCTIONS=200000 "$HERE/csmith_run.sh" 120000 1 "$T/cs-long"
check 1 "csmith: one level over the cap, also on retry (HANG)" \
  env EMU_MAX_INSTRUCTIONS=200000 EMU_RETRY_MAX=250000 "$HERE/csmith_run.sh" 120000 1 "$T/cs-hang"
echo "selftest: $ok ok, $bad wrong"
[ $bad = 0 ]
