#!/usr/bin/env bash
# linux/userland/lmbench.sh — a focused subset of lmbench 3 for the
# KlaussCPU Linux port (static, musl), into linux-ws/lmbench-build/:
#   lat_syscall lat_proc lat_ctx lat_pagefault bw_mem lat_mem_rd tlb hello
# plus tlbmiss (linux/userland/tlbmiss.c, the TLB-miss cost at 64 KiB
# strides), perfstat (perfstat.c, the board's performance counters around
# a command) and sysloop (sysloop.c, kernel-entry microbenchmarks).
# build-rootfs.sh puts the lmbench ones in /usr/libexec/lmbench and the
# others in /usr/bin; tests/bench.sh runs them.
#
# lmbench's own build (scripts/build, configure-like) is not used.  Its
# bench.h includes <rpc/rpc.h> for the RPC benchmarks' prototypes only;
# musl has no Sun RPC, so a two-type stub stands in, and lib_tcp/lib_udp
# (portmapper calls) are left out: none of these benchmarks uses them.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
REV=27b43aeec9dc0bb45f420388c78e42dde4c23bf0	# github.com/intel/lmbench (lmbench3)
SRC=$WS/lmbench
OUT=$WS/lmbench-build
[ -f "$WS/sysroot/lib/libc.a" ] || { echo "no musl sysroot: run linux/musl/build.sh" >&2; exit 1; }
if [ "$(git -C "$SRC" rev-parse HEAD 2>/dev/null)" != "$REV" ]; then
  rm -rf "$SRC"; mkdir -p "$SRC"
  git -C "$SRC" init -q
  git -C "$SRC" fetch -q --depth 1 https://github.com/intel/lmbench.git "$REV"
  git -C "$SRC" checkout -q FETCH_HEAD
fi
rm -rf "$OUT"; mkdir -p "$OUT/obj" "$OUT/compat/rpc"
cat >"$OUT/compat/rpc/rpc.h" <<'EOF'
/* Stub for lmbench's bench.h: no Sun RPC benchmarks are built. */
#include <sys/types.h>
typedef struct lm_svcxprt SVCXPRT;
typedef struct lm_client CLIENT;
EOF
echo '/* Stub for lmbench. */' >"$OUT/compat/rpc/types.h"
CC=("$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$WS/sysroot")
CFLAGS=(-O2 -D_GNU_SOURCE -DHAVE_socklen_t -DHAVE_DRAND48 -DHAVE_SCHED_SETAFFINITY
        -I"$OUT/compat" -Wno-deprecated-non-prototype -Wno-implicit-int)
LIB="lib_timing lib_mem lib_stats lib_debug lib_sched lib_unix getopt"
BENCH="lat_syscall lat_proc lat_ctx lat_pagefault bw_mem lat_mem_rd tlb hello"
for f in $LIB $BENCH; do
  "${CC[@]}" "${CFLAGS[@]}" -c "$SRC/src/$f.c" -o "$OUT/obj/$f.o" &
done
wait
"$B/llvm-ar" rcs "$OUT/obj/liblm.a" $(for f in $LIB; do echo "$OUT/obj/$f.o"; done)
for b in $BENCH; do
  "${CC[@]}" -static -o "$OUT/$b" "$OUT/obj/$b.o" "$OUT/obj/liblm.a" -lm
done
"${CC[@]}" -static -O2 -o "$OUT/tlbmiss" "$HERE/tlbmiss.c"
"${CC[@]}" -static -O2 -o "$OUT/perfstat" "$HERE/perfstat.c"
"${CC[@]}" -static -O2 -o "$OUT/sysloop" "$HERE/sysloop.c"
"${CC[@]}" -static -O2 -o "$OUT/strtest" "$HERE/strtest.c"
echo "lmbench: $OUT ($BENCH tlbmiss perfstat sysloop strtest; lmbench $REV)"
