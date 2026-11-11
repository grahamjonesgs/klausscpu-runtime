#!/usr/bin/env bash
# linux/musl/build.sh — musl for KlaussCPU Linux user programs.
#
# Links this port (arch/klausscpu and src/*/klausscpu) into the musl checkout
# in linux-ws/musl (cloned at the pinned tag if missing), applies patches/
# (generic changes the port needs, gated on arch macros: the dynamic linker
# with an ELF class narrower than the pointer size, an arch ELF header check,
# <link.h>'s ElfW), writes config.mak directly, builds libc.a and the
# crt objects with klausscpu-llvm and installs a sysroot in linux-ws/sysroot:
# clang --target=klausscpu-unknown-linux-musl --sysroot=linux-ws/sysroot then
# compiles and links static programs.
#   KLAUSSCPU_LLVM_BIN  toolchain bin dir (default ../klausscpu-llvm/build/bin)
#   LINUX_WS            workspace holding musl/ and sysroot/ (default linux-ws)
#   LINUX_KERNEL_WS     workspace holding the kernel source linux/, for its
#                       UAPI headers (default LINUX_WS)
#   MUSL_SHARED=1       also build libc.so, the dynamic linker
#                       (ld-musl-klausscpu.so.1 -> libc.so) and Scrt1.o:
#                       -fPIC objects, musl's configure link flags and the
#                       Linux-triple (hidden, PIC) compiler-rt builtins.
#                       Needs a toolchain with dynamic linking
#                       (klausscpu-llvm dynlink, build-dynlink) and a
#                       workspace of its own (LINUX_WS): the default static
#                       build is unchanged.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
KWS=${LINUX_KERNEL_WS:-$WS}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
SHARED=${MUSL_SHARED:-0}
# The shared build has a workspace of its own: its libc.a, libc.so and
# sysroot must not mix with the default static one.
if [ "$SHARED" = 1 ] && [ "$(realpath -m "$WS")" = "$(realpath -m "$RT/linux-ws")" ]; then
  echo "musl: MUSL_SHARED=1 needs LINUX_WS set to a workspace of its own"; exit 1
fi
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
# The patches own the musl tree: when the series changes, on the first
# build, or when the tree has lost them, every tracked file goes back to $TAG
# (local edits are lost; the port's symlinks and config.mak are untracked and
# stay) and the whole series is applied again.  The stamp keeps an unchanged
# series from touching the files (and so from rebuilding what includes them).
PATCHES=("$HERE"/patches/*.patch)
STAMP=$SRC/.klausscpu-patches
want=$(cat "${PATCHES[@]}" | sha256sum | cut -d' ' -f1)
if [ "$(cat "$STAMP" 2>/dev/null)" != "$want" ] ||
   ! git -C "$SRC" apply --reverse --check "${PATCHES[@]}" 2>/dev/null; then
  git -C "$SRC" checkout -q "$TAG" -- .
  git -C "$SRC" apply --check "${PATCHES[@]}"
  git -C "$SRC" apply "${PATCHES[@]}"
  echo "$want" >"$STAMP"
fi
LDFLAGS_AUTO=
if [ "$SHARED" = 1 ]; then
  # The Linux triple's builtins (-fPIC, hidden): the bare-metal archive has
  # absolute jump tables and exported symbols, so it cannot go into libc.so.
  BUILTINS=$("$B/clang" --target=klausscpu-unknown-linux-musl -rtlib=compiler-rt -print-libgcc-file-name)
  [ -f "$BUILTINS" ] || { echo "musl: no Linux builtins at $BUILTINS (build-builtins.sh)"; exit 1; }
  # musl's configure flags for libc.so (tryldflag; lld takes them all).
  LDFLAGS_AUTO="-Wl,--sort-section,alignment -Wl,--sort-common -Wl,--gc-sections -Wl,--hash-style=both -Wl,--no-undefined -Wl,--exclude-libs=ALL -Wl,--dynamic-list=./dynamic.list"
  # The driver must link -shared for the Linux triple (klausscpu-llvm
  # dynlink f97f22d; older drivers made every link -static).
  "$B/clang" --target=klausscpu-unknown-linux-musl -nostdlib -shared -x c /dev/null -### 2>&1 |
    grep -q '"-shared"' ||
    { echo "musl: $B/clang does not link -shared for klausscpu-unknown-linux-musl"; exit 1; }
else
  BUILTINS=$("$B/clang" --target=klausscpu-unknown-elf -rtlib=compiler-rt -print-libgcc-file-name)
fi
{
cat <<MAK
ARCH = klausscpu
SUBARCH =
ASMSUBARCH =
srcdir = .
prefix = $SYSROOT
exec_prefix = \$(prefix)
bindir = \$(exec_prefix)/bin
libdir = \$(prefix)/lib
includedir = \$(prefix)/include
MAK
if [ "$SHARED" = 1 ]; then
  # Where install-libs puts the dynamic linker's link: in the sysroot, never
  # the host's /lib (the link is made relative below).
  echo "syslibdir = $SYSROOT/lib"
else
  echo "syslibdir = /lib"
fi
cat <<MAK
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
LDFLAGS_AUTO =${LDFLAGS_AUTO:+ $LDFLAGS_AUTO}
CROSS_COMPILE =
LIBCC = $BUILTINS
OPTIMIZE_GLOBS = internal/*.c malloc/*.c string/*.c
ALL_TOOLS =
TOOL_LIBS =
ADD_CFI = no
MALLOC_DIR = mallocng
WRAPCC_GCC = \$(CC)
MAK
# libc.a from non-PIC objects in both modes (configure's choice when the
# compiler does not default to PIC; until 2026-11-11 the static build used
# the -fPIC ones, which the Linux PIC code generation would now send through
# the GOT), libc.so from -fPIC ones.
if [ "$SHARED" != 1 ]; then
  echo "SHARED_LIBS ="
fi
} >"$SRC/config.mak.new"
# Nothing in musl's Makefile depends on config.mak or on <link.h>, so a
# changed configuration or patch series rebuilds everything.
cfg=$( { cat "$SRC/config.mak.new"; echo "$want"; } | sha256sum | cut -d' ' -f1)
if [ "$(cat "$SRC/.klausscpu-config" 2>/dev/null)" != "$cfg" ]; then
  rm -rf "$SRC/obj" "$SRC/lib"
  echo "$cfg" >"$SRC/.klausscpu-config"
fi
mv "$SRC/config.mak.new" "$SRC/config.mak"
make -C "$SRC" -j"${JOBS:-$(nproc)}" >"$WS/musl-build.log" 2>&1 || { tail -30 "$WS/musl-build.log"; exit 1; }
make -C "$SRC" install-headers install-libs >>"$WS/musl-build.log" 2>&1
if [ "$SHARED" = 1 ]; then
  ln -sfn libc.so "$SYSROOT/lib/ld-musl-klausscpu.so.1"
fi
# The kernel's UAPI headers (linux/*.h, asm/*.h) beside musl's: busybox
# needs them (linux/vt.h, ...).
LINUX_WS=$KWS LINUX_BUILD=$WS/build-headers KLAUSSCPU_LLVM_BIN=$B "$RT/linux/build.sh" \
  headers_install INSTALL_HDR_PATH="$SYSROOT" >>"$WS/musl-build.log" 2>&1
echo "musl: sysroot $SYSROOT"
