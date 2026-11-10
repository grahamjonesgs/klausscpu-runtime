#!/bin/sh
# iperf3-test.sh: iperf3's version, then a TCP and a UDP run over loopback
# against one-shot servers.  Ends with "iperf3-test: PASS" when both
# clients finish.  Installed as /usr/share/klausscpu/tests/iperf3-test.sh.
# On the board, from the PC: "iperf3 -s" on one side and
# "iperf3 -c <other> [-R] [-u -b 50M]" on the other.
iperf3 --version | head -1
ok=0
iperf3 -s -1 -D -p 5201 --logfile /tmp/iperf3-tcp.log
iperf3 -s -1 -D -p 5202 --logfile /tmp/iperf3-udp.log
sleep 1
iperf3 -c 127.0.0.1 -p 5201 -t 2 -f m >/tmp/iperf3-c.log 2>&1 && grep -q 'iperf Done' /tmp/iperf3-c.log && ok=$((ok + 1))
grep -E 'sender|receiver' /tmp/iperf3-c.log
iperf3 -c 127.0.0.1 -p 5202 -u -b 10M -t 2 -f m >/tmp/iperf3-u.log 2>&1 && grep -q 'iperf Done' /tmp/iperf3-u.log && ok=$((ok + 1))
grep -E 'sender|receiver' /tmp/iperf3-u.log
[ $ok = 2 ] && echo "iperf3-test: PASS" || { echo "iperf3-test: FAIL"; tail -5 /tmp/iperf3-c.log /tmp/iperf3-u.log; }
