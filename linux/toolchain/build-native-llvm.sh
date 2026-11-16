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
#   NATIVE_ORDER=1      link with lld --symbol-ordering-file from
#                       linux/toolchain/clang-order.txt.gz: a call-graph
#                       order.  It is lld's cdsort layout (the default
#                       --call-graph-profile-sort) of the call edges klausscc's
#                       profiler counted (KLAUSSCC_PROFILE .calls, klausscc
#                       tools/tlbord), then the executed functions with no
#                       recorded call edge, hottest first.  Edges from the
#                       Release binary on 10 workloads, each normalised:
#                       hello.i -O0/-O2, zlib deflate.c -O0/-O2, bzip2
#                       compress.c -O2, the C++ hello -O0/-O2, tests/cxx eh.cpp
#                       -std=c++23 -O2, the driver on hello.c with the
#                       sysroot's headers, and a link (ld.lld).  23287
#                       functions; the rest stay in link order after them.
#                       On the board's 121 MB the 90 MB binary's text pages
#                       are evicted and re-read from the SD card during a
#                       compile (11-17% of the time) unless the code a compile
#                       runs is packed.  The old order (hottest first) packed
#                       it, but mixed clang's phases into one block and
#                       doubled the TLB refills.  This one keeps callers next
#                       to callees.  klausscc emulator, 32-entry TLB, unordered
#                       / hottest-first / this order: main-TLB refills deflate
#                       -O2 13.9 / 28.4 / 6.6 M, C++ hello -O2 31.1 / 51.3 /
#                       14.9 M; not in the training set: bzip2 decompress.c
#                       -O2 20.0 / 42.1 / 10.9 M, tests/cxx terminate.cpp -O2
#                       1.05 / 2.00 / 0.48 M.  Micro-ITLB refills drop about
#                       as much.  64 KiB text pages touched per compile, in
#                       the same order: 828 / 474 / 303, 892 / 560 / 307,
#                       818 / 495 / 378, 864 / 513 / 308
#                       (CLANG_ON_BOARD_PLAN.md M6).  The link flags are set on
#                       every run, so it also relinks an existing build.
#   NATIVE_MALLOC       the allocator linked into the binary: mallocng (default,
#                       the sysroot's libc.a) or oldmalloc: a libc.a from the
#                       same musl tree and patches built with MALLOC_DIR=oldmalloc
#                       (configure's --with-malloc=oldmalloc) by linux/musl/build.sh
#                       in NATIVE_MALLOC_WS (default linux-ws/musl-oldmalloc;
#                       built or brought up to date on every run).  Its lib/ goes
#                       first on the search path (-L), so -lc takes that libc.a
#                       whole: no mallocng object is linked, and nothing else in
#                       libc changes.  Only this binary's link changes, not the
#                       system's libc (CLANG_ON_BOARD_PLAN.md M6).
#                       Measured on the emulator (2026-11-15): -0.3 to -0.7% user
#                       instructions, but the C++ hello's anonymous peak grows 11%
#                       (40 -> 45 MB), squeezing clang's text out of the page cache
#                       (3x the major faults): not the default.
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
LDFLAGS="-static"
if [ "${NATIVE_ORDER:-0}" = 1 ]; then
  gunzip -c "$HERE/clang-order.txt.gz" >"$BUILD/clang-order.txt"
  LDFLAGS="$LDFLAGS -Wl,--symbol-ordering-file=$BUILD/clang-order.txt -Wl,--no-warn-symbol-ordering"
fi
case ${NATIVE_MALLOC:-mallocng} in
mallocng) ;;
oldmalloc)
  MWS=${NATIVE_MALLOC_WS:-$RT/linux-ws/musl-oldmalloc}
  LINUX_WS=$MWS LINUX_KERNEL_WS=${LINUX_KERNEL_WS:-$RT/linux-ws} MUSL_MALLOC=oldmalloc \
    "$RT/linux/musl/build.sh" >/dev/null
  LDFLAGS="$LDFLAGS -L$MWS/sysroot/lib" ;;
*) echo "NATIVE_MALLOC is mallocng or oldmalloc" >&2; exit 1 ;;
esac
cmake -S "$LLVM_SRC/llvm" -B "$BUILD" -DCMAKE_EXE_LINKER_FLAGS="$LDFLAGS" >>"$BUILD/configure.log" 2>&1 ||
  { tail -5 "$BUILD/configure.log"; exit 1; }
ninja -C "$BUILD" $TARGETS
