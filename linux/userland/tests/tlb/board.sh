#!/usr/bin/env bash
# linux/userland/tests/tlb/board.sh: TLB refill cost and the workloads'
# refill share on the board (LINUX_PLAN.md "TLB size and refill").
#   - dchase PAGES STEPS 0: data-side pointer chase, one word per 64 KiB
#     page; cycles per refill = delta cycles / delta refills between a run
#     inside the TLB's reach and one beyond it.
#   - ichase_N: N code stubs, one per 64 KiB page (gen.py N), the same for
#     the instruction side.
#   - dchase2 PAGES STEPS 1: dchase with a 128 KiB D-cache thrash before
#     every step (the handler's data cold).
#   - wl.sh: the workloads under perfstat, 3 runs each.
# Uses the board's perfstat and sysloop (/usr/bin), Python 3 and sqlite3
# (the shared system image).  Output in $TLB_OUT (default linux-ws/tlbmeas).
#   BOARD_SSH  a command that runs a command on the board without a prompt
#              (default: ssh root@$BOARD_IP); BOARD_SCP likewise for scp
#   BOARD_IP   default 192.168.68.59
# Take the board first (the board protocol): this script does not lock.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../../.." && pwd)
B=${KLAUSSCPU_LLVM_BIN:-$RT/../klausscpu-llvm/build/bin}
IP=${BOARD_IP:-192.168.68.59}
KEY=${BOARD_SSH_KEY:-$HOME/.ssh/klausscpu_board_ed25519}
# The board's host key is pinned in its own known_hosts on first contact.
KO="-o UserKnownHostsFile=$HOME/.cache/klausscpu-board-known_hosts -o StrictHostKeyChecking=accept-new"
[ -f "$KEY" ] && KO="$KO -i $KEY -o IdentitiesOnly=yes"
read -r -a SSH <<<"${BOARD_SSH:-ssh $KO -o BatchMode=yes -o ConnectTimeout=20 root@$IP}"
read -r -a SCP <<<"${BOARD_SCP:-scp $KO -o BatchMode=yes}"
O=${TLB_OUT:-$RT/linux-ws/tlbmeas}
mkdir -p "$O"; cd "$O"
CC=("$B/clang" --target=klausscpu-unknown-linux-musl --sysroot="$RT/linux-ws/sysroot" -static -O2)
"${CC[@]}" -o dchase "$HERE/dchase.c"
"${CC[@]}" -o dchase2 "$HERE/dchase2.c"
for n in 8 32 64; do
  python3 "$HERE/gen.py" $n >ichase_$n.S
  "${CC[@]}" -o ichase_$n "$HERE/ichase.c" ichase_$n.S
done
"${SCP[@]}" dchase dchase2 ichase_8 ichase_32 ichase_64 "$HERE/wl.sh" "$HERE/loop.py" "root@$IP:/tmp/"
"${SSH[@]}" 'cd /tmp; chmod +x dchase dchase2 ichase_* wl.sh; dmesg | grep "TLB entries"
for p in 8 16 32 48 64; do ./dchase $p 1000000 0 | tail -1; done
for n in 8 32 64; do ./ichase_$n | tail -1; done
for r in 1 2 3; do for n in 4 32 64; do ./dchase2 $n 4000 1 | tail -1; done; done' | tee micro.txt
"${SSH[@]}" 'cd /tmp; T=/usr/share/klausscpu/tests
./wl.sh py_pass python3 -S -c pass
./wl.sh py_loop python3 -S /tmp/loop.py
./wl.sh sys_fork sysloop fork 200
./wl.sh sys_exec sysloop exec 200
./wl.sh sys_fault sysloop fault 2000
for i in 1 2 3; do rm -f /tmp/s.db; perfstat sh -c "sqlite3 /tmp/s.db < $T/sqlite-test.sql >/dev/null 2>&1" 2>&1 | awk -v n=sqlite "\$1==\"cycles\"{c=\$2} \$1==\"instructions\"{i=\$2} \$1==\"tlb_refills\"{t=\$2} END{printf \"%s cycles %d instr %d refills %d\n\",n,c,i,t}"; done
./wl.sh lua lua $T/lua-selftest.lua
rm -f /tmp/s.db dchase dchase2 ichase_* wl.sh loop.py' | tee workloads.txt
