#!/usr/bin/env bash
# linux/rustc/build-llvm.sh — the LLVM libraries for a rustc that runs on the
# board (RUST_ON_BOARD_PLAN.md R2): klausscpu-llvm cross-built for
# klausscpu-unknown-linux-musl against the board's libc++, as
# linux/toolchain/build-native-llvm.sh builds clang, but:
#   - LLVM only (rustc links the component libraries into librustc_driver.so);
#   - PIC, because they go into that shared library;
#   - threads on: rustc refuses an LLVM without them (LLVMIsMultithreaded);
#   - no LTO and no PGO: the libraries must be machine code for rustc's link.
# Output in klausscpu-llvm/build-native-rustc-llvm (RUSTC_LLVM_BUILD).
#   KLAUSSCPU_LLVM_BIN  the host toolchain (default ../klausscpu-llvm/build/bin)
#   KLAUSSCPU_SYSROOT   default linux-ws/dyn/sysroot (musl + libc++)
#   RUSTC_LLVM_BUILD_TYPE  CMAKE_BUILD_TYPE (default MinSizeRel: the board's
#                       RAM is the limit, R8 measures Release)
#   RUSTC_LLVM_RECONFIGURE=1  configure again from scratch
# Then linux/rustc/llvm-config (a host script) answers rustc's llvm-config
# questions for this build.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
export KLAUSSCPU_LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$(cd "$RT/../klausscpu-llvm/build/bin" && pwd)}
export KLAUSSCPU_SYSROOT=${KLAUSSCPU_SYSROOT:-$RT/linux-ws/dyn/sysroot}
LLVM_SRC=$(cd "$KLAUSSCPU_LLVM_BIN/../.." && pwd)
BUILD=${RUSTC_LLVM_BUILD:-$LLVM_SRC/build-native-rustc-llvm}
B=$KLAUSSCPU_LLVM_BIN
[ -f "$KLAUSSCPU_SYSROOT/lib/libc++.a" ] || { echo "no libc++ in $KLAUSSCPU_SYSROOT (linux/cxx/build.sh)" >&2; exit 1; }
[ "${RUSTC_LLVM_RECONFIGURE:-0}" = 1 ] && rm -rf "$BUILD"
mkdir -p "$BUILD"
if [ ! -f "$BUILD/build.ninja" ]; then
  cmake -G Ninja -S "$LLVM_SRC/llvm" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$RT/linux/cxx/klausscpu-linux.cmake" \
    -DCMAKE_BUILD_TYPE=${RUSTC_LLVM_BUILD_TYPE:-MinSizeRel} \
    -DLLVM_ENABLE_PROJECTS= \
    -DLLVM_TARGETS_TO_BUILD="" -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=KlaussCPU \
    -DLLVM_HOST_TRIPLE=klausscpu-unknown-linux-musl \
    -DLLVM_DEFAULT_TARGET_TRIPLE=klausscpu-unknown-linux-musl \
    -DLLVM_TABLEGEN="$B/llvm-tblgen" -DLLVM_NATIVE_TOOL_DIR="$B" \
    -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_ENABLE_PIC=ON \
    -DLLVM_ENABLE_LIBCXX=ON \
    -DLLVM_ENABLE_THREADS=ON \
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF \
    -DLLVM_ENABLE_PLUGINS=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_DOCS=OFF -DLLVM_BUILD_TOOLS=OFF \
    >"$BUILD/configure.log" 2>&1 || { tail -40 "$BUILD/configure.log"; exit 1; }
fi
ninja -C "$BUILD" llvm-libraries
