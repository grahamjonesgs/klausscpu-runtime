#!/usr/bin/env bash
# validate.sh [-o OUTDIR] MODE [ARGS]  -- the KlaussCPU toolchain validation driver.
#
# Modes (see README.md for what each one checks and how long it takes):
#   quick                 emulator corpus at -O1 and -O2 against the committed
#                         UART baselines, instruction counts and .text sizes,
#                         4 ABI conformance seeds, 30 Csmith seeds
#   full                  quick, plus 300 Csmith seeds (-O0 -O1 -O2 -Os), 150
#                         --float seeds, 30 ABI seeds, the KlaussCPU lit tests
#                         and unit tests, and an oracle snapshot (diffed
#                         against ORACLE_BASE when set)
#   board                 the board suite at -O1 (each program twice), CoreMark
#                         and Embench-IoT at -O2 and -Os (BENCH=0 skips them),
#                         compared with baselines/perf.tsv
#   zephyr                Zephyr apps: build, emulator, board (VNC, SSH),
#                         compared with baselines/perf.tsv
#   csmith N [FIRST]      N Csmith seeds on the emulator (-O0 -O1 -O2 -Os)
#   csmith-board N [FIRST]  N Csmith seeds, one ELF per level (-O1 -O2) run on
#                         the emulator AND the board: board vs emulator vs host
#   abi [N] [FIRST]       N ABI conformance seeds (default 8)
#   oracle [BASE]         snapshot the toolchain's output bytes; diff with BASE
#   selftest [EMU_RUN [BOARD_RUN]]  fault injection for the harness itself
#                         (selftest.sh); runs the -O1 corpus first unless an
#                         emu_suite.sh -O1 output dir is given
#   baseline DIR...       rewrite baselines/ from the output dirs of a quick,
#                         board and/or zephyr run (after an intended change)
#   deps CHECKOUT         link the gitignored prerequisites (picolibc-install,
#                         lwip, wolfSSL/wolfSSH sources, the west workspace,
#                         doomgeneric, the benchmark sources) from another
#                         runtime checkout into this one
#
# OUTDIR defaults to $VALIDATE_OUT/<mode>-<time> ($VALIDATE_OUT defaults to
# ${TMPDIR:-/tmp}/klausscpu-validate).  Environment: see common.sh
# (KLAUSSCPU_LLVM_BIN, KLAUSSCC, BOARD_LOCK, JOBS), csmith_lib.sh, and
#   CSMITH_SEED   first Csmith seed for quick/full (default 1)
#   PERF_TOL      override every perf tolerance (percent)
#   STATS_TOL     instruction-count / .text growth that is flagged (default 2%)
# Prints a PASS/FAIL line per check and a summary; exits 1 if anything failed.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=
if [ "${1:-}" = -o ]; then OUT=$2; shift 2; fi
MODE=${1:-}; shift || true
[ -n "$MODE" ] || { sed -n '2,/^set -uo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 2; }

if [ "$MODE" = deps ]; then
  RT=$(cd "$HERE/../.." && pwd)
  SRC=$(cd "${1:?usage: validate.sh deps <checkout>}" && pwd)
  for p in picolibc-install picolibc-install-oldabi lwip freertos/FreeRTOS-Kernel \
           freertos/wolfssl/wolfssl-src freertos/wolfssl/wolfssh-src \
           zephyr-ws/zephyr zephyr-ws/modules zephyr-ws/.west zephyr-ws/.venv \
           zephyr-ws/klausscpu-zephyr/apps/doom/doomgeneric-src \
           baremetal/benchmarks/coremark baremetal/benchmarks/embench-iot; do
    if [ -e "$SRC/$p" ] && [ ! -e "$RT/$p" ]; then
      ln -s "$SRC/$p" "$RT/$p" && echo "linked $p"
      # .gitignore names these as directories ("lwip/"), which does not match
      # a symlink: keep the links out of 'git status' and 'git add -A'.
      ex=$(git -C "$RT" rev-parse --path-format=absolute --git-path info/exclude 2>/dev/null) &&
        { mkdir -p "$(dirname "$ex")"; grep -qxF "/$p" "$ex" 2>/dev/null || echo "/$p" >>"$ex"; }
    fi
  done
  exit 0
fi

. "$HERE/common.sh"
[ -n "$OUT" ] || OUT=${VALIDATE_OUT:-${TMPDIR:-/tmp}/klausscpu-validate}/$MODE-$(date +%Y%m%d-%H%M%S)
OUT=$(realpath -m "$OUT"); mkdir -p "$OUT"
SUM=$OUT/summary.txt; : >"$SUM"
BASE=$HERE/baselines
result() {   # result PASS|FAIL <check> <detail>
  printf '%-4s %-28s %s\n' "$1" "$2" "$3" | tee -a "$SUM"
}
run() {      # run <check> <logfile> <command...> -- PASS/FAIL on the command's status
  local name=$1 log=$2; shift 2
  if "$@" >"$log" 2>&1; then result PASS "$name" "$(grep -v '^ *$' "$log" | tail -n 1)"
  else result FAIL "$name" "see $log"; fi
}
echo "validate $MODE: toolchain $(toolchain_id) (KLAUSSCPU_LLVM_BIN=$LLVM_BIN), klausscc $KLAUSSCC, output $OUT"
case $MODE in board|zephyr|csmith-board) echo "validate $MODE: board lock $BOARD_LOCK (every session sharing the board must use the same file)" ;; esac

