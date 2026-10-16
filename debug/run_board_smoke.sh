#!/usr/bin/env bash
# Debugger smoke test on the real board: the emulator smoke test's lldb
# session (smoke.lldb on dbgtest.c, -O1 -g) against core 1 over JTAG.
# Needs the Nexys A7 connected with a DEBUG_EN=1 bitstream loaded, and a
# klausscc built with --features board.  UART output goes to the board's
# console port, not checked here.
#
#   KLAUSSCPU_LLVM_BIN, KLAUSSCC, SERIAL, JTAG_KHZ as for board-gdbserver.sh
#   PORT                gdbserver port    (default 5299)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$ROOT/../klausscpu-llvm/build/bin}
PORT=${PORT:-5299}
export KLAUSSCPU_LLVM_BIN=$LLVM_BIN

ELF=$ROOT/baremetal/dbgtest.elf
OUT=$(mktemp -d)
"$HERE/board-gdbserver.sh" dbgtest "$PORT" </dev/null >"$OUT/server.txt" 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true; rm -rf "$OUT"' EXIT
for _ in $(seq 600); do
  grep -q 'waiting for a debugger' "$OUT/server.txt" 2>/dev/null && break
  if ! kill -0 $SERVER 2>/dev/null; then cat "$OUT/server.txt"; echo "board smoke test: server failed"; exit 1; fi
  sleep 0.1
done

# The board reports a watchpoint up to two instructions after the store, so
# the last hit in bump() lands back in main(), where `i` does not exist (and
# a failing command ends an lldb batch script): ask for `counter` only.
sed "s|@ELF@|$ELF|; s|@PORT@|$PORT|; s|^frame variable i counter$|frame variable counter|" \
  "$HERE/smoke.lldb" >"$OUT/smoke.lldb"
timeout 300 "$LLVM_BIN/lldb" -b -s "$OUT/smoke.lldb" >"$OUT/lldb.txt" 2>&1 || true
wait "$SERVER" || true

FAIL=0
check() {  # check FILE TEXT
  if ! grep -qF -- "$2" "$OUT/$1"; then
    echo "FAIL: '$2' not in $1"
    FAIL=1
  fi
}
check lldb.txt 'stop reason = breakpoint 1.1'
check lldb.txt 'main at dbgtest.c'
check lldb.txt '(const point) *a = (x = 1, y = 2)'
check lldb.txt '(const point) *b = (x = 4, y = 6)'
check lldb.txt 'square(v=-3)'
check lldb.txt 'Return value: (long) $0 = 9'
check lldb.txt 'fib(n=1) at dbgtest.c'
check lldb.txt 'fib(n=6) at dbgtest.c'
check lldb.txt 'Watchpoint 1 hit'
check lldb.txt 'new value: 3'
check lldb.txt '(volatile long) counter = 6'
check lldb.txt 'received signal: SIGTRAP'
check lldb.txt 'exited with status = 33'
check server.txt 'core 1 halted at 0x'
check server.txt 'program halted (exit status 33)'
if [ "$FAIL" -ne 0 ] || [ -n "${VERBOSE:-}" ]; then
  echo "---- lldb ----"; cat "$OUT/lldb.txt"
  echo "---- server ----"; cat "$OUT/server.txt"
fi
[ "$FAIL" -eq 0 ] || exit 1
echo "board debug smoke test: PASS"
