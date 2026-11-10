#!/usr/bin/env bash
# linux/userland/fio.sh — static fio (the flexible I/O tester) for the
# KlaussCPU Linux port, against linux/musl's sysroot, into
# linux-ws/fio-build/fio; build-rootfs.sh puts it in the system image.
# Buildroot's fio needs shared libraries, so it is built here.  fio has its
# own configure: --cpu is unknown to it, so it uses its generic arch model
# (arch/arch-generic.h: compiler barriers only, which is all a single CPU
# needs).  musl's fcntl.h lacks FALLOC_FL_ZERO_RANGE, which the kernel's
# linux/falloc.h has.  No external I/O engines (libaio, rdma, ...).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
V=3.41
SHA=a40b45112005e30a67bab6be4860b198c35104a628b6ec4d15720d62b5d1bab9
SRC=$WS/fio
OUT=$WS/fio-build
[ -f "$WS/sysroot/lib/libc.a" ] || { echo "no musl sysroot: run linux/musl/build.sh" >&2; exit 1; }
if [ ! -f "$SRC/configure" ]; then
  tgz=$WS/fio-$V.tar.gz
  [ -f "$tgz" ] || curl -fsSL --retry 5 -o "$tgz" "https://brick.kernel.dk/snaps/fio-$V.tar.gz"
  echo "$SHA  $tgz" | sha256sum -c --quiet || { echo "$tgz: bad SHA-256" >&2; rm -f "$tgz"; exit 1; }
  rm -rf "$SRC"; mkdir -p "$SRC"
  tar -xzf "$tgz" -C "$SRC" --strip-components=1
fi
# fio builds in its source tree: build a copy.
rm -rf "$OUT"; cp -a "$SRC" "$OUT"
cd "$OUT"
./configure --cc="$B/clang --target=klausscpu-unknown-linux-musl --sysroot=$WS/sysroot" \
  --cpu=klausscpu --build-static --disable-native \
  --extra-cflags="-O2 -include linux/falloc.h -Wno-macro-redefined" \
  --disable-numa --disable-rdma --disable-rados --disable-rbd --disable-http --disable-gfapi \
  --disable-pmem --disable-xnvme --disable-isal --disable-libblkio --disable-tcmalloc \
  --disable-libzbc --disable-dfs --disable-libnfs \
  >"$OUT/configure.log" 2>&1 || { tail -20 "$OUT/configure.log"; exit 1; }
make -j"${JOBS:-$(nproc)}" fio AR="$B/llvm-ar" RANLIB="$B/llvm-ranlib" >"$OUT/build.log" 2>&1 ||
  { grep -m5 "error" "$OUT/build.log"; exit 1; }
"$B/llvm-strip" fio
echo "fio: $OUT/fio (fio $V)"
