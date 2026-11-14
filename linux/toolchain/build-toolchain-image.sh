#!/usr/bin/env bash
# linux/toolchain/build-toolchain-image.sh [OUT.img [SIZE_MiB]] — the board's
# toolchain image (CLANG_ON_BOARD_PLAN.md M4): a read-only ext4 image that
# rcS loop-mounts at /opt/llvm from /linux/toolchain.img on the SD card
# (install-toolchain puts it there).
#
#   /opt/llvm/bin/llvm   the static multicall binary (build-native-llvm.sh
#                        with NATIVE_MULTICALL=1), and as symlinks to it
#                        clang, clang++, cc, c++, ld.lld, lld, llvm-ar,
#                        llvm-ranlib, llvm-nm, llvm-objdump, llvm-readelf,
#                        llvm-readobj
#   /opt/llvm/lib/clang/<ver>/include   clang's resource headers
#   /opt/llvm/lib/clang/<ver>/lib/klausscpu-unknown-linux-musl  the Linux
#                        compiler-rt builtins
#   /opt/llvm/klausscpu-linux-musl   the sysroot: musl's headers, crt objects,
#                        libc.so and the archives, the kernel UAPI headers,
#                        libc++/libc++abi/libunwind headers and libraries (the
#                        shared musl sysroot).  The driver takes
#                        <bindir>/../klausscpu-linux-musl as the sysroot when
#                        none is given, so no configuration file is needed.
#
# Programs built on the board run against the system image's /lib/libc.so,
# which must be the sysroot's: the script prints the sysroot's hash.
#   NATIVE_BUILD        default klausscpu-llvm/build-native-klausscpu-multi-o2 (the
#                       Release build: 24% fewer instructions, ~17% faster on the
#                       board than MinSizeRel, 90 MB instead of 64 MB)
#   KLAUSSCPU_LLVM_BIN  host toolchain (default ../klausscpu-llvm/build/bin):
#                       the builtins come from its resource directory
#   KLAUSSCPU_SYSROOT   default linux-ws/dyn/sysroot
#   TC_STAGE            staging directory (default linux-ws/toolchain-stage)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
B=${KLAUSSCPU_LLVM_BIN:-$(cd "$RT/../klausscpu-llvm/build/bin" && pwd)}
NB=${NATIVE_BUILD:-$(cd "$B/../.." && pwd)/build-native-klausscpu-multi-o2}
SR=${KLAUSSCPU_SYSROOT:-$RT/linux-ws/dyn/sysroot}
ST=${TC_STAGE:-$RT/linux-ws/toolchain-stage}
OUTIMG=${1:-$RT/linux-ws/toolchain.img}
SIZE=${2:-}
TRIPLE=klausscpu-unknown-linux-musl

for f in "$NB/bin/llvm" "$SR/lib/libc.so" "$SR/lib/libc++.a" "$SR/include/c++/v1/vector"; do
  [ -e "$f" ] || { echo "toolchain: missing $f" >&2; exit 1; }
done
VER=$("$B/clang" -dumpversion | cut -d. -f1)
RES=$("$B/clang" --target=$TRIPLE -print-resource-dir 2>/dev/null)
BUILTINS=$("$B/clang" --target=$TRIPLE -rtlib=compiler-rt -print-libgcc-file-name 2>/dev/null)
[ -f "$BUILTINS" ] || { echo "toolchain: no Linux builtins at $BUILTINS" >&2; exit 1; }

rm -rf "$ST"
mkdir -p "$ST/bin" "$ST/lib/clang/$VER/lib/$TRIPLE" "$ST/klausscpu-linux-musl"
"$B/llvm-strip" --strip-all "$NB/bin/llvm" -o "$ST/bin/llvm"
for t in clang clang++ cc c++ ld.lld lld llvm-ar llvm-ranlib llvm-nm llvm-objdump llvm-readelf llvm-readobj; do
  ln -s llvm "$ST/bin/$t"
done

# clang's resource directory: headers and the Linux builtins.
cp -r "$RES/include" "$ST/lib/clang/$VER/include"
cp "$BUILTINS" "$ST/lib/clang/$VER/lib/$TRIPLE/"

# The sysroot: the shared musl sysroot whole (headers, crt objects,
# libc.so/libc.a and the other musl archives, the C++ runtime).
cp -a "$SR/include" "$SR/lib" "$ST/klausscpu-linux-musl/"

du -sh "$ST/bin" "$ST/klausscpu-linux-musl" "$ST/lib" "$ST" | sed 's/^/toolchain: /'
used=$(du -sm "$ST" | cut -f1)
SIZE=${SIZE:-$(( (used * 115 / 100) + 8 ))}
tar --owner=0 --group=0 --numeric-owner -C "$ST" -cf "$ST.tar" .
rm -f "$OUTIMG"
mke2fs -q -t ext4 -O ^has_journal -L klausscpu-llvm -m 0 -d "$ST.tar" "$OUTIMG" "${SIZE}M"
e2fsck -fn "$OUTIMG" >/dev/null
rm -f "$ST.tar"
echo "toolchain: $OUTIMG (${SIZE} MiB, ${used} MiB used, sha256 $(sha256sum "$OUTIMG" | cut -d' ' -f1))"
# Buildroot strips the system image's copy (--strip-all).
"$B/llvm-strip" --strip-all "$SR/lib/libc.so" -o "$ST.libc.so"
echo "toolchain: sysroot libc.so stripped sha256 $(sha256sum "$ST.libc.so" | cut -d' ' -f1) (must match the board's /lib/libc.so)"
rm -f "$ST.libc.so"
