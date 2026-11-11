#!/usr/bin/env bash
# linux/userland/tests/dynlink/board.sh — DYNLINK_PLAN M6: run.sh's guest
# tests on the board, over ssh.
#
# The board's root is the read-only system image (no /lib, no tar, no
# chroot), so this copies the same layout run.sh puts in the initramfs
# (DT_OUT/extra.list: /lib/ld-musl-klausscpu.so.1, /lib/libdt*.so, /opt,
# /bin/dt-*) into /tmp/dtroot with scp, adds the board's busybox, a static
# musltest and xchroot (xchroot.c), mounts /proc and /dev there, runs every
# test command in that chroot and checks the output like run.sh.  Run
# run.sh first (it builds the programs).  Removes /tmp/dtroot afterwards.
#
#   BOARD_SSH   command that runs a command on the board (default: ssh
#               root@$BOARD_IP; it must work without a prompt)
#   BOARD_SCP   scp equivalent for copying a directory (default: scp -r)
#   BOARD_IP    default 192.168.68.59
#   DT_OUT      run.sh's output directory (default linux-ws/dynlink-m5)
#   DT_RUNS     start-up timing runs per program (default 500)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../../.." && pwd)
SRC=$(cd "$RT/.." && pwd)
B=${KLAUSSCPU_LLVM_BIN:-$SRC/klausscpu-llvm/build/bin}
OUT=${DT_OUT:-$RT/linux-ws/dynlink-m5}
IP=${BOARD_IP:-192.168.68.59}
read -r -a SSH <<<"${BOARD_SSH:-ssh root@$IP}"
read -r -a SCP <<<"${BOARD_SCP:-scp -r}"
RUNS=${DT_RUNS:-500}
[ -f "$OUT/extra.list" ] || { echo "no $OUT/extra.list: run run.sh first" >&2; exit 2; }

STAGE=$OUT/board-root
rm -rf "$STAGE"; mkdir -p "$STAGE/bin" "$STAGE/proc" "$STAGE/dev" "$STAGE/tmp"
awk -v R="$STAGE" '$1=="dir"{print "mkdir -p \"" R $2 "\""}
	$1=="file"{print "install -m " $4 " \"" $3 "\" \"" R $2 "\""}
	$1=="slink"{print "ln -sfn \"" $3 "\" \"" R $2 "\""}' "$OUT/extra.list" | sh
"$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$RT/linux-ws/sysroot" -static -O2 \
	-o "$STAGE/bin/xchroot" "$HERE/xchroot.c"
"$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$RT/linux-ws/sysroot" -static -O2 \
	-o "$STAGE/bin/musltest" "$RT/linux/userland/musltest.c" -lpthread

D=/tmp/dtroot
"${SSH[@]}" "umount $D/proc $D/dev 2>/dev/null; rm -rf $D"
"${SCP[@]}" "$STAGE" "root@$IP:$D"
"${SSH[@]}" "chmod -R a+rx $D && mount -t proc proc $D/proc && mount -o bind /dev $D/dev &&
	cp /bin/busybox $D/bin/busybox && ln -sf busybox $D/bin/sh"
CMDS=""
for m in core tls dlopen misc preload ldlp badelf; do CMDS="$CMDS|/bin/dt-main $m"; done
CMDS="$CMDS|/lib/ld-musl-klausscpu.so.1 /bin/dt-main core-direct|/lib/ld-musl-klausscpu.so.1 --list /bin/dt-main|/bin/musltest"
CMDS="$CMDS|/bin/dt-start $RUNS /bin/dt-true-static /bin/dt-true-dyn /bin/dt-true-dt1"
O=$OUT/board.out
# stderr (the loader's expected error messages) goes to its own log, shown
# after the checks: interleaved with stdout it can split a PASS line.
"${SSH[@]}" "L='${CMDS#|}'; IFS='|'; for c in \$L; do echo \"=== \$c\"; IFS=' '; $D/bin/xchroot $D \$c 2>>$D/stderr.log; echo \"rc=\$?\"; IFS='|'; done; echo DT-ALL-DONE; echo '--- stderr'; cat $D/stderr.log" >"$O" 2>&1 || true
"${SSH[@]}" "umount $D/proc $D/dev; rm -rf $D"

FAILS=$(grep -c '^FAIL' "$O" || true)
PASSES=$(grep -c '^PASS' "$O" || true)
grep -q '^DT-ALL-DONE' "$O" || FAILS=$((FAILS + 1))
grep -E '^TIME' "$O" || true
echo "board: $PASSES PASS, $FAILS FAIL (output: $O)"
[ "$FAILS" = 0 ]
