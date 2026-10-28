#!/usr/bin/env bash
# zephyr/build.sh <app> <build-dir> [extra cmake -D...]
#
# Pristine `west build` of one Zephyr app for the nexys_a7 board with the
# KlaussCPU toolchain (the invocation in zephyr-ws/klausscpu-zephyr/CLAUDE.md).
# <app> is one of the port's apps (zephyr-ws/klausscpu-zephyr/apps/<app>),
# `hello` (samples/hello_world), `dbg_synchronization` (samples/synchronization
# with debug info and thread names), or an absolute source path.
#
#   ZEPHYR_WS   west workspace holding zephyr/, modules/ and .venv/
#               (default <runtime>/zephyr-ws).  The port itself
#               (klausscpu-zephyr) and wolfSSL/wolfSSH always come from the
#               runtime checkout being validated.
set -uo pipefail
. "$(dirname "$0")/../common.sh"
[ $# -ge 2 ] || die "usage: zephyr/build.sh <app> <build-dir> [-D...]"
APP=$1; BD=$(realpath -m "$2"); shift 2
WS=$(cd "${ZEPHYR_WS:-$RT/zephyr-ws}" && pwd)
MOD=$RT/zephyr-ws/klausscpu-zephyr
ZEPHYR=$WS/zephyr
[ -d "$ZEPHYR/kernel" ] || die "no Zephyr tree in $ZEPHYR (west update, or set ZEPHYR_WS)"
WOLF="$RT/freertos/wolfssl/wolfssl-src;$RT/freertos/wolfssl/wolfssh-src"
EXTRA=()
case $APP in
  hello) SRC=$ZEPHYR/samples/hello_world; MODS="$MOD" ;;
  dbg_synchronization) SRC=$ZEPHYR/samples/synchronization; MODS="$MOD"
    EXTRA=(-DCONFIG_DEBUG_INFO=y -DCONFIG_THREAD_MONITOR=y -DCONFIG_THREAD_NAME=y) ;;
  doom) SRC=$MOD/apps/doom; MODS="$MOD" ;;
  /*) SRC=$APP; MODS="$MOD" ;;
  *) SRC=$MOD/apps/$APP; MODS="$MOD;$WOLF" ;;
esac
if [ -e "$WS/.venv/bin/activate" ]; then
  # shellcheck disable=SC1091
  . "$WS/.venv/bin/activate"
fi
command -v west >/dev/null || die "west not found (no $WS/.venv and not on PATH)"
cd "$WS" && west build -p always -b nexys_a7 "$SRC" --build-dir "$BD" -- \
  -DZEPHYR_TOOLCHAIN_VARIANT=klausscpu-clang -DKLAUSSCPU_LLVM_BIN="$KLAUSSCPU_LLVM_BIN" \
  -DTOOLCHAIN_ROOT="$MOD" "-DEXTRA_ZEPHYR_MODULES=$MODS" \
  "-DARCH_ROOT=$MOD;$ZEPHYR" "-DSOC_ROOT=$MOD;$ZEPHYR" "-DBOARD_ROOT=$MOD;$ZEPHYR" "-DDTS_ROOT=$MOD;$ZEPHYR" \
  "${EXTRA[@]}" "$@"
