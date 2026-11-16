#!/usr/bin/env bash
# linux/toolchain/pgo-profile.sh — the PGO profile for the board's clang
# (CLANG_ON_BOARD_PLAN.md M6), end to end: writes linux/toolchain/clang.profdata.xz,
# which build-native-llvm.sh uses with NATIVE_PROFDATA=default.
#
# The profile is taken on the HOST: an instrumented x86 clang+lld (the same
# multicall configuration as the board's, KlaussCPU target only, Release, no
# assertions, no threads), built by the host toolchain, compiling FOR
# KlaussCPU.  The work is the same as the board's, so the branch profile
# transfers: of 151,840 profiled functions 10% report a CFG hash mismatch
# (libstdc++ vs libc++ templates, 64- vs 32-bit), 8% by call count.
# Training workloads (the CDSX set of build-native-llvm.sh minus the two
# held-out ones, bzip2 decompress.c and tests/cxx terminate.cpp, which the
# profile has never seen): hello.i -O0/-O2, zlib deflate.c -O0/-O2, bzip2
# compress.c -O2, the C++ hello -O0/-O2, tests/cxx eh.cpp -std=c++23 -O2, the
# driver on hello.c with the sysroot's headers, and a link.  The inputs are
# preprocessed here, as tools/tlbord/run.sh does.
#
# Steps (each skipped if its output exists; PGO_REDO=1 redoes all):
#   1. compiler-rt's profile runtime for x86_64, built with the host toolchain
#      (build/bin/clang cannot link C++ on every host: PGO_GCC_DIR names the
#      gcc install directory it should use, default the newest with libstdc++),
#      in a copy of build/bin's resource directory (build/ itself is untouched);
#   2. klausscpu-llvm/build-pgo-instr: LLVM_BUILD_INSTRUMENTED=IR, built with
#      build/bin/clang (about 7 minutes on 24 cores);
#   3. training, one .profraw per process; llvm-profdata merge; xz -9.
# Output the profile's function names embed the source paths, so the profile
# only matches this checkout of klausscpu-llvm.  Delete build-pgo-instr when
# done (about 2 GB).
#   PGO_WS      scratch (default ~/.cache/pgo-lto/profile-ws)
#   PGO_OUT     output (default linux/toolchain/clang.profdata.xz)
#   KLAUSSCPU_LLVM_BIN, KLAUSSCPU_SYSROOT: as build-native-llvm.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
export KLAUSSCPU_LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$(cd "$RT/../klausscpu-llvm/build/bin" && pwd)}
export KLAUSSCPU_SYSROOT=${KLAUSSCPU_SYSROOT:-$RT/linux-ws/dyn/sysroot}
B=$KLAUSSCPU_LLVM_BIN
LL=$(cd "$B/../.." && pwd)
WS=${PGO_WS:-$HOME/.cache/pgo-lto/profile-ws}
OUT=${PGO_OUT:-$HERE/clang.profdata.xz}
INSTR=$LL/build-pgo-instr
T=klausscpu-unknown-linux-musl
mkdir -p "$WS"
[ "${PGO_REDO:-0}" != 1 ] || rm -rf "$WS/crt" "$WS/res" "$INSTR" "$WS/stage" "$WS/prof" "$WS/in"

