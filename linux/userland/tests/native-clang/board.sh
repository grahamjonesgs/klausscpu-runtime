#!/usr/bin/env bash
# linux/userland/tests/native-clang/board.sh — the board's clang must produce
# exactly what the cross clang produces (CLANG_ON_BOARD_PLAN.md M5).
#
# Each corpus source is preprocessed here with the cross clang (so the board
# needs no headers for this test), then the .i is compiled to an object by
# the cross clang and by the board's clang with the same driver flags, and
# the two objects must be byte-identical: any difference is a miscompile of
# clang itself (or a host dependency in it).  The .comment section (clang's
# version string, which names the commit each was built from) is left out of
# the comparison; the two strings are printed.
#   NC_CORPUS   corpus names (default: see CORPUS below)
#   NC_OPTS     optimisation levels (default "-O2 -O0")
#   NC_OUT      work directory (default linux-ws/native-clang)
#   BOARD_CLANG the board's clang (default /opt/llvm/bin/clang)
#   BOARD_DIR   board work directory (default /data/nc-test)
#   BOARD_SSH, BOARD_SCP, BOARD_IP, BOARD_SSH_KEY as in ../libffi/board.sh
# Take the board first (the board protocol): this script does not lock.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../../.." && pwd)
SRC=$(cd "$RT/.." && pwd)
B=${KLAUSSCPU_LLVM_BIN:-$SRC/klausscpu-llvm/build/bin}
SR=${KLAUSSCPU_SYSROOT:-$RT/linux-ws/dyn/sysroot}
BR=${BR_OUT:-$RT/linux-ws/br-output-dyn}/build
OUT=${NC_OUT:-$RT/linux-ws/native-clang}
OPTS=${NC_OPTS:--O2 -O0}
BOARD_CLANG=${BOARD_CLANG:-/opt/llvm/bin/clang}
BDIR=${BOARD_DIR:-/data/nc-test}
IP=${BOARD_IP:-192.168.68.59}
KEY=${BOARD_SSH_KEY:-$HOME/.ssh/klausscpu_board_ed25519}
KO="-o UserKnownHostsFile=$HOME/.cache/klausscpu-board-known_hosts -o StrictHostKeyChecking=accept-new"
[ -f "$KEY" ] && KO="$KO -i $KEY -o IdentitiesOnly=yes"
read -r -a SSH <<<"${BOARD_SSH:-ssh $KO -o BatchMode=yes -o ConnectTimeout=20 root@$IP}"
read -r -a SCP <<<"${BOARD_SCP:-scp $KO -o BatchMode=yes}"
T=klausscpu-unknown-linux-musl

rm -rf "$OUT"; mkdir -p "$OUT/i" "$OUT/host" "$OUT/board"
cd "$OUT"
# corpus entries: "name|triple|source|preprocessor flags|compile flags"
CORPUS=()
add() { CORPUS+=("$1|$2|$3|${4:-}|${5:-}"); }
PICO=$RT/picolibc-install
for f in "$BR"/libzlib-*/{adler32,compress,crc32,deflate,infback,inffast,inflate,inftrees,trees,uncompr,zutil}.c; do
  [ -f "$f" ] && add "zlib-$(basename "$f" .c)" $T "$f" "-DHAVE_UNISTD_H -I$(dirname "$f")"
done
for f in "$BR"/bzip2-*/{blocksort,huffman,crctable,randtable,compress,decompress,bzlib,bzip2}.c; do
  [ -f "$f" ] && add "bzip2-$(basename "$f" .c)" $T "$f" "-D_FILE_OFFSET_BITS=64 -I$(dirname "$f")"
