#!/usr/bin/env bash
# Build baremetal/programs/<name>.c with -O1 -g, load baremetal/<name>.elf onto
# the board over the UART with the debug module's halt-on-load set, and serve
# core 1 (halted on the program's first instruction) for lldb / VS Code over
# the board's USB-JTAG.  Needs a DEBUG_EN=1 bitstream on the FPGA and a
# klausscc built with `cargo build --release --features board`.
#
#   debug/board-gdbserver.sh <name> [port]
#
#   KLAUSSCPU_LLVM_BIN  toolchain bin dir (default ../klausscpu-llvm/build/bin)
#   KLAUSSCC            klausscc binary   (default ../klausscc/target/release/klausscc)
#   OPT                 optimisation flags (default "-O1 -g")
#   SERIAL              loader serial port (default: auto-detect)
#   ATTACH=1            don't load: halt the program already running
#   JTAG_KHZ            JTAG clock (default 10000)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NAME=${1:?usage: board-gdbserver.sh <program> [port]}
NAME=${NAME%.c}
NAME=${NAME%.elf}
PORT=${2:-1234}
export KLAUSSCPU_LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$ROOT/../klausscpu-llvm/build/bin}
KLAUSSCC=${KLAUSSCC:-$ROOT/../klausscc/target/release/klausscc}

ARGS=(--input "$ROOT/baremetal/$NAME.elf" --gdbserver "$PORT" --board --jtag-khz "${JTAG_KHZ:-10000}")
if [ "${ATTACH:-0}" != 1 ]; then
  make -C "$ROOT/baremetal" -B "$NAME.elf" OPT="${OPT:--O1 -g}"
  if [ -n "${SERIAL:-}" ]; then ARGS+=(-s "$SERIAL"); else ARGS+=(-s); fi
fi
exec "$KLAUSSCC" "${ARGS[@]}"
