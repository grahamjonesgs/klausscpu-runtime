#!/usr/bin/env bash
# linux/userland/tests/cxx/board.sh: run.sh's C++ test programs on the board.
# The programs come from run.sh's output (CT_OUT, default linux-ws/cxx-test:
# run run.sh first); they go to /tmp/cxxt on the board with the C++ shared
# libraries (libc++, libc++abi, libunwind from the sysroot; the system image
# has none yet), found through LD_LIBRARY_PATH, and libffi's ffirun runs
# them.  check.sh checks the output here, as for the emulator.
#   BOARD_SSH, BOARD_SCP, BOARD_IP, BOARD_SSH_KEY as in ../libffi/board.sh
# Take the board first (the board protocol): this script does not lock.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../../.." && pwd)
OUT=${CT_OUT:-$RT/linux-ws/cxx-test}
DYN_SYSROOT=${DYN_SYSROOT:-$RT/linux-ws/dyn/sysroot}
IP=${BOARD_IP:-192.168.68.59}
KEY=${BOARD_SSH_KEY:-$HOME/.ssh/klausscpu_board_ed25519}
KO="-o UserKnownHostsFile=$HOME/.cache/klausscpu-board-known_hosts -o StrictHostKeyChecking=accept-new"
[ -f "$KEY" ] && KO="$KO -i $KEY -o IdentitiesOnly=yes"
read -r -a SSH <<<"${BOARD_SSH:-ssh $KO -o BatchMode=yes -o ConnectTimeout=20 root@$IP}"
read -r -a SCP <<<"${BOARD_SCP:-scp $KO -o BatchMode=yes}"
for f in "$OUT/list" "$OUT/ffirun" "$OUT/bin"; do
	[ -e "$f" ] || { echo "missing $f: run run.sh first" >&2; exit 2; }
done
ST=$OUT/board
rm -rf "$ST"; mkdir -p "$ST/cxxt/bin" "$ST/cxxt/lib"
cd "$OUT"
for f in bin/*; do
	case $f in *.log) ;; *.so) cp "$f" "$ST/cxxt/lib/" ;; *) cp "$f" "$ST/cxxt/bin/" ;; esac
done
for l in libc++.so.1 libc++abi.so.1 libunwind.so.1; do cp -L "$DYN_SYSROOT/lib/$l" "$ST/cxxt/lib/"; done
# The emulator's paths (/test/bin) become /tmp/cxxt/bin; each program gets
# LD_LIBRARY_PATH (ffirun's "name VAR=value path args" form).
sed 's| /test/bin/| LD_LIBRARY_PATH=/tmp/cxxt/lib /tmp/cxxt/bin/|' list >"$ST/cxxt/list"
cp ffirun "$ST/cxxt/"
"${SSH[@]}" 'rm -rf /tmp/cxxt'
"${SCP[@]}" -r "$ST/cxxt" "root@$IP:/tmp/"
start=$(date +%s)
"${SSH[@]}" 'cd /tmp/cxxt && chmod 755 ffirun bin/* && ./ffirun list >out 2>err; echo "ffirun rc=$?"; echo CT-ALL-DONE >>out'
echo "board run: $(( $(date +%s) - start )) s"
"${SCP[@]}" "root@$IP:/tmp/cxxt/out" "$ST/board.out"
"${SSH[@]}" 'rm -rf /tmp/cxxt'
"$HERE/check.sh" "$ST/board.out" list