GCC=${PGO_GCC_DIR:-}
if [ -z "$GCC" ]; then
  for d in $(ls -d /usr/lib/gcc/x86_64-linux-gnu/*/ 2>/dev/null | sort -V); do
    if [ -e "${d}libstdc++.so" ]; then GCC=${d%/}; fi
  done
fi
G=()
[ -z "$GCC" ] || G=("--gcc-install-dir=$GCC")

# 1. the profile runtime, in a private copy of the resource directory
VER=$("$B/clang" -dumpversion | cut -d. -f1)
if [ ! -f "$WS/res/lib/x86_64-unknown-linux-gnu/libclang_rt.profile.a" ]; then
  rm -rf "$WS/crt" "$WS/res"
  cmake -G Ninja -S "$LL/compiler-rt" -B "$WS/crt" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$B/clang" -DCMAKE_CXX_COMPILER="$B/clang++" -DCMAKE_ASM_COMPILER="$B/clang" \
    -DCMAKE_C_COMPILER_TARGET=x86_64-unknown-linux-gnu \
    -DCMAKE_C_FLAGS="${G[*]}" -DCMAKE_CXX_FLAGS="${G[*]}" -DCMAKE_ASM_FLAGS="${G[*]}" -DCMAKE_EXE_LINKER_FLAGS="${G[*]}" \
    -DLLVM_CMAKE_DIR="$LL/build/lib/cmake/llvm" -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
    -DCOMPILER_RT_BUILD_BUILTINS=OFF -DCOMPILER_RT_BUILD_SANITIZERS=OFF -DCOMPILER_RT_BUILD_XRAY=OFF \
    -DCOMPILER_RT_BUILD_LIBFUZZER=OFF -DCOMPILER_RT_BUILD_MEMPROF=OFF -DCOMPILER_RT_BUILD_ORC=OFF \
    -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF -DCOMPILER_RT_BUILD_GWP_ASAN=OFF -DCOMPILER_RT_INCLUDE_TESTS=OFF \
    -DCOMPILER_RT_BUILD_PROFILE=ON >"$WS/crt.log" 2>&1 || { tail -20 "$WS/crt.log"; exit 1; }
  ninja -C "$WS/crt" profile >>"$WS/crt.log" 2>&1 || { tail -20 "$WS/crt.log"; exit 1; }
  cp -a "$LL/build/lib/clang/$VER" "$WS/res"
  mkdir -p "$WS/res/lib/x86_64-unknown-linux-gnu"
  cp "$WS/crt/lib/linux/libclang_rt.profile-x86_64.a" "$WS/res/lib/x86_64-unknown-linux-gnu/libclang_rt.profile.a"
fi

# 2. the instrumented host build: the board's configuration, for the host
if [ ! -x "$INSTR/bin/llvm" ]; then
  F="${G[*]} -resource-dir=$WS/res"
  cmake -G Ninja -S "$LL/llvm" -B "$INSTR" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$B/clang" -DCMAKE_CXX_COMPILER="$B/clang++" \
    -DCMAKE_C_FLAGS="$F" -DCMAKE_CXX_FLAGS="$F" -DCMAKE_EXE_LINKER_FLAGS="$F" -DCMAKE_SHARED_LINKER_FLAGS="$F" \
    -DLLVM_USE_LINKER=lld -DLLVM_BUILD_INSTRUMENTED=IR \
    -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD="" -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=KlaussCPU \
    -DLLVM_TABLEGEN="$B/llvm-tblgen" -DCLANG_TABLEGEN="$B/clang-tblgen" -DLLVM_NATIVE_TOOL_DIR="$B" \
    -DLLVM_ENABLE_ASSERTIONS=OFF -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
    -DLLVM_ENABLE_TERMINFO=OFF -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_THREADS=OFF \
    -DLLVM_ENABLE_PLUGINS=OFF -DCLANG_PLUGIN_SUPPORT=OFF -DLLVM_ENABLE_UNWIND_TABLES=OFF \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_DOCS=OFF -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF \
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_OBJC_REWRITER=OFF \
    -DLLVM_TOOL_LLVM_DRIVER_BUILD=ON \
    "-DLLVM_DISTRIBUTION_COMPONENTS=clang;lld;llvm-ar;llvm-nm;llvm-objdump;llvm-readobj" \
    >"$WS/instr-conf.log" 2>&1 || { tail -20 "$WS/instr-conf.log"; exit 1; }
  ninja -C "$INSTR" llvm-driver >"$WS/instr-build.log" 2>&1 || { tail -20 "$WS/instr-build.log"; exit 1; }
fi

# the inputs (as tools/tlbord/run.sh makes them), preprocessed with the host compiler
IN=$WS/in
if [ ! -f "$IN/done" ]; then
  mkdir -p "$IN"; cd "$IN"
  P=("$B/clang" --target=$T --sysroot=$RT/linux-ws/dyn/sysroot)
  Z=$(ls -d "$RT"/linux-ws/br-output-dyn/build/libzlib-*/ | head -1)
  "${P[@]}" -E -DHAVE_UNISTD_H -I"$Z" "$Z/deflate.c" -o deflate.i
  BZ=$(ls -d "$RT"/linux-ws/br-output-dyn/build/bzip2-*/ | head -1)
  "${P[@]}" -E -I"$BZ" "$BZ/compress.c" -o bzcompress.i
  printf '#include <stdio.h>\nint main(void) { printf("hello\\n"); return 0; }\n' >hello.c
  "${P[@]}" -E hello.c -o hello.i
  "$B/clang++" --target=$T --sysroot=$RT/linux-ws/dyn/sysroot -E "$RT/../klausscc/tools/cachesim-ic/cxx-hello.cpp" -o cxx.ii
  CT=$RT/linux/userland/tests/cxx
  "$B/clang++" --target=$T --sysroot=$RT/linux-ws/dyn/sysroot -std=c++23 -I"$CT" -E "$CT/eh.cpp" -o eh.ii
  touch done
fi

# 3. training: the instrumented binary in the board's directory layout
S=$WS/stage
rm -rf "$S" "$WS/prof" "$WS/out"; mkdir -p "$S/bin" "$WS/prof" "$WS/out"
cp "$INSTR/bin/llvm" "$S/bin/llvm"
for t in clang clang++ ld.lld; do ln -s llvm "$S/bin/$t"; done
ln -s "$LL/build/lib" "$S/lib"                        # the resource directory
ln -s "$KLAUSSCPU_SYSROOT" "$S/klausscpu-linux-musl"  # the driver's default sysroot
export LLVM_PROFILE_FILE=$WS/prof/%p.profraw
C=$S/bin/clang; X=$S/bin/clang++; O=$WS/out
set -x
$C --target=$T -O0 -c "$IN/hello.i" -o "$O/hello-O0.o"
$C --target=$T -O2 -c "$IN/hello.i" -o "$O/hello-O2.o"
$C --target=$T -O0 -c "$IN/deflate.i" -o "$O/deflate-O0.o"
$C --target=$T -O2 -c "$IN/deflate.i" -o "$O/deflate-O2.o"
$C --target=$T -O2 -c "$IN/bzcompress.i" -o "$O/bzcompress-O2.o"
$X --target=$T -O0 -c "$IN/cxx.ii" -o "$O/cxx-O0.o"
$X --target=$T -O2 -c "$IN/cxx.ii" -o "$O/cxx-O2.o"
$X --target=$T -std=c++23 -O2 -c "$IN/eh.ii" -o "$O/eh-O2.o"
$C --target=$T -O2 -c "$IN/hello.c" -o "$O/drv.o"
$C --target=$T "$O/drv.o" -o "$O/hello"
set +x
unset LLVM_PROFILE_FILE
"$B/llvm-profdata" merge -o "$WS/clang.profdata" "$WS"/prof/*.profraw
"$B/llvm-profdata" show "$WS/clang.profdata" | tail -6
xz -9 -T1 -c "$WS/clang.profdata" >"$OUT"
ls -l "$OUT"
echo "pgo-profile: $OUT; delete $INSTR when done"
