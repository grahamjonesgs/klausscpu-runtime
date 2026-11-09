#!/usr/bin/env bash
# linux/musl/build.sh — musl for KlaussCPU Linux user programs.
#
# Links this port (arch/klausscpu and src/*/klausscpu) into the musl checkout
# in linux-ws/musl (cloned at the pinned tag if missing), writes config.mak
# directly (static only: there is no dynamic linker yet), builds libc.a and
# the crt objects with klausscpu-llvm and installs a sysroot in
# linux-ws/sysroot: clang --target=klausscpu-unknown-linux-musl
# --sysroot=linux-ws/sysroot then compiles and links static programs.
#   KLAUSSCPU_LLVM_BIN  toolchain bin dir (default ../klausscpu-llvm/build/bin)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
SRC=$WS/musl
SYSROOT=$WS/sysroot
TAG=v1.2.5
if [ ! -d "$SRC/.git" ]; then
  git clone -q --depth 1 --branch "$TAG" https://git.musl-libc.org/git/musl "$SRC"
fi
ln -sfn "$HERE/arch/klausscpu" "$SRC/arch/klausscpu"
for d in "$HERE"/src/*/klausscpu; do
  rel=${d#"$HERE"/}
  ln -sfn "$d" "$SRC/$rel"
done
BUILTINS=$("$B/clang" --target=klausscpu-unknown-elf -rtlib=compiler-rt -print-libgcc-file-name)
cat >"$SRC/config.mak" <<MAK
ARCH = klausscpu
SUBARCH =
ASMSUBARCH =
srcdir = .
prefix = $SYSROOT
exec_prefix = \$(prefix)
bindir = \$(exec_prefix)/bin
libdir = \$(prefix)/lib
includedir = \$(prefix)/include
syslibdir = /lib
CC = $B/clang --target=klausscpu-unknown-linux-musl
AR = $B/llvm-ar
RANLIB = $B/llvm-ranlib
CFLAGS =
CFLAGS_AUTO = -Os -pipe -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Werror=implicit-function-declaration -Werror=implicit-int -Werror=pointer-sign -Werror=pointer-arith -Wno-unused-command-line-argument
CFLAGS_C99FSE = -std=c99 -nostdinc -ffreestanding -frounding-math -fno-strict-aliasing
CFLAGS_MEMOPS =
CFLAGS_NOSSP = -fno-stack-protector
CPPFLAGS =
LDFLAGS =
LDFLAGS_AUTO =
CROSS_COMPILE =
LIBCC = $BUILTINS
OPTIMIZE_GLOBS = internal/*.c malloc/*.c string/*.c
ALL_TOOLS =
TOOL_LIBS =
ADD_CFI = no
MALLOC_DIR = mallocng
WRAPCC_GCC = \$(CC)
AOBJS = \$(LOBJS)
SHARED_LIBS =
MAK
make -C "$SRC" -j"${JOBS:-$(nproc)}" >"$WS/musl-build.log" 2>&1 || { tail -30 "$WS/musl-build.log"; exit 1; }
make -C "$SRC" install-headers install-libs >>"$WS/musl-build.log" 2>&1
echo "musl: sysroot $SYSROOT"