emu_corpus() {
  local O
  for O in -O1 -O2; do
    "$HERE/emu_suite.sh" "$OUT/emu$O" "$O"
    if "$HERE/compare.sh" "$BASE/emu$O" "$OUT/emu$O" >"$OUT/emu$O.cmp" 2>&1; then
      result PASS "emulator corpus $O" "$(grep '^uart:' "$OUT/emu$O.cmp")"
    else
      result FAIL "emulator corpus $O" "$(grep -E '^uart:|^build errors' "$OUT/emu$O.cmp" | tr '\n' ' ') -- see $OUT/emu$O.cmp"
    fi
    grep -E '^(STOP|MISSING|NOT IN BASE|=== DIFF)|WARN|^instructions|^\.text' "$OUT/emu$O.cmp" | sed 's/^/     /'
  done
}
abi() {   # abi <count> [first]
  run "ABI conformance x$1" "$OUT/abi.log" env OUT="$OUT/abi" PICOLIBC="$(picolibc_for_toolchain)" \
    "$RT/abi-test/run_abi_test.sh" "${2:-1}" "$1"
  rm -rf "$OUT/abi"
}
csmith() {   # csmith <name> <first> <count> [CSMITH_OPTS, default: $CSMITH_OPTS]
  local d=$OUT/$1 o=${4-${CSMITH_OPTS:-}}
  if CSMITH_OPTS=$o "$HERE/csmith_run.sh" "$2" "$3" "$d" >"$d.log" 2>&1; then
    result PASS "$1 ($3 seeds from $2${o:+ $o})" "$(tr '\n' ' ' <"$d.log")"
  else
    result FAIL "$1 ($3 seeds from $2${o:+ $o})" "$(tr '\n' ' ' <"$d.log") -- see $d/fail"
  fi
  rm -rf "$d/work"
}
lit() {
  local L=$LLVM_SRC
  [ -x "$LLVM_BIN/llvm-lit" ] || { result FAIL "lit" "no llvm-lit in $LLVM_BIN"; return; }
  run "lit (KlaussCPU)" "$OUT/lit.log" "$LLVM_BIN/llvm-lit" -s \
    "$L/llvm/test/CodeGen/KlaussCPU" "$L/llvm/test/MC/KlaussCPU" "$L/llvm/test/MC/Disassembler/KlaussCPU" \
    "$L/llvm/test/DebugInfo/KlaussCPU" "$L/llvm/test/Transforms/LoopUnroll/KlaussCPU" \
    "$L"/lld/test/ELF/klausscpu-*.s "$L"/clang/test/CodeGen/klausscpu-*.c \
    "$L"/clang/test/Driver/klausscpu-*.c "$L"/clang/test/Preprocessor/klausscpu-*.c
  local U=$LLVM_BIN/../unittests/Target/KlaussCPU/KlaussCPUTests
  if [ -x "$U" ]; then run "unit tests (KlaussCPUTests)" "$OUT/unittests.log" "$U"; fi
}
perf_check() {   # perf_check <metrics.tsv> <key-prefix-regex> <label>
  local b=$OUT/perf-baseline-$3.tsv
  grep -E "^(#|$2)" "$BASE/perf.tsv" >"$b"
  # shellcheck disable=SC2086
  run "perf vs baseline ($3)" "$OUT/perf-$3.cmp" \
    python3 -I "$HERE/perf.py" compare "$b" "$1" ${PERF_TOL:+--tol $PERF_TOL}
  grep -E 'REGRESSION|improved' "$OUT/perf-$3.cmp" | sed 's/^/     /'
}

