#!/usr/bin/env bash
# linux/userland/tests/native-clang/perf.sh — how fast the board's clang
# compiles (CLANG_ON_BOARD_PLAN.md M6).  A few sources, preprocessed here
# (so header search is not measured, except in the "-hdr" runs, which
# compile hello.cpp from source with the board's headers), compiled on the
# board under perfstat (the hardware counters, user/sys time, peak RSS) by
# each clang given, at each optimisation level.
#   NP_CLANGS   board clangs to compare (default /opt/llvm/bin/clang)
#   NP_OPTS     optimisation levels (default "-O0 -O1 -O2")
#   NP_FILES    corpus names, a subset of: hello zlib-deflate sqlite-part
#               cxx-hello (default all)
#   NP_OUT      work directory (default linux-ws/native-perf)
#   BOARD_*     as in board.sh.  Take the board first: this script does not lock.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../../.." && pwd)
SRC=$(cd "$RT/.." && pwd)
B=${KLAUSSCPU_LLVM_BIN:-$SRC/klausscpu-llvm/build/bin}
SR=${KLAUSSCPU_SYSROOT:-$RT/linux-ws/dyn/sysroot}
BR=${BR_OUT:-$RT/linux-ws/br-output-dyn}/build
OUT=${NP_OUT:-$RT/linux-ws/native-perf}
CLANGS=${NP_CLANGS:-/opt/llvm/bin/clang}
OPTS=${NP_OPTS:--O0 -O1 -O2}
FILES=${NP_FILES:-hello zlib-deflate cxx-hello}
IP=${BOARD_IP:-192.168.68.59}
KEY=${BOARD_SSH_KEY:-$HOME/.ssh/klausscpu_board_ed25519}
KO="-o UserKnownHostsFile=$HOME/.cache/klausscpu-board-known_hosts -o StrictHostKeyChecking=accept-new"
[ -f "$KEY" ] && KO="$KO -i $KEY -o IdentitiesOnly=yes"
read -r -a SSH <<<"${BOARD_SSH:-ssh $KO -o BatchMode=yes -o ConnectTimeout=20 root@$IP}"
read -r -a SCP <<<"${BOARD_SCP:-scp $KO -o BatchMode=yes}"
T=klausscpu-unknown-linux-musl
BD=/data/np-test

rm -rf "$OUT"; mkdir -p "$OUT/f"
cd "$OUT"
"$B/clang" --target=$T --sysroot="$RT/linux-ws/sysroot" -static -O2 "$RT/linux/userland/perfstat.c" -o f/perfstat
printf '#include <stdio.h>\nint main(void) { printf("hello\\n"); return 0; }\n' >hello.c
for name in $FILES; do
  case $name in
  hello) "$B/clang" --target=$T --sysroot="$SR" -E hello.c -o f/hello.i ;;
  zlib-deflate) d=$(ls -d "$BR"/libzlib-*/ | head -1)
    "$B/clang" --target=$T --sysroot="$SR" -DHAVE_UNISTD_H -I"$d" -E "$d/deflate.c" -o f/zlib-deflate.i ;;
  cxx-hello) cp "$HOME/.cache/clang-board/m2/hello.cpp" f/cxx-hello.cpp 2>/dev/null ||
      printf '#include <cstdio>\n#include <string>\n#include <vector>\nint main(){std::vector<std::string> v{"a","b"};std::printf("%%zu\\n",v.size());}\n' >f/cxx-hello.cpp
    "$B/clang++" --target=$T --sysroot="$SR" -E f/cxx-hello.cpp -o f/cxx-hello.ii ;;
  esac
done
ls -la f
"${SSH[@]}" "rm -rf $BD && mkdir -p $BD"
"${SCP[@]}" -q f/* "root@$IP:$BD/"
{
  for c in $CLANGS; do
    for name in $FILES; do
      for o in $OPTS; do
        case $name in cxx-hello) file=$name.ii drv=$c++ ;; *) file=$name.i drv=$c ;; esac
        echo "== $c $name $o"
        "${SSH[@]}" "cd $BD && s=\$(date +%s) && ./perfstat $drv --target=$T $o -c $file -o out.o 2>&1; echo \"wall \$((\$(date +%s) - s)) s\""
      done
    done
  done
} 2>&1 | tee perf.txt
"${SSH[@]}" "rm -rf $BD"
