#!/usr/bin/env bash
# board-check.sh [--boot] [--quick] [OUTDIR]: the board's Linux checked from
# this PC in one go, under both board locks (the board protocol: announce
# "board: taking it" to the other sessions first).  Run it after any
# bitstream, kernel or system-image change.
#   system    the kernel log (CPU_FEATURES, TLB entries, no Oops/BUG/panic/
#             WARNING), the system image's stamp, /data mounted read-write
#   net       ping, the httpd page, an SSH login on 22 (OpenSSH) and on 2222
#             (Dropbear), both presenting the QSPI keystore's host key, and
#             an 8 MiB scp round trip
#   packages  /usr/share/klausscpu/tests/buildroot-test.sh (every Buildroot
#             package)
#   python    the standard modules with C parts import, ctypes calls libc
#   dynlink   linux/userland/tests/dynlink/board.sh (run its run.sh first)
#   libffi    linux/userland/tests/libffi/board.sh (run its run.sh first)
# --boot      first load the kernel over the UART (KERNEL, default
#             linux-ws/build-board/vmlinux, else build-perf) and wait for SSH: a fresh boot,
#             and the console is kept in OUTDIR/console.log
# --quick     skip dynlink and libffi (about a minute instead of ten)
# OUTDIR      default linux-ws/board-check/<date-time>
#   BOARD_SSH  a command that runs a command on the board without a prompt
#              (default: ssh -o BatchMode=yes root@$BOARD_IP, i.e. a key);
#              BOARD_SCP likewise for scp
#   BOARD_IP   default 192.168.68.59
#   BOARD_HOSTKEY_FP  the expected host key fingerprint (default Zephyr's
#              QSPI key, LINUX_PLAN.md "SSH servers")
# Prints PASS/FAIL per check and a summary; exits 1 if anything failed.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
BOOT=0 QUICK=0
while [ $# -gt 0 ]; do
	case $1 in
	--boot) BOOT=1 ;;
	--quick) QUICK=1 ;;
	-*) echo "usage: board-check.sh [--boot] [--quick] [OUTDIR]" >&2; exit 2 ;;
	*) break ;;
	esac
	shift
done
OUT=$(realpath -m "${1:-$RT/linux-ws/board-check/$(date +%Y%m%d-%H%M%S)}")
mkdir -p "$OUT"
# Both locks for the whole run (re-executed under them once).
L1=/run/user/$(id -u)/klausscpu-board.lock L2=$HOME/.cache/klausscpu-board.lock
if [ -z "${BOARD_CHECK_LOCKED:-}" ]; then
	BOARD_CHECK_LOCKED=1 exec flock "$L1" flock "$L2" "$0" $([ $BOOT = 1 ] && echo --boot) \
		$([ $QUICK = 1 ] && echo --quick) "$OUT"
fi
IP=${BOARD_IP:-192.168.68.59}
KEY=${BOARD_SSH_KEY:-$HOME/.ssh/klausscpu_board_ed25519}
# The board's host key is pinned in its own known_hosts on first contact.
KO="-o UserKnownHostsFile=$HOME/.cache/klausscpu-board-known_hosts -o StrictHostKeyChecking=accept-new"
[ -f "$KEY" ] && KO="$KO -i $KEY -o IdentitiesOnly=yes"
read -r -a SSH <<<"${BOARD_SSH:-ssh $KO -o BatchMode=yes -o ConnectTimeout=20 root@$IP}"
read -r -a SCP <<<"${BOARD_SCP:-scp $KO -o BatchMode=yes}"
export BOARD_SSH="${SSH[*]}" BOARD_SCP="${SCP[*]}" BOARD_IP=$IP
FP=${BOARD_HOSTKEY_FP:-SHA256:DYRxGyaKM2ZNhrjasuYI3VHyznQ7oWfC9LxA2KwsIYI}
FAILS=0 PASSES=0
pass() { echo "PASS $*"; PASSES=$((PASSES + 1)); }
fail() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }
check() { local name=$1; shift; if "$@"; then pass "$name"; else fail "$name"; fi; }
rsh() { "${SSH[@]}" "$@"; }