case $MODE in
quick)
  emu_corpus
  abi 4
  csmith csmith "${CSMITH_SEED:-1}" 30 ""
  ;;
full)
  emu_corpus
  abi 30
  csmith csmith "${CSMITH_SEED:-1}" 300 ""
  csmith csmith-float "$(( ${CSMITH_SEED:-1} + 300 ))" 150 --float
  lit
  "$HERE/oracle.sh" "$OUT/oracle" >"$OUT/oracle.log" 2>&1
  if [ -n "${ORACLE_BASE:-}" ]; then
    run "oracle vs $ORACLE_BASE" "$OUT/oracle.diff" "$HERE/oracle_diff.sh" "$ORACLE_BASE" "$OUT/oracle"
  else
    result PASS "oracle snapshot" "$OUT/oracle (set ORACLE_BASE to diff)"
  fi
  ;;
board)
  "$HERE/board_suite.sh" "$OUT/board-O1" -O1
  if "$HERE/compare.sh" "$BASE/board-O1" "$OUT/board-O1" >"$OUT/board-O1.cmp" 2>&1; then
    result PASS "board suite -O1 (UART)" "$(grep '^uart:' "$OUT/board-O1.cmp")"
  else
    result FAIL "board suite -O1 (UART)" "$(grep '^uart:' "$OUT/board-O1.cmp") -- see $OUT/board-O1.cmp"
  fi
  grep -E '^(STOP|MISSING|NOT IN BASE|=== DIFF)' "$OUT/board-O1.cmp" | sed 's/^/     /'
  if [ -s "$OUT/board-O1/repeat.txt" ]; then result FAIL "board repeat run" "$(tr '\n' ' ' <"$OUT/board-O1/repeat.txt")"
  else result PASS "board repeat run" "the runs agree"; fi
  if grep -v -E 'halted=1|^test_(rtos|sync) rc=124' "$OUT/board-O1/summary.txt" | grep -q .; then
    result FAIL "board halts" "$(grep -v -E 'halted=1|^test_(rtos|sync) rc=124' "$OUT/board-O1/summary.txt" | tr '\n' ' ')"
  else result PASS "board halts" "every program halted"; fi
  python3 -I "$HERE/perf.py" extract board "$OUT/board-O1" board.O1 >"$OUT/perf.tsv" 2>"$OUT/perf-noise.txt"
  [ -s "$OUT/perf-noise.txt" ] && sed 's/^/     /' "$OUT/perf-noise.txt"
  if [ "${BENCH:-1}" != 0 ]; then
    if "$HERE/bench.sh" "$OUT/bench" -O2 -Os >"$OUT/bench.log" 2>&1; then
      result PASS "CoreMark + Embench" "$(grep -E '^-O' "$OUT/bench.log" | tr '\n' ' ')"
    else
      result FAIL "CoreMark + Embench" "$(grep -E '^-O' "$OUT/bench.log" | tr '\n' ' ') -- see $OUT/bench"
    fi
    for O in O2 Os; do
      python3 -I "$HERE/perf.py" extract bench "$OUT/bench/$O" "bench.$O" >>"$OUT/perf.tsv"
    done
  fi
  perf_check "$OUT/perf.tsv" "board\.|bench\." board
  ;;
