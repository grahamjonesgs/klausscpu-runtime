#!/usr/bin/env bash
# linux/cxx/build.sh — the C++ runtime for KlaussCPU Linux user programs:
# libunwind, libc++abi and libc++ from klausscpu-llvm, built with its clang
# (llvm/runtimes, CMake) against the shared musl sysroot, and installed into
# that sysroot: include/c++/v1, lib/libc++.{a,so}, lib/libc++abi.{a,so},
# lib/libunwind.{a,so}.  The driver then builds and links C++ with plain
# `clang++ --sysroot=<sysroot> x.cpp` (CLANG_ON_BOARD_PLAN.md M2).
#   KLAUSSCPU_LLVM_BIN  toolchain bin dir (default ../klausscpu-llvm/build/bin)
#   KLAUSSCPU_SYSROOT   musl sysroot with libc.so (default linux-ws/dyn/sysroot,
#                       from MUSL_SHARED=1 LINUX_WS=linux-ws/dyn linux/musl/build.sh)
#   CXX_WS              build directory (default linux-ws/cxx)
#   CXX_RECONFIGURE=1   configure again from scratch
# The archives are -fPIC (they go into PIEs and static programs alike); the
# shared libraries are what the board's system image will carry.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
export KLAUSSCPU_LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$(cd "$RT/../klausscpu-llvm/build/bin" && pwd)}
export KLAUSSCPU_SYSROOT=${KLAUSSCPU_SYSROOT:-$RT/linux-ws/dyn/sysroot}
WS=${CXX_WS:-$RT/linux-ws/cxx}
LLVM_SRC=$(cd "$KLAUSSCPU_LLVM_BIN/../.." && pwd)
[ -f "$KLAUSSCPU_SYSROOT/lib/libc.so" ] || {
  echo "cxx: no shared musl sysroot at $KLAUSSCPU_SYSROOT" >&2; exit 1; }
[ -f "$("$KLAUSSCPU_LLVM_BIN/clang" --target=klausscpu-unknown-linux-musl \
        -rtlib=compiler-rt -print-libgcc-file-name)" ] || {
  echo "cxx: no Linux-triple builtins (BUILTINS_TRIPLE=klausscpu-unknown-linux-musl build-builtins.sh)" >&2; exit 1; }

BUILD=$WS/build
if [ "${CXX_RECONFIGURE:-0}" = 1 ]; then rm -rf "$BUILD"; fi
mkdir -p "$BUILD"
if [ ! -f "$BUILD/build.ninja" ]; then
  cmake -G Ninja -S "$LLVM_SRC/runtimes" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$HERE/klausscpu-linux.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$KLAUSSCPU_SYSROOT" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DLLVM_ENABLE_RUNTIMES="libunwind;libcxxabi;libcxx" \
    -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLIBUNWIND_USE_COMPILER_RT=ON \
    -DLIBUNWIND_ENABLE_SHARED=ON -DLIBUNWIND_ENABLE_STATIC=ON \
    -DLIBUNWIND_INCLUDE_TESTS=OFF \
    -DLIBCXXABI_USE_COMPILER_RT=ON \
    -DLIBCXXABI_USE_LLVM_UNWINDER=ON \
    -DLIBCXXABI_ENABLE_SHARED=ON -DLIBCXXABI_ENABLE_STATIC=ON \
    -DLIBCXXABI_INCLUDE_TESTS=OFF \
    -DLIBCXX_USE_COMPILER_RT=ON \
    -DLIBCXX_HAS_MUSL_LIBC=ON \
    -DLIBCXX_CXX_ABI=libcxxabi \
    -DLIBCXX_ENABLE_SHARED=ON -DLIBCXX_ENABLE_STATIC=ON \
    -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF \
    -DLIBCXX_INSTALL_MODULES=OFF \
    >"$WS/configure.log" 2>&1 || { tail -40 "$WS/configure.log"; exit 1; }
fi
ninja -C "$BUILD" >"$WS/build.log" 2>&1 || { grep -E "error|FAILED" "$WS/build.log" | head -40; exit 1; }
ninja -C "$BUILD" install >"$WS/install.log" 2>&1 || { tail -20 "$WS/install.log"; exit 1; }
echo "cxx: installed into $KLAUSSCPU_SYSROOT"
ls -l "$KLAUSSCPU_SYSROOT"/lib/lib{c++,c++abi,unwind}.* 2>/dev/null
