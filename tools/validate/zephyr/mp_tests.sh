#!/usr/bin/env bash
# zephyr/mp_tests.sh <outdir> [run-tests.py args...]
#
# MicroPython's own test suite (tests/run-tests.py) against the board's
# `python` command.  ssh_shell must be running on the board.
#
#  1. Puts micropython-lib's unittest (pinned by the MicroPython checkout's
#     lib/micropython-lib) on the SD card as /lib/unittest, if missing.
#  2. Runs the tests through ssh_python.py under flock $BOARD_LOCK; results
#     go to <outdir>/results, the log to <outdir>/mp_tests.log.
#  3. Compares the failures with mp_tests-known-fail.txt (each with its
#     reason).  Exits 1 on a failure that isn't listed, and reports listed
#     tests that now pass.
#
# With extra arguments (e.g. -d basics, -i REGEX) they are passed to
# run-tests.py instead of the default test set.
#
#   KLAUSSCPU_BOARD_IP, KLAUSSCPU_SSH_USER, KLAUSSCPU_SSH_PASS  as sshrun.sh
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../common.sh"
[ $# -ge 1 ] || die "usage: zephyr/mp_tests.sh <outdir> [run-tests.py args...]"
OUT=$(realpath -m "$1"); shift
MP=$RT/zephyr-ws/modules/lib/micropython
[ -f "$MP/tests/run-tests.py" ] || die "no MicroPython checkout at $MP (west update micropython)"
mkdir -p "$OUT"
IP=${KLAUSSCPU_BOARD_IP:-192.168.68.59}
SSHOPTS=(-o BatchMode=no -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
         -o PubkeyAuthentication=no -o LogLevel=ERROR)
export SSH_ASKPASS=$HERE/askpass.sh SSH_ASKPASS_REQUIRE=force DISPLAY=${DISPLAY:-:0}

# 1. unittest on the SD card.
LIBREV=$(git -C "$MP" ls-tree HEAD lib/micropython-lib | awk '{print $3}')
LIB=$OUT/micropython-lib
if [ ! -f "$LIB/python-stdlib/unittest/unittest/__init__.py" ]; then
  rm -rf "$LIB"
  git init -q "$LIB" &&
    git -C "$LIB" fetch -q --depth 1 https://github.com/micropython/micropython-lib "$LIBREV" &&
    git -C "$LIB" checkout -q FETCH_HEAD || die "could not fetch micropython-lib $LIBREV"
fi
printf -- '-mkdir lib\n-mkdir lib/unittest\nput %s lib/unittest/__init__.py\n' \
  "$LIB/python-stdlib/unittest/unittest/__init__.py" >"$OUT/sftp.batch"
flock "$BOARD_LOCK" timeout 60 sftp "${SSHOPTS[@]}" -b "$OUT/sftp.batch" "admin@$IP" \
  >"$OUT/sftp.log" 2>&1 || die "uploading unittest failed (see $OUT/sftp.log)"

# 2. The tests.  A run whose log stops growing for MP_TESTS_STALL seconds
# (default 600; one test times out after 10-30 s) is killed.
rm -rf "$OUT/results"
( cd "$MP/tests" &&
  exec flock "$BOARD_LOCK" timeout "${MP_TESTS_TIMEOUT:-10800}" python3 -u run-tests.py \
    -t exec:"python3 $HERE/ssh_python.py" -r "$OUT/results" "$@" ) >"$OUT/mp_tests.log" 2>&1 &
pid=$!
last=-1; still=0
while kill -0 $pid 2>/dev/null; do
  sleep 10
  size=$(stat -c %s "$OUT/mp_tests.log")
  if [ "$size" = "$last" ]; then still=$((still + 10)); else still=0; last=$size; fi
  if [ $still -ge "${MP_TESTS_STALL:-600}" ]; then
    echo "mp_tests.sh: no output for ${still}s, killing the run" >>"$OUT/mp_tests.log"
    pkill -TERM -P $pid 2>/dev/null; kill $pid 2>/dev/null
    pkill -TERM -f "ssh_python.py" 2>/dev/null
    break
  fi
done
wait $pid; rc=$?
grep -aE '^[0-9]+ tests (performed|passed)|^[0-9]+ tests? (failed|skipped)' "$OUT/mp_tests.log" | cut -c1-200
if ! grep -aqE '^[0-9]+ tests performed' "$OUT/mp_tests.log"; then
  echo "FAIL mp_tests: run-tests.py did not finish (status $rc; see $OUT/mp_tests.log)"
  grep -a 'ssh_python:' "$OUT/mp_tests.log"
  exit 1
fi
grep -a 'ssh_python:' "$OUT/mp_tests.log" && echo "NOTE mp_tests: the SSH session dropped (above)"

# The board must still answer a new session once the test session is gone.
# (sshrun.sh's status is ssh's: 255 when the board closes the connection at
# the end, as it always does, so only its output counts.)
sleep 3
flock "$BOARD_LOCK" timeout 60 "$HERE/sshrun.sh" 'kernel uptime' >"$OUT/after.txt" 2>&1
if grep -aq 'Uptime:' "$OUT/after.txt"; then
  echo "PASS mp_tests: ssh_shell answers after the run"
else
  echo "FAIL mp_tests: ssh_shell does not answer after the run (see $OUT/after.txt)"
  exit 1
fi

# 3. Against the known failures (only for the default test set).
fail=0
grep -aoE '^FAIL +[^ ]+' "$OUT/mp_tests.log" | awk '{print $2}' | sort >"$OUT/failed.txt"
if [ $# -eq 0 ]; then
  grep -vE '^\s*(#|$)' "$HERE/mp_tests-known-fail.txt" | awk '{print $1}' | sort >"$OUT/known.txt"
  while read -r t; do echo "FAIL mp_tests $t (not in mp_tests-known-fail.txt)"; fail=1; done \
    < <(comm -23 "$OUT/failed.txt" "$OUT/known.txt")
  while read -r t; do echo "NOTE mp_tests $t now passes: drop it from mp_tests-known-fail.txt"; done \
    < <(comm -13 "$OUT/failed.txt" "$OUT/known.txt")
else
  while read -r t; do echo "FAIL mp_tests $t"; done <"$OUT/failed.txt"
  [ -s "$OUT/failed.txt" ] && fail=1
fi
[ $fail = 0 ] && echo "PASS mp_tests (see $OUT/mp_tests.log)"
exit $fail
