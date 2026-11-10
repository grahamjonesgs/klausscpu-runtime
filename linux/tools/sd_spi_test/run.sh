#!/usr/bin/env bash
# linux/tools/sd_spi_test/run.sh [iterations] [seed]: build and run the host
# test of the SD card protocol code (sd_spi.c) against the controller and
# card model (sim.c).  The binary goes to linux-ws/sd_spi_test/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../.." && pwd)
OUT=${LINUX_WS:-$RT/linux-ws}/sd_spi_test
mkdir -p "$OUT"
${CC:-cc} -O2 -g -Wall -Wextra -Wno-unused-parameter -Werror \
  -I"$HERE/shim" -I"$RT/linux/arch/klausscpu/kernel" \
  "$HERE/sim.c" "$RT/linux/arch/klausscpu/kernel/sd_spi.c" -o "$OUT/sd_spi_test"
exec "$OUT/sd_spi_test" "$@"
