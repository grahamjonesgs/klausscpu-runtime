#!/usr/bin/env bash
# RTOS debugging smoke test, the VS Code way: for the FreeRTOS demo and the
# Zephyr synchronization sample, build and serve with debug/rtos-gdbserver.sh
# (as the RTOS launch configurations' tasks do), then drive lldb-dap with the
# configurations' commands (dap_rtos.py): breakpoint, RTOS threads, stacks.
# Emulator by default; BOARD=1 runs on the board (DEBUG_EN=1 bitstream).
#
#   KLAUSSCPU_LLVM_BIN, KLAUSSCC, BOARD, SERIAL as for rtos-gdbserver.sh
#   PORT  gdbserver port (default 5499)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$ROOT/../klausscpu-llvm/build/bin}
PORT=${PORT:-5499}
export KLAUSSCPU_LLVM_BIN=$LLVM_BIN
OUT=$(mktemp -d)
SERVER=
trap '[ -n "$SERVER" ] && kill $SERVER 2>/dev/null; rm -rf "$OUT"' EXIT

FAIL=0
run() {  # run KIND NAME SOURCE PATTERN MIN_THREADS
  local kind=$1 name=$2 src=$3 pat=$4 min=$5 line
  line=$(grep -n -- "$pat" "$src" | head -1 | cut -d: -f1)
  echo "== $kind $name (break at $(basename "$src"):$line)"
  "$HERE/rtos-gdbserver.sh" "$kind" "$name" "$PORT" </dev/null >"$OUT/server.txt" 2>&1 &
  SERVER=$!
  for _ in $(seq 3000); do   # a Zephyr build can take a few minutes
    grep -q 'waiting for a debugger' "$OUT/server.txt" 2>/dev/null && break
    if ! kill -0 $SERVER 2>/dev/null; then tail -20 "$OUT/server.txt"; echo "FAIL: $kind $name server"; FAIL=1; return; fi
    sleep 0.1
  done
  if ! python3 "$HERE/dap_rtos.py" "$LLVM_BIN/lldb-dap" "$ROOT" "$PORT" "$src" "$line" "$min"; then
    FAIL=1
    tail -5 "$OUT/server.txt"
  fi
  kill $SERVER 2>/dev/null || true
  wait $SERVER 2>/dev/null || true
  SERVER=
}
run freertos demo "$ROOT/freertos/demo/demo.c" 'safe_printf("\[B\]' 5
run zephyr synchronization "$ROOT/zephyr-ws/zephyr/samples/synchronization/src/main.c" 'k_busy_wait' 3
[ "$FAIL" -eq 0 ] || { echo "RTOS debug smoke test: FAIL"; exit 1; }
echo "RTOS debug smoke test: PASS"
