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
#
# Then programs are linked on the board from its -O2 objects (static, as the
# driver links by default), the links compared with the cross lld's links
# of the host objects (also without .comment), and run on the board, each
# checked for its own success line: libc++'s tests (eh, stdlib), musltest,
# strtest, zlib's example and a bzip2 round trip.
#   NC_SKIP     an extended regex of corpus names to leave out (e.g.
#               'cxx-stdlib': its -O2 compile takes over an hour on the board)
#   NC_CORPUS   corpus names (default: see CORPUS below)
#   NC_OPTS     optimisation levels (default "-O2 -O0")
#   NC_OUT      work directory (default linux-ws/native-clang)
#   BOARD_CLANG the board's clang (default /opt/llvm/bin/clang)
#   BOARD_DIR   board work directory (default /data/nc-test)
#   BOARD_SSH, BOARD_SCP, BOARD_IP, BOARD_SSH_KEY as in ../libffi/board.sh
# Take the board first (the board protocol): this script does not lock.
set -euo pipefail
{	# read whole before it runs: the script may be edited meanwhile
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
for f in "$BR"/libzlib-*/{adler32,compress,crc32,deflate,gzclose,gzlib,gzread,gzwrite,infback,inffast,inflate,inftrees,trees,uncompr,zutil}.c; do
  [ -f "$f" ] && add "zlib-$(basename "$f" .c)" $T "$f" "-DHAVE_UNISTD_H -I$(dirname "$f")"
done
for f in "$BR"/libzlib-*/test/example.c; do
  [ -f "$f" ] && add zlib-example $T "$f" "-DHAVE_UNISTD_H -I$(dirname "$f")/.."
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
  if [ -n "${NC_SKIP:-}" ] && grep -qE "^(${NC_SKIP})\$" <<<"$name"; then echo "SKIP $name (NC_SKIP)"; continue; fi
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

# ---- links: "name|driver|-O2 objects|run command|success pattern" ----
: >links
if grep -q -- '-O2 ' jobs; then
  have() { for o in "$@"; do [ -f "host/$o" ] || return 1; done; }
  lk() { local objs=$3; have $objs && echo "$1|$2|$objs|$4|$5" >>links || true; }
  lk eh clang++ "cxx-eh-O2.o cxx-eh_lib-O2.o" "./eh" "^DONE eh failures=0"
  lk stdlib clang++ "cxx-stdlib-O2.o" "./stdlib" "^DONE stdlib failures=0"
  lk musltest clang "userland-musltest-O2.o" "./musltest" "^musltest: PASS"
  lk strtest clang "userland-strtest-O2.o" "./strtest" "^strtest: PASS"
  lk zlib-example clang "$(cd host && ls zlib-*-O2.o | tr '\n' ' ')" "./zlib-example /tmp/nc-zlib.gz" "inflate with dictionary"
  lk bzip2 clang "$(cd host && ls bzip2-*-O2.o | tr '\n' ' ')" \
    "./bzip2 -c <$BDIR/i/bzip2-compress.i >t.bz2 && ./bzip2 -dc <t.bz2 | cmp - $BDIR/i/bzip2-compress.i && echo BZIP2-ROUNDTRIP-OK" \
    "^BZIP2-ROUNDTRIP-OK"
fi
mkdir -p host/link
while IFS='|' read -r name drv objs _; do
  (cd host && "$B/$drv" --target=$T --sysroot="$SR" -o "link/$name" $objs) ||
    { echo "FAIL host-link $name"; fail=$((fail + 1)); }
done <links

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
# Link with the board's own lld (its default sysroot), then run.
"${SCP[@]}" -q links "root@$IP:$BDIR/o/"
"${SSH[@]}" "mkdir -p $BDIR/o/link && cd $BDIR/o && while IFS='|' read -r name drv objs run pat; do
  case \$drv in clang++) d=$BOARD_CLANG++ ;; *) d=$BOARD_CLANG ;; esac
  s=\$(date +%s)
  \$d --target=$T -o link/\$name \$objs 2>link/\$name.err && rm -f link/\$name.err
  echo \"link-\$name \$((\$(date +%s) - s))\" >>times
  [ -x link/\$name ] && (cd link && sh -c \"\$run\" >\$name.out 2>&1; echo \"exit=\$?\" >>\$name.out)
done <links"
"${SCP[@]}" -q -r "root@$IP:$BDIR/o/*" board/
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
while IFS='|' read -r name _ _ _ pat; do
  if [ -f "board/link/$name.err" ] || [ ! -f "board/link/$name" ]; then
    echo "FAIL link-$name: board link failed: $(head -3 "board/link/$name.err" 2>/dev/null | tr '\n' ' ')"; fail=$((fail + 1)); continue
  fi
  if "$B/llvm-objcopy" -R .comment "host/link/$name" cmp-h.o && "$B/llvm-objcopy" -R .comment "board/link/$name" cmp-b.o &&
     cmp -s cmp-h.o cmp-b.o; then
    same=$((same + 1))
  else
    echo "FAIL link-$name: links differ ($(cmp cmp-h.o cmp-b.o | head -1))"; fail=$((fail + 1))
  fi
  if grep -qE "$pat" "board/link/$name.out" && grep -q '^exit=0' "board/link/$name.out"; then
    echo "PASS run-$name"
  else
    echo "FAIL run-$name: $(tail -3 "board/link/$name.out" | tr '\n' ' ')"; fail=$((fail + 1))
  fi
done <links
rm -f cmp-h.o cmp-b.o
for w in host board; do
  f=$(ls $w/*.o 2>/dev/null | head -1)
  [ -n "$f" ] && echo "INFO $w: $("$B/llvm-readelf" -p .comment "$f" | grep -o 'clang version.*')"
done
sort -k2 -n -r board/times | head -5 | sed 's/^/INFO slowest: /'
echo "RESULT: $same identical (objects and links), $fail failing"
[ "$fail" = 0 ]
exit
}
