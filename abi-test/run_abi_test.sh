#!/usr/bin/env bash
# run_abi_test.sh [FIRST_SEED] [COUNT] [NFUNCS]
#
# ABI conformance test.  For each seed, gen_abi_test.py writes a caller and a
# callee translation unit (random argument/return type mixes: signed and
# unsigned 8-64-bit integers, pointers, float/double, __int128, structs and
# unions of 1-24 bytes, variadic functions).  Host gcc gives the expected
# output; then every combination of caller and callee optimisation level
# ($OPTS, default -O0 -O1 -O2 -Os -Oz; a level can carry more flags joined by
# '+', "-Os+-moutline") is built for KlaussCPU, run on the klausscc emulator
# and compared line by line.
#
#   KLAUSSCPU_LLVM_BIN  toolchain bin dir (default ../klausscpu-llvm/build/bin)
#   KLAUSSCC            klausscc binary (default ../klausscc/target/release/klausscc)
#   PICOLIBC            picolibc install (default ./picolibc-install), built
#                       with the same ABI as the toolchain
#   OUT                 work directory (default: a temporary one, removed)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(dirname "$HERE")
LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
EMU=${KLAUSSCC:-$RT/../klausscc/target/release/klausscc}
PICO=${PICOLIBC:-$RT/picolibc-install}
OPTS=${OPTS:-"-O0 -O1 -O2 -Os -Oz"}
FIRST=${1:-1}; COUNT=${2:-4}; NFUNCS=${3:-60}
if [ -n "${OUT:-}" ]; then mkdir -p "$OUT"; else OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT; fi

CC="$LLVM_BIN/clang -target klausscpu-unknown-elf"
CFLAGS="-isystem $PICO/include -nostdlib -fPIC -D__IEEE_LITTLE_ENDIAN -D_LDBL_EQ_DBL -std=c2x -w"
BUILTINS=$($CC -rtlib=compiler-rt -print-libgcc-file-name)
mkdir -p "$OUT/rt"
for s in crt0 uart_stubs syscalls stdio_handles; do
  $CC $CFLAGS -O1 -I"$RT" -c "$RT/src/$s.c" -o "$OUT/rt/$s.o" || exit 2
done
RTOBJS="$OUT/rt/crt0.o $OUT/rt/uart_stubs.o $OUT/rt/syscalls.o $OUT/rt/stdio_handles.o"

fail=0; runs=0
for seed in $(seq "$FIRST" $((FIRST + COUNT - 1))); do
  W=$OUT/s$seed; mkdir -p "$W"
  python3 -I "$HERE/gen_abi_test.py" "$seed" "$W" "$NFUNCS" || exit 2
  gcc -std=c2x -O0 -w "$W/caller.c" "$W/callee.c" -o "$W/host" || exit 2
  "$W/host" > "$W/expected.txt" || exit 2
  for O in $OPTS; do
    $CC $CFLAGS ${O//+/ } -c "$W/caller.c" -o "$W/caller$O.o" || { echo "seed $seed: caller $O does not compile"; fail=1; continue; }
    $CC $CFLAGS ${O//+/ } -c "$W/callee.c" -o "$W/callee$O.o" || { echo "seed $seed: callee $O does not compile"; fail=1; continue; }
  done
  for OA in $OPTS; do
    for OB in $OPTS; do
      E=$W/t$OA$OB.elf
      "$LLVM_BIN/ld.lld" -T "$RT/klausscpu.ld" --gc-sections -o "$E" $RTOBJS \
        "$W/caller$OA.o" "$W/callee$OB.o" "$PICO/lib/libc.a" "$BUILTINS" || { fail=1; continue; }
      "$EMU" -i "$E" --emulate --max-instructions 200000000 </dev/null 2>&1 \
        | tr -d '\r' | grep -E '^(T[0-9]+ |ABI-DONE)' > "$W/got$OA$OB.txt"
      runs=$((runs + 1))
      if ! cmp -s "$W/expected.txt" "$W/got$OA$OB.txt"; then
        echo "seed $seed: caller $OA callee $OB MISMATCH"
        diff "$W/expected.txt" "$W/got$OA$OB.txt" | head -5
        fail=1
      fi
    done
  done
done
echo "abi-test: $runs runs, seeds $FIRST..$((FIRST + COUNT - 1)), $NFUNCS functions each: $([ $fail = 0 ] && echo PASS || echo FAIL)"
exit $fail