if [ $BOOT = 1 ]; then
	# The board kernel: build-board (overlayfs, the overlay sdinit), else build-perf.
	K=${KERNEL:-$RT/linux-ws/build-board/vmlinux}
	[ -f "$K" ] || K=$RT/linux-ws/build-perf/vmlinux
	KLAUSSCC=${KLAUSSCC:-$RT/../klausscc/target/release/klausscc}
	echo "booting $K"
	"$KLAUSSCC" -i "$K" -s -m </dev/null >"$OUT/console.log" 2>&1 &
	CONSOLE=$!
	trap 'kill $CONSOLE 2>/dev/null' EXIT
	sleep 5
	if timeout 900 bash -c "until grep -aq 'rcS: httpd' '$OUT/console.log' && timeout 5 bash -c '</dev/tcp/$IP/22' 2>/dev/null; do sleep 5; done"; then
		pass "boot: rcS reached httpd and SSH answers"
	else
		fail "boot: no SSH within 15 min (see $OUT/console.log)"
		echo "board-check: $PASSES PASS, $FAILS FAIL"; exit 1
	fi
fi

echo "--- system"
rsh 'cat /etc/klausscpu-rootfs; uname -a; dmesg' >"$OUT/system.txt" 2>&1
check "ssh login on 22 runs commands" grep -q '^klausscpu-rootfs ' "$OUT/system.txt"
grep -a -E 'CPU_FEATURES|TLB entries' "$OUT/system.txt" | head -2
check "kernel log: no Oops, BUG, panic or WARNING" \
	bash -c "! grep -a -E 'Oops|BUG:|Kernel panic|WARNING:|Call trace' '$OUT/system.txt'"
check "/data is mounted read-write" rsh 'grep -q " /data ext4 rw" /proc/mounts'

echo "--- net"
check "ping" bash -c "ping -c 3 -q -W 2 $IP >/dev/null"
check "httpd page" bash -c "curl -s -m 10 http://$IP/ | grep -q 'Linux on KlaussCPU'"
for p in 22 2222; do
	check "ssh login on port $p" rsh -p $p true
	got=$(ssh-keyscan -T 10 -t ecdsa -p $p "$IP" 2>/dev/null | ssh-keygen -lf - 2>/dev/null | awk '{print $2}')
	check "port $p host key is the keystore's ($FP)" test "$got" = "$FP"
done
head -c $((8 << 20)) /dev/urandom >"$OUT/r8"
check "8 MiB scp round trip" bash -c "${SCP[*]} '$OUT/r8' root@$IP:/tmp/r8 && ${SCP[*]} root@$IP:/tmp/r8 '$OUT/r8.back' && cmp -s '$OUT/r8' '$OUT/r8.back'"
rsh 'rm -f /tmp/r8'; rm -f "$OUT/r8" "$OUT/r8.back"

echo "--- packages"
rsh 'sh /usr/share/klausscpu/tests/buildroot-test.sh' >"$OUT/buildroot-test.txt" 2>&1
tail -1 "$OUT/buildroot-test.txt"
check "buildroot-test" bash -c "tail -1 '$OUT/buildroot-test.txt' | grep -Eq 'buildroot-test: ([0-9]+)/\\1 passed'"

echo "--- python"
check "python: C modules import, ctypes calls libc" rsh 'python3 -c "
import ssl, sqlite3, hashlib, zlib, bz2, lzma, readline, curses, ctypes, json, socket, threading, subprocess
assert ctypes.CDLL(None).strlen(b\"klauss\") == 6
print(\"python ok\", ssl.OPENSSL_VERSION)"'

if [ $QUICK = 0 ]; then
	echo "--- dynlink"
	if BOARD_SCP="${SCP[*]} -r" "$RT/linux/userland/tests/dynlink/board.sh" >"$OUT/dynlink.txt" 2>&1; then
		pass "dynlink: $(tail -1 "$OUT/dynlink.txt")"
	else
		fail "dynlink: $(tail -1 "$OUT/dynlink.txt") (see $OUT/dynlink.txt)"
	fi
	echo "--- libffi"
	if "$RT/linux/userland/tests/libffi/board.sh" >"$OUT/libffi.txt" 2>&1; then
		pass "libffi: $(grep -c '^PASS' "$OUT/libffi.txt") board checks"
	else
		fail "libffi (see $OUT/libffi.txt)"
	fi
	grep -E '^(PASS|FAIL)' "$OUT/libffi.txt" | sed 's/^/  /'
fi
rsh 'dmesg' >"$OUT/dmesg-after.txt" 2>&1
check "kernel log after the tests: no Oops, BUG, panic or WARNING" \
	bash -c "! grep -a -E 'Oops|BUG:|Kernel panic|WARNING:|Call trace' '$OUT/dmesg-after.txt'"

echo "board-check: $PASSES PASS, $FAILS FAIL (output: $OUT)"
[ "$FAILS" = 0 ]
