#!/usr/bin/env bash
# linux/toolchain/build-native-llvm.sh [ninja targets...] — clang, lld and a
# few LLVM tools built FOR KlaussCPU Linux (CLANG_ON_BOARD_PLAN.md M3): a
# cross build of klausscpu-llvm with its own clang, against the shared musl
# sysroot with libc++ (linux/cxx/build.sh), linked statically.  Output in
# klausscpu-llvm/build-native-klausscpu (never build/).
#   KLAUSSCPU_LLVM_BIN  the host toolchain (default ../klausscpu-llvm/build/bin);
#                       its llvm-tblgen / clang-tblgen are the build's tablegens
#   KLAUSSCPU_SYSROOT   default linux-ws/dyn/sysroot
#   NATIVE_BUILD        build directory (default klausscpu-llvm/build-native-klausscpu)
#   NATIVE_RECONFIGURE=1  configure again from scratch
#   NATIVE_BUILD_TYPE   CMAKE_BUILD_TYPE (default MinSizeRel)
#   NATIVE_MULTICALL=1  one multicall binary, bin/llvm (LLVM_TOOL_LLVM_DRIVER_BUILD),
#                       with the tools as symlinks to it, as busybox does:
#                       the tools share one copy of LLVM (target llvm-driver)
# MinSizeRel, no assertions, only the KlaussCPU target, no zlib/zstd/
# libxml2/terminfo, no threads, no plugins.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
export KLAUSSCPU_LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$(cd "$RT/../klausscpu-llvm/build/bin" && pwd)}
export KLAUSSCPU_SYSROOT=${KLAUSSCPU_SYSROOT:-$RT/linux-ws/dyn/sysroot}
LLVM_SRC=$(cd "$KLAUSSCPU_LLVM_BIN/../.." && pwd)
BUILD=${NATIVE_BUILD:-$LLVM_SRC/build-native-klausscpu}
TARGETS=${*:-clang lld llvm-ar llvm-nm llvm-objdump llvm-readobj}
MULTI=()
if [ "${NATIVE_MULTICALL:-0}" = 1 ]; then
  MULTI=(-DLLVM_TOOL_LLVM_DRIVER_BUILD=ON
    "-DLLVM_DISTRIBUTION_COMPONENTS=clang;lld;llvm-ar;llvm-nm;llvm-objdump;llvm-readobj")
  TARGETS=${*:-llvm-driver}
fi
[ -f "$KLAUSSCPU_SYSROOT/lib/libc++.a" ] || { echo "no libc++ in $KLAUSSCPU_SYSROOT (linux/cxx/build.sh)" >&2; exit 1; }
B=$KLAUSSCPU_LLVM_BIN
if [ "${NATIVE_RECONFIGURE:-0}" = 1 ]; then rm -rf "$BUILD"; fi
mkdir -p "$BUILD"
if [ ! -f "$BUILD/build.ninja" ]; then
  cmake -G Ninja -S "$LLVM_SRC/llvm" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$RT/linux/cxx/klausscpu-linux.cmake" \
    -DCMAKE_BUILD_TYPE=${NATIVE_BUILD_TYPE:-MinSizeRel} \
    -DLLVM_ENABLE_PROJECTS="clang;lld" \
    -DLLVM_TARGETS_TO_BUILD="" -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=KlaussCPU \
    -DLLVM_HOST_TRIPLE=klausscpu-unknown-linux-musl \
    -DLLVM_DEFAULT_TARGET_TRIPLE=klausscpu-unknown-linux-musl \
    -DLLVM_TABLEGEN="$B/llvm-tblgen" -DCLANG_TABLEGEN="$B/clang-tblgen" \
    -DLLVM_NATIVE_TOOL_DIR="$B" \
    -DLLVM_ENABLE_ASSERTIONS=OFF \
    -DLLVM_ENABLE_PIC=OFF \
    -DLLVM_ENABLE_LIBCXX=ON \
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF \
    -DLLVM_ENABLE_THREADS=OFF \
    -DLLVM_ENABLE_PLUGINS=OFF -DCLANG_PLUGIN_SUPPORT=OFF \
    -DLLVM_ENABLE_UNWIND_TABLES=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF \
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF \
    -DCLANG_ENABLE_OBJC_REWRITER=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    "${MULTI[@]}" \
    >"$BUILD/configure.log" 2>&1 || { tail -40 "$BUILD/configure.log"; exit 1; }
fi
# Hot-function ordering (lld --symbol-ordering-file from a klausscc profile)
# was tried and made the board slower: CLANG_ON_BOARD_PLAN.md M6.
LDFLAGS="-static"
cmake -S "$LLVM_SRC/llvm" -B "$BUILD" -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" >>"$BUILD/configure.log" 2>&1 ||
  { tail -5 "$BUILD/configure.log"; exit 1; }
ninja -C "$BUILD" $TARGETS
