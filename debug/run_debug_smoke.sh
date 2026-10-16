#!/usr/bin/env bash
# Debugger smoke test on the emulator: build baremetal/programs/dbgtest.c with
# -O1 -g, serve it with `klausscc --gdbserver`, drive lldb with smoke.lldb and
# check breakpoints, backtraces, variables, step-out return values,
# conditional breakpoints, watchpoints, the debugtrap and the exit status;
# then drive lldb-dap over DAP the way VS Code does (dap_smoke.py).
#
#   KLAUSSCPU_LLVM_BIN  toolchain bin dir (default ../klausscpu-llvm/build/bin)
#   KLAUSSCC            klausscc binary   (default ../klausscc/target/release/klausscc)
#   PORT                gdbserver port    (default 5199)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$ROOT/../klausscpu-llvm/build/bin}
KLAUSSCC=${KLAUSSCC:-$ROOT/../klausscc/target/release/klausscc}
PORT=${PORT:-5199}
export KLAUSSCPU_LLVM_BIN=$LLVM_BIN

make -C "$ROOT/baremetal" -B dbgtest.elf OPT="-O1 -g" >/dev/null
ELF=$ROOT/baremetal/dbgtest.elf
OUT=$(mktemp -d)
"$KLAUSSCC" --input "$ELF" --gdbserver "$PORT" </dev/null >"$OUT/uart.txt" 2>"$OUT/server.txt" &
SERVER=$!
trap 'kill $SERVER 2>/dev/null || true; rm -rf "$OUT"' EXIT
for _ in $(seq 100); do
  grep -q 'waiting for a debugger' "$OUT/server.txt" 2>/dev/null && break
  sleep 0.1
done

sed "s|@ELF@|$ELF|; s|@PORT@|$PORT|" "$HERE/smoke.lldb" >"$OUT/smoke.lldb"
"$LLVM_BIN/lldb" -b -s "$OUT/smoke.lldb" >"$OUT/lldb.txt" 2>&1 || true
wait "$SERVER" || true

FAIL=0
check() {  # check FILE TEXT
  if ! grep -qF -- "$2" "$OUT/$1"; then
    echo "FAIL: '$2' not in $1"
    FAIL=1
  fi
}
check lldb.txt 'stop reason = breakpoint 1.1'
check lldb.txt 'frame #1: '
check lldb.txt 'main at dbgtest.c'
check lldb.txt '(const point) *a = (x = 1, y = 2)'
check lldb.txt '(const point) *b = (x = 4, y = 6)'
check lldb.txt 'square(v=-3)'
check lldb.txt 'Return value: (long) $0 = 9'
check lldb.txt 'fib(n=1) at dbgtest.c'
check lldb.txt 'fib(n=6) at dbgtest.c'
check lldb.txt 'Watchpoint 1 hit'
check lldb.txt 'new value: 3'
check lldb.txt '(int) i = 4'
check lldb.txt 'received signal: SIGTRAP'
check lldb.txt 'exited with status = 33'
check uart.txt 'd=25 fib=8 counter=6'
check server.txt 'program halted (exit status 33)'

# The same program through lldb-dap, the way VS Code drives it.
"$KLAUSSCC" --input "$ELF" --gdbserver "$((PORT + 1))" </dev/null >"$OUT/uart2.txt" 2>"$OUT/server2.txt" &
SERVER=$!
for _ in $(seq 100); do
  grep -q 'waiting for a debugger' "$OUT/server2.txt" 2>/dev/null && break
  sleep 0.1
done
LINE=$(grep -n 'long dx = a->x - b->x;' "$ROOT/baremetal/programs/dbgtest.c" | cut -d: -f1)
if ! python3 "$HERE/dap_smoke.py" "$LLVM_BIN/lldb-dap" "$ELF" "$((PORT + 1))" \
     "$ROOT/baremetal/programs/dbgtest.c" "$LINE" >"$OUT/dap.txt" 2>&1; then
  echo "FAIL: lldb-dap (VS Code path) checks"
  FAIL=1
fi
wait "$SERVER" || true

if [ "$FAIL" -ne 0 ]; then
  echo "---- dap ----"; cat "$OUT/dap.txt" 2>/dev/null || true
  echo "---- lldb ----"; cat "$OUT/lldb.txt"
  echo "---- server ----"; cat "$OUT/server.txt"
  exit 1
fi
grep -E '^(ok|FAIL) ' "$OUT/dap.txt"
echo "debug smoke test: PASS"
