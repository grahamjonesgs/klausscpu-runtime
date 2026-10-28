#!/usr/bin/env bash
# zephyr/sshrun.sh "cmd1" "cmd2" ...
#
# Log in to the ssh_shell app on the board and type each command, giving each
# $DELAY seconds (default 3), then `exit`.  Prints the session with the
# terminal escape sequences removed.
#
#   KLAUSSCPU_BOARD_IP  board address (default 192.168.68.59)
#   KLAUSSCPU_SSH_USER  user (default admin)
#   KLAUSSCPU_SSH_PASS  password (default: the ssh_server.c default)
#   TMO                 overall timeout in seconds (default 120)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
D=${DELAY:-3}
SSH_ASKPASS=$HERE/askpass.sh SSH_ASKPASS_REQUIRE=force DISPLAY=${DISPLAY:-:0} \
  timeout "${TMO:-120}" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o PubkeyAuthentication=no -o LogLevel=ERROR -tt \
    "${KLAUSSCPU_SSH_USER:-admin}@${KLAUSSCPU_BOARD_IP:-192.168.68.59}" \
    < <(sleep 2; for c in "$@"; do printf '%s\r' "$c"; sleep "$D"; done; printf 'exit\r'; sleep 1) 2>&1 |
  sed 's/\x1b\[[0-9;]*[A-Za-z]//g'
