#!/usr/bin/env bash
# Build baremetal/programs/<name>.c with -O1 -g and serve baremetal/<name>.elf
# on the emulator for lldb / VS Code (used by .vscode/tasks.json).
#
#   debug/emu-gdbserver.sh <name> [port]
#
#   KLAUSSCPU_LLVM_BIN  toolchain bin dir (default ../klausscpu-llvm/build/bin)
#   KLAUSSCC            klausscc binary   (default ../klausscc/target/release/klausscc)
#   OPT                 optimisation flags (default "-O1 -g")
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NAME=${1:?usage: emu-gdbserver.sh <program> [port]}
NAME=${NAME%.c}
NAME=${NAME%.elf}
PORT=${2:-1234}
export KLAUSSCPU_LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$ROOT/../klausscpu-llvm/build/bin}
KLAUSSCC=${KLAUSSCC:-$ROOT/../klausscc/target/release/klausscc}

make -C "$ROOT/baremetal" -B "$NAME.elf" OPT="${OPT:--O1 -g}"
exec "$KLAUSSCC" --input "$ROOT/baremetal/$NAME.elf" --gdbserver "$PORT"
