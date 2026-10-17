#!/usr/bin/env bash
# Build a FreeRTOS program or a Zephyr app with debug info and serve it for
# lldb / VS Code, on the emulator or (BOARD=1) on the board over JTAG (used by
# .vscode/tasks.json).  The ELF is also linked to debug/.rtos.elf, which the
# launch configurations open, so VS Code asks for the program only once.
#
#   debug/rtos-gdbserver.sh freertos <program> [port]   freertos/<program>.elf
#                                                        (demo, console_demo, net_demo)
#   debug/rtos-gdbserver.sh zephyr <app> [port]         klausscpu-zephyr/apps/<app>,
#                                                        else zephyr/samples/<app>
#
#   KLAUSSCPU_LLVM_BIN  toolchain bin dir (default ../klausscpu-llvm/build/bin)
#   KLAUSSCC            klausscc binary   (default ../klausscc/target/release/klausscc)
#   BOARD=1             debug on the board (see board-gdbserver.sh): load over the
#                       UART halted at the entry; SERIAL / JTAG_KHZ as there
#
# FreeRTOS builds with `make DBG=-g` (-O1 -g).  Zephyr builds into
# zephyr-ws/build_dbg_<app> (incremental; delete it for a pristine build) with
# debug info and the thread list the OS plugin needs (CONFIG_THREAD_MONITOR,
# CONFIG_THREAD_NAME), at the app's own optimisation level.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
KIND=${1:?usage: rtos-gdbserver.sh freertos|zephyr <program> [port]}
NAME=${2:?usage: rtos-gdbserver.sh freertos|zephyr <program> [port]}
PORT=${3:-1234}
export KLAUSSCPU_LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$ROOT/../klausscpu-llvm/build/bin}
KLAUSSCC=${KLAUSSCC:-$ROOT/../klausscc/target/release/klausscc}
echo "klausscpu-rtos: building $KIND $NAME"

case $KIND in
  freertos)
    NAME=${NAME%.elf}
    # -B: objects from an earlier build without -g would otherwise be reused
    make -C "$ROOT/freertos" -B "$NAME.elf" DBG=-g
    ELF=$ROOT/freertos/$NAME.elf
    ;;
  zephyr)
    WS=$ROOT/zephyr-ws
    MOD=$WS/klausscpu-zephyr
    ZEPHYR=$WS/zephyr
    if [ -d "$MOD/apps/$NAME" ]; then APP=$MOD/apps/$NAME
    elif [ -d "$ZEPHYR/samples/$NAME" ]; then APP=$ZEPHYR/samples/$NAME
    elif [ -d "$NAME" ]; then APP=$(cd "$NAME" && pwd); NAME=$(basename "$APP")
    else echo "klausscpu-rtos: cannot find Zephyr app '$NAME'" >&2; exit 1
    fi
    BUILD=$WS/build_dbg_$NAME
    # shellcheck disable=SC1091
    source "$WS/.venv/bin/activate"
    (cd "$WS" && west build -b nexys_a7 "$APP" --build-dir "$BUILD" -- \
      -DZEPHYR_TOOLCHAIN_VARIANT=klausscpu-clang \
      -DKLAUSSCPU_LLVM_BIN="$KLAUSSCPU_LLVM_BIN" \
      -DTOOLCHAIN_ROOT="$MOD" -DEXTRA_ZEPHYR_MODULES="$MOD" \
      "-DARCH_ROOT=$MOD;$ZEPHYR" "-DSOC_ROOT=$MOD;$ZEPHYR" \
      "-DBOARD_ROOT=$MOD;$ZEPHYR" "-DDTS_ROOT=$MOD;$ZEPHYR" \
      -DCONFIG_DEBUG_INFO=y -DCONFIG_THREAD_MONITOR=y -DCONFIG_THREAD_NAME=y)
    ELF=$BUILD/zephyr/zephyr.elf
    ;;
  *)
    echo "klausscpu-rtos: unknown kind '$KIND' (freertos or zephyr)" >&2
    exit 1
    ;;
esac

ln -sf "$ELF" "$ROOT/debug/.rtos.elf"
ARGS=(--input "$ELF" --gdbserver "$PORT")
if [ "${BOARD:-0}" = 1 ]; then
  ARGS+=(--board --jtag-khz "${JTAG_KHZ:-10000}")
  if [ -n "${SERIAL:-}" ]; then ARGS+=(-s "$SERIAL"); else ARGS+=(-s); fi
fi
exec "$KLAUSSCC" "${ARGS[@]}"
