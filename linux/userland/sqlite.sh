#!/usr/bin/env bash
# linux/userland/sqlite.sh — the static sqlite3 shell (SQLite amalgamation)
# for the KlaussCPU Linux port, against linux/musl's sysroot, into
# linux-ws/sqlite-build/sqlite3; build-rootfs.sh puts it in the system
# image.  -O2: the amalgamation is a good compiler test in its own right.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
V=3540100			# 3.54.1
URL=https://www.sqlite.org/2026/sqlite-amalgamation-$V.zip
SHA3=92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30
SRC=$WS/sqlite
OUT=$WS/sqlite-build
OPT=${SQLITE_OPT:--O2}
[ -f "$WS/sysroot/lib/libc.a" ] || { echo "no musl sysroot: run linux/musl/build.sh" >&2; exit 1; }
if [ ! -f "$SRC/sqlite3.c" ]; then
  zip=$WS/$(basename "$URL")
  [ -f "$zip" ] || curl -fsSL --retry 5 -o "$zip" "$URL"
  got=$(openssl dgst -sha3-256 -r "$zip" | cut -d' ' -f1)
  [ "$got" = "$SHA3" ] || { echo "$zip: bad SHA3-256 ($got)" >&2; rm -f "$zip"; exit 1; }
  rm -rf "$SRC" "$WS/sqlite-amalgamation-$V"
  (cd "$WS" && unzip -q "$zip") && mv "$WS/sqlite-amalgamation-$V" "$SRC"
fi
rm -rf "$OUT"; mkdir -p "$OUT"
CC=("$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot")
DEFS=(-DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_ENABLE_MATH_FUNCTIONS -DSQLITE_DQS=0
      -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_THREADSAFE=1 -DHAVE_USLEEP=1
      # One section per function: sqlite3.o's .text alone is beyond the
      # branch range, and lld can only put range-extension thunks between
      # input sections ("InputSection too large for range extension thunk").
      -ffunction-sections -fdata-sections)
"${CC[@]}" $OPT "${DEFS[@]}" -c "$SRC/sqlite3.c" -o "$OUT/sqlite3.o" &
"${CC[@]}" $OPT "${DEFS[@]}" -DSQLITE_SHELL_IS_UTF8=1 -c "$SRC/shell.c" -o "$OUT/shell.o" &
wait
"${CC[@]}" -static -Wl,--gc-sections -o "$OUT/sqlite3" "$OUT/shell.o" "$OUT/sqlite3.o" -lm -lpthread
echo "sqlite: $OUT/sqlite3 (SQLite $V, $OPT)"
