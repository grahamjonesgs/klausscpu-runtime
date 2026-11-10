#!/usr/bin/env bash
# linux/userland/dropbear.sh — static Dropbear (SSH server and client, one
# multi-call binary) for the KlaussCPU Linux port, against linux/musl's
# sysroot.  configure is not run: dropbear/config.h is written for musl,
# and the sources are cross-compiled directly into
# linux-ws/dropbear-build/dropbearmulti, which the "busybox" initramfs
# profile picks up.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
SRC=$WS/dropbear
OUT=$WS/dropbear-build
TAG=DROPBEAR_2026.94
[ -d "$SRC/.git" ] || git clone -q --depth 1 --branch "$TAG" https://github.com/mkj/dropbear.git "$SRC"
[ -f "$WS/sysroot/lib/libc.a" ] || { echo "no musl sysroot: run linux/musl/build.sh" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/obj/ltc" "$OUT/obj/ltm" "$OUT/obj/db" "$OUT/inc"
cp "$HERE/dropbear/config.h" "$HERE/dropbear/localoptions.h" "$OUT/inc/"
# default_options_guard.h as Dropbear's Makefile makes it (ifndef_wrapper.sh):
# every "#define X Y" of default_options.h wrapped in #ifndef X.
sed 's/^\( *#define \([^ ][^ ]*\) .*\)/#ifndef \2\
\1\
#endif/' "$SRC/src/default_options.h" >"$OUT/inc/default_options_guard.h"
CC=("$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot")
CFLAGS=(-Os -ffunction-sections -fdata-sections -Wno-unused-command-line-argument
        -I"$OUT/inc" -I"$SRC/src" -I"$SRC/libtomcrypt/src/headers" -I"$SRC/libtommath"
        -DLOCALOPTIONS_H_EXISTS -DDROPBEAR_MULTI -DDROPBEAR_SERVER -DDROPBEAR_CLIENT
        -DDBMULTI_dropbear -DDBMULTI_dropbearkey -DDBMULTI_dbclient -DDBMULTI_scp
        -DPROGRESS_METER)
jobs=${JOBS:-$(nproc)}
compile() { # <out dir> <extra flags> <sources...>
  local out=$1 extra=$2; shift 2
  printf '%s\n' "$@" | xargs -P "$jobs" -I{} sh -c '
    o="$0/$(echo "{}" | tr / _ | sed "s/\.c\$/.o/")"
    "$@" -c "{}" -o "$o" || exit 255' "$out" "${CC[@]}" "${CFLAGS[@]}" $extra
}
cd "$SRC"
compile "$OUT/obj/ltm" "" libtommath/*.c
# The *tab.c files are tables that other sources #include.
compile "$OUT/obj/ltc" -DLTC_SOURCE $(find libtomcrypt/src -name '*.c' ! -name '*tab.c')
DB_SRCS="dbutil buffer dbhelpers dss bignum signkey rsa dbrandom queue atomicio compat
  fake-rfc2553 ltc_prng ecc ecdsa sk-ecdsa crypto_desc curve25519 ed25519 sk-ed25519
  dbmalloc dbctype gensignkey gendss genrsa gened25519
  svr-kex svr-auth sshpty svr-authpasswd svr-authpubkey svr-authpubkeyoptions
  svr-session svr-service svr-chansession svr-runopts svr-agentfwd svr-main
  svr-x11fwd svr-forward svr-tcpfwd svr-streamfwd svr-authpam
  cli-main cli-auth cli-authpasswd cli-kex cli-session cli-runopts cli-chansession
  cli-authpubkey cli-tcpfwd cli-channel cli-authinteract cli-agentfwd cli-readconf
  common-session packet common-algo common-kex common-channel common-chansession
  termcodes loginrec tcp-accept listener process-packet dh_groups common-runopts
  circbuffer list netio chachapoly gcm kex-x25519 kex-dh kex-ecdh kex-pqhybrid
  sntrup761 mlkem768
  dropbearkey scp progressmeter scpmisc dbmulti"
compile "$OUT/obj/db" "" $(for s in $DB_SRCS; do echo "src/$s.c"; done)
"${CC[@]}" -static -Wl,--gc-sections -o "$OUT/dropbearmulti" \
  "$OUT"/obj/db/*.o "$OUT"/obj/ltc/*.o "$OUT"/obj/ltm/*.o -lcrypt
echo "dropbear: $OUT/dropbearmulti"