done
for f in "$RT"/linux/userland/tests/dynlink/*.c "$RT"/linux/userland/tests/libffi/abi.c; do
  add "$(basename "$(dirname "$f")")-$(basename "$f" .c)" $T "$f" "-I$(dirname "$f")"
done
for f in "$RT"/linux/userland/*.c; do
  add "userland-$(basename "$f" .c)" $T "$f"
done
for f in "$RT"/baremetal/programs/{dhrystone,queens,bst,expr,adventure,crypto}.c; do
  [ -f "$f" ] && add "bare-$(basename "$f" .c)" klausscpu-unknown-elf "$f" \
    "-nostdlibinc -isystem $PICO/include -D__IEEE_LITTLE_ENDIAN -D_LDBL_EQ_DBL -I$RT -I$RT/baremetal -I$RT/src" \
    "-fPIC -ffunction-sections -fdata-sections"
done
for f in "$RT"/linux/userland/tests/cxx/{eh,eh_lib,stdlib}.cpp; do
  add "cxx-$(basename "$f" .cpp)" $T "$f" "-std=c++23 -I$(dirname "$f")" "-std=c++23"
done

# ---- preprocess and compile here ----
: >jobs
for e in "${CORPUS[@]}"; do
  IFS='|' read -r name triple src extra cc <<<"$e"
  case $src in *.cpp) drv=clang++ ext=ii ;; *) drv=clang ext=i ;; esac
  sr=; [ "$triple" = $T ] && sr=--sysroot=$SR
  if ! "$B/$drv" --target="$triple" $sr $extra -E "$src" -o "i/$name.$ext" 2>"i/$name.err"; then
    echo "SKIP $name (does not preprocess: $(head -1 "i/$name.err"))"; rm -f "i/$name.$ext"; continue
  fi
  rm -f "i/$name.err"
  for o in $OPTS; do
    echo "$name$o $triple $o $name.$ext $cc" >>jobs
  done
done
fail=0
while read -r id triple o file cc; do
  case $file in *.ii) drv=clang++ ;; *) drv=clang ;; esac
  # Compiled from i/, as on the board, so the file names match.
  (cd i && "$B/$drv" --target="$triple" "$o" $cc -c "$file" -o "../host/$id.o") ||
    { echo "FAIL host-compile $id"; fail=$((fail + 1)); }
done <jobs
echo "corpus: $(ls i | wc -l) sources, $(wc -l <jobs) compiles, $(du -sh i | cut -f1) preprocessed"

# ---- the board ----
"${SSH[@]}" "rm -rf $BDIR && mkdir -p $BDIR/i $BDIR/o"
"${SCP[@]}" -q i/* jobs "root@$IP:$BDIR/i/"
start=$(date +%s)
# One compile at a time, timed; clang++ is a symlink beside clang.
"${SSH[@]}" "cd $BDIR/i && while read -r id triple o file cc; do
  case \$file in *.ii) drv=\${BOARD_CLANG:-$BOARD_CLANG}++ ;; *) drv=$BOARD_CLANG ;; esac
  s=\$(date +%s)
  if \$drv --target=\$triple \$o \$cc -c \$file -o ../o/\$id.o 2>../o/\$id.err; then rm -f ../o/\$id.err; fi
  echo \"\$id \$((\$(date +%s) - s))\" >>../o/times
done <jobs"
echo "board: $(( $(date +%s) - start )) s for $(wc -l <jobs) compiles"
"${SCP[@]}" -q "root@$IP:$BDIR/o/*" board/
"${SSH[@]}" "rm -rf $BDIR"

# ---- compare ----
same=0
while read -r id _; do
  if [ -f "board/$id.err" ]; then
    echo "FAIL $id: board compile failed: $(head -3 "board/$id.err" | tr '\n' ' ')"; fail=$((fail + 1))
  elif [ ! -f "board/$id.o" ]; then
    echo "FAIL $id: no board object"; fail=$((fail + 1))
  elif "$B/llvm-objcopy" -R .comment "host/$id.o" cmp-h.o && "$B/llvm-objcopy" -R .comment "board/$id.o" cmp-b.o &&
       cmp -s cmp-h.o cmp-b.o; then
    same=$((same + 1))
  else
    echo "FAIL $id: objects differ ($(cmp "host/$id.o" "board/$id.o" | head -1))"; fail=$((fail + 1))
  fi
done <jobs
rm -f cmp-h.o cmp-b.o
for w in host board; do
  f=$(ls $w/*.o 2>/dev/null | head -1)
  [ -n "$f" ] && echo "INFO $w: $("$B/llvm-readelf" -p .comment "$f" | grep -o 'clang version.*')"
done
sort -k2 -n -r board/times | head -5 | sed 's/^/INFO slowest: /'
echo "RESULT: $same identical, $fail failing"
[ "$fail" = 0 ]
