#!/usr/bin/env bash
# linux/userland/curl.sh — static curl with mbedTLS for the KlaussCPU Linux
# port, against linux/musl's sysroot, into linux-ws/curl-build/curl; also
# fetches Mozilla's CA bundle (as curl.se publishes it) for the system
# image's /etc/ssl/certs/ca-certificates.crt.  build-rootfs.sh installs both.
#
# TLS: mbedTLS 3.6 LTS (curl 8.15 dropped BearSSL), built with its plain
# Makefile into linux-ws/mbedtls-build.  curl: HTTP(S), FTP and FILE; no
# zlib/brotli/zstd/nghttp2/idn/psl; the synchronous resolver (no threads
# needed in mbedTLS).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
CV=8.23.0
CSHA=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5
MV=3.6.7
MSHA=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
CA=cacert-2026-11-05.pem
CASHA=427c064216761a1e72a8b30f0d954d08cb6e4435a82091fb865afd20c8497285
MSRC=$WS/mbedtls
MOUT=$WS/mbedtls-build
SRC=$WS/curl
OUT=$WS/curl-build
[ -f "$WS/sysroot/lib/libc.a" ] || { echo "no musl sysroot: run linux/musl/build.sh" >&2; exit 1; }

fetch() { # <url> <sha256> -> the local file
  local f=$WS/$(basename "$1")
  [ -f "$f" ] || curl -fsSL --retry 5 -o "$f" "$1"
  echo "$2  $f" | sha256sum -c --quiet || { echo "$f: bad SHA-256" >&2; rm -f "$f"; exit 1; }
  echo "$f"
}
CCT=("$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot")

# mbedTLS
if [ ! -f "$MSRC/library/ssl_tls.c" ]; then
  f=$(fetch "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MV/mbedtls-$MV.tar.bz2" "$MSHA")
  rm -rf "$MSRC"; mkdir -p "$MSRC"
  tar -xjf "$f" -C "$MSRC" --strip-components=1
fi
rm -rf "$MOUT"; mkdir -p "$MOUT/lib" "$MOUT/include"
make -C "$MSRC/library" -j"${JOBS:-$(nproc)}" CC="${CCT[*]}" AR="$B/llvm-ar" \
  CFLAGS="-O2 -ffunction-sections -fdata-sections" libmbedcrypto.a libmbedx509.a libmbedtls.a \
  >"$MOUT/build.log" 2>&1 || { tail -30 "$MOUT/build.log"; exit 1; }
cp "$MSRC"/library/libmbed*.a "$MOUT/lib/"
cp -r "$MSRC/include/mbedtls" "$MSRC/include/psa" "$MOUT/include/"
make -C "$MSRC/library" clean >/dev/null 2>&1 || true
# mbedTLS's own known-answer tests (AES, GCM, SHA-2, RSA, ECP, ...): the
# crypto under curl's https, checked on the target.
"${CCT[@]}" -static -O2 -I"$MOUT/include" "$MSRC/programs/test/selftest.c" \
  -L"$MOUT/lib" -lmbedtls -lmbedx509 -lmbedcrypto -o "$MOUT/mbedtls-selftest"

# curl
if [ ! -f "$SRC/configure" ]; then
  f=$(fetch "https://curl.se/download/curl-$CV.tar.xz" "$CSHA")
  rm -rf "$SRC"; mkdir -p "$SRC"
  tar -xJf "$f" -C "$SRC" --strip-components=1
fi
rm -rf "$OUT"; mkdir -p "$OUT"
cd "$OUT"
# config.sub doesn't know klausscpu: a 64-bit Linux triplet it knows only
# puts configure in cross mode (every test compiles with the clang below).
"$SRC/configure" --host=riscv64-unknown-linux-musl --build="$(uname -m)-pc-linux-gnu" \
  CC="${CCT[*]}" AR="$B/llvm-ar" RANLIB="$B/llvm-ranlib" \
  CFLAGS="-O2 -ffunction-sections -fdata-sections" CPPFLAGS="-I$MOUT/include" \
  LDFLAGS="-static -L$MOUT/lib -Wl,--gc-sections" \
  --disable-shared --enable-static --with-mbedtls="$MOUT" \
  --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --without-ca-path \
  --without-zlib --without-brotli --without-zstd --without-nghttp2 --without-libidn2 \
  --without-libpsl --without-librtmp --without-libssh2 --disable-threaded-resolver \
  --disable-ldap --disable-ldaps --disable-rtsp --disable-dict --disable-telnet \
  --disable-tftp --disable-pop3 --disable-imap --disable-smtp --disable-gopher \
  --disable-mqtt --disable-smb --disable-manual --disable-docs --disable-ipfs \
  >"$OUT/configure.log" 2>&1 || { tail -30 "$OUT/configure.log"; exit 1; }
# curl --version names the configure host: say what it really is.
sed -i 's/^#define CURL_OS .*/#define CURL_OS "klausscpu-unknown-linux-musl"/' lib/curl_config.h
make -j"${JOBS:-$(nproc)}" -C lib >"$OUT/build.log" 2>&1 &&
  make -j"${JOBS:-$(nproc)}" -C src curl >>"$OUT/build.log" 2>&1 || { tail -30 "$OUT/build.log"; exit 1; }
cp src/curl "$OUT/curl"

fetch "https://curl.se/ca/$CA" "$CASHA" >/dev/null
cp "$WS/$CA" "$OUT/ca-certificates.crt"
echo "curl: $OUT/curl (curl $CV, mbedTLS $MV), CA bundle $CA"
