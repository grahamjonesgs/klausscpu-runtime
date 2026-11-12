#!/usr/bin/env bash
# linux/userland/tests/libffi/board.sh: run.sh's libffi testsuite programs
# and ctypes_smoke.py on the board, against the system image's own
# libffi.so.8 and Python (the shared system image, /usr/lib).  The
# programs come from run.sh's output (FT_OUT, default linux-ws/libffi-test:
# run run.sh testsuite first); ffirun runs them on the board from /tmp/ffit
# and check.py checks the output here, as in the emulator.  This is the
# test the emulator can't do: closures written and run at run time against
# the real I-cache.
#   BOARD_SSH  a command that runs a command on the board without a prompt
#              (default: ssh root@$BOARD_IP); BOARD_SCP likewise for scp
#   BOARD_IP   default 192.168.68.59
# Take the board first (the board protocol): this script does not lock.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../../.." && pwd)
OUT=${FT_OUT:-$RT/linux-ws/libffi-test}
BR_OUT=${BR_OUT:-$RT/linux-ws/br-output-dyn}
TS=${FFI_SRC:-$BR_OUT/build/libffi-3.4.8}/testsuite
IP=${BOARD_IP:-192.168.68.59}
KEY=${BOARD_SSH_KEY:-$HOME/.ssh/klausscpu_board_ed25519}
# The board's host key is pinned in its own known_hosts on first contact.
KO="-o UserKnownHostsFile=$HOME/.cache/klausscpu-board-known_hosts -o StrictHostKeyChecking=accept-new"
[ -f "$KEY" ] && KO="$KO -i $KEY -o IdentitiesOnly=yes"
read -r -a SSH <<<"${BOARD_SSH:-ssh $KO -o BatchMode=yes -o ConnectTimeout=20 root@$IP}"
read -r -a SCP <<<"${BOARD_SCP:-scp $KO -o BatchMode=yes}"
for f in "$OUT/build.jobs" "$OUT/ffirun" "$OUT/unsupported.txt" "$TS"; do
	[ -e "$f" ] || { echo "missing $f: run run.sh testsuite first" >&2; exit 2; }
done
ST=$OUT/board
rm -rf "$ST"; mkdir -p "$ST/ffit/bin"
cd "$OUT"
while read -r name src rest; do
	exe=bin/${name//\//__}
	[ -x "$exe" ] && { echo "$name /tmp/ffit/bin/$(basename "$exe")"; cp "$exe" "$ST/ffit/bin/"; }
done <build.jobs >"$ST/ffit/list"
cp ffirun "$HERE/ctypes_smoke.py" "$ST/ffit/"
"${SSH[@]}" 'rm -rf /tmp/ffit'
"${SCP[@]}" -r "$ST/ffit" "root@$IP:/tmp/"
"${SSH[@]}" 'cd /tmp/ffit && chmod 755 ffirun bin/* && ./ffirun list >out 2>err; echo "ffirun rc=$?"'
"${SCP[@]}" "root@$IP:/tmp/ffit/out" "$ST/testsuite.out"
FAILS=0
python3 -I "$HERE/check.py" "$ST/testsuite.out" build.jobs "$TS" "$HERE" bin unsupported.txt >"$ST/check.txt" || true
grep -v '^PASS ' "$ST/check.txt" || true
summary=$(grep '^SUMMARY' "$ST/check.txt" || echo "SUMMARY none")
np=$(sed -n 's/.* pass=\([0-9]*\).*/\1/p' <<<"$summary")
nf=$(sed -n 's/.* fail=\([0-9]*\).*/\1/p' <<<"$summary")
nj=$(wc -l <build.jobs)
if [ -n "$np" ] && [ "$np" -gt 0 ] && [ "$nf" = 0 ] && [ $((np + nf)) = "$nj" ]; then
	echo "PASS libffi testsuite on the board: $summary"
else
	echo "FAIL libffi testsuite on the board: $summary (jobs $nj)"; FAILS=$((FAILS + 1))
fi
"${SSH[@]}" 'cd /tmp/ffit && PYTHONHASHSEED=0 python3 -s ctypes_smoke.py' >"$ST/ctypes.out" 2>&1 || true
"${SSH[@]}" 'rm -rf /tmp/ffit'
if grep -q '^ALL-CTYPES-DONE failures=0' "$ST/ctypes.out"; then
	echo "PASS ctypes_smoke.py on the board ($(grep -c '^PASS' "$ST/ctypes.out") checks)"
else
	echo "FAIL ctypes_smoke.py on the board (see $ST/ctypes.out)"; FAILS=$((FAILS + 1))
fi
[ "$FAILS" = 0 ]
