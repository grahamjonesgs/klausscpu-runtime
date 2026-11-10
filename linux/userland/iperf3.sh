#!/usr/bin/env bash
# linux/userland/iperf3.sh — static iperf3 (client and server) for the
# KlaussCPU Linux port, against linux/musl's sysroot, into
# linux-ws/iperf3-build/iperf3; build-rootfs.sh puts it in the system image.
# Cross-configured (autoconf) out of tree: no OpenSSL (iperf3's optional
# authentication), no SCTP.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
V=3.22
SHA=1c0d0fb02c52626111d6e132db80edfbf27bbaff8bd9245df2a371dcb0b35a92
SRC=$WS/iperf3
OUT=$WS/iperf3-build
[ -f "$WS/sysroot/lib/libc.a" ] || { echo "no musl sysroot: run linux/musl/build.sh" >&2; exit 1; }
if [ ! -f "$SRC/configure" ]; then
  tgz=$WS/iperf-$V.tar.gz
  [ -f "$tgz" ] || curl -fsSL --retry 5 -o "$tgz" "https://downloads.es.net/pub/iperf/iperf-$V.tar.gz"
  echo "$SHA  $tgz" | sha256sum -c --quiet || { echo "$tgz: bad SHA-256" >&2; rm -f "$tgz"; exit 1; }
  rm -rf "$SRC"; mkdir -p "$SRC"
  tar -xzf "$tgz" -C "$SRC" --strip-components=1
fi
rm -rf "$OUT"; mkdir -p "$OUT"
cd "$OUT"
# config.sub doesn't know klausscpu.  --host only puts configure in cross
# mode (every test compiles with the clang below, and shared libraries are
# off), so a 64-bit Linux triplet it does know stands in.
"$SRC/configure" --host=riscv64-unknown-linux-musl --build="$(uname -m)-pc-linux-gnu" \
  CC="$B/clang --target=klausscpu-unknown-linux-musl --sysroot=$WS/sysroot" \
  AR="$B/llvm-ar" RANLIB="$B/llvm-ranlib" CFLAGS="-O2" LDFLAGS="-static" \
  --disable-shared --enable-static --enable-static-bin --without-openssl --without-sctp \
  >"$OUT/configure.log" 2>&1 || { tail -20 "$OUT/configure.log"; exit 1; }
make -C src -j"${JOBS:-$(nproc)}" iperf3 >"$OUT/build.log" 2>&1 || { tail -30 "$OUT/build.log"; exit 1; }
cp src/iperf3 "$OUT/iperf3"
echo "iperf3: $OUT/iperf3 (iperf $V)"
