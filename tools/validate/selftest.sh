#!/usr/bin/env bash
# selftest.sh <outdir> <emu-O1 run> [<board-O1 run>]
#
# Fault injection for the harness itself: checks that compare.sh, perf.py and the
# Csmith scripts catch what they must.  <emu-O1 run> is an emu_suite.sh -O1
# output dir that matches baselines/emu-O1 (e.g. <quick out>/emu-O1);
# <board-O1 run> a board_suite.sh output dir (the board cases are skipped
# without it).  Nothing here touches the board.  Each case copies a run,
# tampers with one capture and expects compare.sh to pass or fail:
#   a halting program that hangs after part of its output, that traps or hits
#   an invalid opcode after all of it, or whose emulator run was killed; a
#   never-halting program that now halts or prints a different early line; a
#   PASS line that turns into FAIL; a missing program; a never-halting
#   program whose late verdict turns FAIL, or that stops before its verdicts,
#   or prints a new FAIL (and, passing, one that gets less far, interleaves
#   its early or later lines differently, or reaches a new PASS); a board run
#   that stops halting.
# Then Csmith on the emulator through a klausscc wrapper: a trap after a
# correct checksum (stop-Trap), and one level that runs past the cap while
# the others halt (ok-long with the default retry cap, HANG with a small one);
# csmith_board.sh with a wrong emulator checksum in a program too big for the
# board (it never reaches the board); and perf.py with a metric that is no
# longer measured.
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
  if [ "$rc" = "$want" ]; then ok=$((ok + 1)); echo "ok   $name: $(grep -E '^(STOP|MISSING|VERDICT|ORDER)|^uart:|^[a-zA-Z-]+ [0-9]+$|^-O[0-9sz] [a-zA-Z-]+ [0-9]+$| MISSING$|regression' "$T/last.log" | tr '\n' ' ')"
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
# Programs stopped by the cap: their verdicts come after the first 12 lines.
mk rtos "$E"; sed -i 's/^Result: PASS/Result: FAIL/' "$T/rtos/test_rtos.out"
check 1 "capped program, late Result: PASS turns FAIL" "$C" "$BASE" "$T/rtos"
check 1 "capped program, late Result: PASS turns FAIL vs run" "$C" "$E" "$T/rtos"
mk sync "$E"; sed -i 's/(expected 10)  PASS/(expected 10)  FAIL/' "$T/sync/test_sync.out"
check 1 "capped program, late PASS turns FAIL" "$C" "$BASE" "$T/sync"
mk lost "$E"; sed -i '/=== all tasks done ===/,/^Result:/d' "$T/lost/test_rtos.out"
check 1 "capped program stops before its verdicts" "$C" "$BASE" "$T/lost"
mk slow "$E"; sed -i '/^pass [1-4][0-9]: /d' "$T/slow/blit_capture.out"
sed -i '/^\[B\] 6 /{h;d}; /^\[C\] 5 /G' "$T/slow/test_rtos.out"
check 0 "capped programs: fewer passes, late interleaving changed" "$C" "$BASE" "$T/slow"
mk order "$E"; sed -i '/^\[A\] 0 /{h;d}; /^\[B\] 0 /G' "$T/order/test_rtos.out"
check 0 "capped program, early lines in another order (ORDER)" "$C" "$BASE" "$T/order"
mk fast "$E"; sed -i 's/^Result: PASS/&\nwatchdog: PASS/' "$T/fast/test_rtos.out"
check 0 "capped program reaches a new PASS verdict" "$C" "$BASE" "$T/fast"
mk nfail "$E"; sed -i 's/^Result: PASS/&\nwatchdog: FAIL/' "$T/nfail/test_rtos.out"
check 1 "capped program prints a new FAIL verdict" "$C" "$BASE" "$T/nfail"
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
# Csmith on the board script, without the board: the emulator checksum of
# every level is wrong and every program counts as too big for the board.
cat >"$T/klausscc-badck" <<'W'
#!/usr/bin/env bash
out=$("$REAL_KLAUSSCC" "$@" 2>&1); rc=$?
case " $* " in *" --emulate "*) out=$(printf '%s\n' "$out" | sed 's/checksum = [0-9A-F]*/checksum = DEADBEEF/') ;; esac
printf '%s\n' "$out"; exit $rc
W
chmod +x "$T/klausscc-badck"
check 1 "csmith-board: emulator MISMATCH in a program too big for the board" \
  env KLAUSSCC="$T/klausscc-badck" REAL_KLAUSSCC="$KLAUSSCC" MAX_TEXT=1 "$HERE/csmith_board.sh" 120000 1 "$T/csb-big"
check 0 "csmith-board: good program too big for the board (skip-big)" \
  env MAX_TEXT=1 "$HERE/csmith_board.sh" 120000 1 "$T/csb-big-ok"

# perf.py: a judged metric missing from a group that was measured fails; a
# group that was not measured at all does not.
P=$HERE/baselines/perf.tsv
check 0 "perf: baseline vs itself" python3 -I "$HERE/perf.py" compare "$P" "$P"
grep -v '^board\.O1\.dhrystone_nobuiltin\.' "$P" >"$T/perf-nodhry.tsv"
check 1 "perf: Dhrystone (-fno-builtin) no longer measured" python3 -I "$HERE/perf.py" compare "$P" "$T/perf-nodhry.tsv"
grep -v '^bench\.O2\.embench\.huffbench\.' "$P" >"$T/perf-nohuff.tsv"
check 1 "perf: one Embench benchmark no longer measured" python3 -I "$HERE/perf.py" compare "$P" "$T/perf-nohuff.tsv"
grep -v -E '^(bench|zephyr)\.' "$P" >"$T/perf-nobench.tsv"
check 0 "perf: no benchmarks or Zephyr run (BENCH=0)" python3 -I "$HERE/perf.py" compare "$P" "$T/perf-nobench.tsv"

echo "selftest: $ok ok, $bad wrong"
[ $bad = 0 ]