zephyr)
  "$HERE/zephyr/suite.sh" "$OUT" build emu board >"$OUT/zephyr.log" 2>&1
  grep -E '^(PASS|FAIL)' "$OUT/zephyr.log" | while read -r st rest; do result "$st" "zephyr" "$rest"; done
  perf_check "$OUT/perf.tsv" "zephyr\." zephyr
  ;;
csmith)
  csmith csmith "${2:-${CSMITH_SEED:-1}}" "${1:?usage: validate.sh csmith N [FIRST]}"
  ;;
csmith-board)
  N=${1:?usage: validate.sh csmith-board N [FIRST]}; F=${2:-${CSMITH_SEED:-1}}
  if "$HERE/csmith_board.sh" "$F" "$N" "$OUT/csmith-board" >"$OUT/csmith-board.log" 2>&1; then
    result PASS "csmith-board ($N seeds from $F)" "$(tr '\n' ' ' <"$OUT/csmith-board.log")"
  else
    result FAIL "csmith-board ($N seeds from $F)" "$(tr '\n' ' ' <"$OUT/csmith-board.log") -- see $OUT/csmith-board/{board-emu,compiler}"
  fi
  rm -rf "$OUT/csmith-board/work"
  ;;
abi) abi "${1:-8}" "${2:-1}" ;;
selftest)
  E=${1:-}
  if [ -z "$E" ]; then "$HERE/emu_suite.sh" "$OUT/emu-O1" -O1; E=$OUT/emu-O1; fi
  run "harness self-test" "$OUT/selftest.log" "$HERE/selftest.sh" "$OUT/selftest" "$E" ${2:+"$2"}
  ;;
oracle)
  "$HERE/oracle.sh" "$OUT/oracle" >"$OUT/oracle.log" 2>&1
  if [ -n "${1:-}" ]; then run "oracle vs $1" "$OUT/oracle.diff" "$HERE/oracle_diff.sh" "$1" "$OUT/oracle"
  else result PASS "oracle snapshot" "$OUT/oracle"; fi
  ;;
baseline)
  [ $# -ge 1 ] || die "usage: validate.sh baseline <run-dir>..."
  for d in "$@"; do
    for sub in emu-O1 emu-O2 board-O1; do
      [ -d "$d/$sub" ] || continue
      rm -rf "${BASE:?}/$sub"; mkdir -p "$BASE/$sub"
      for f in "$d/$sub"/*.out; do
        n=$(basename "$f" .out)
        uart_filter "$f" | head -"$(uart_lines "$f")" >"$BASE/$sub/$n.txt"
        printf '%s\t%s\n' "$n" "$(stop_of "$f")" >>"$BASE/$sub/stop.tsv"
      done
      [ -f "$d/$sub/stats.tsv" ] && cp "$d/$sub/stats.tsv" "$BASE/$sub/"
      result PASS "baseline $sub" "from $d/$sub ($(toolchain_id))"
    done
    if [ -f "$d/perf.tsv" ]; then
      python3 -I "$HERE/perf.py" merge "$BASE/perf.tsv" "$d/perf.tsv" "$(toolchain_id)" &&
        result PASS "baseline perf" "merged $d/perf.tsv"
    fi
  done
  ;;
*) die "unknown mode '$MODE' (run without arguments for help)" ;;
esac

echo "---- summary ($OUT) ----"
cat "$SUM"
if grep -q '^FAIL' "$SUM"; then echo "validate $MODE: FAILED"; exit 1; fi
echo "validate $MODE: PASSED"
