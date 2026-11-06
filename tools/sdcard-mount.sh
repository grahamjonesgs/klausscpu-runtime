#!/usr/bin/env bash
# tools/sdcard-mount.sh [-u] [mountpoint]
#
# Mount the board's SD card over SFTP (default ~/sdcard), so an editor can
# open it as a folder.  ssh_shell must be running on the board.  -u unmounts.
# Needs sshfs (apt install sshfs).
#
#   KLAUSSCPU_BOARD_IP, KLAUSSCPU_SSH_USER, KLAUSSCPU_SSH_PASS
#                       as validate/zephyr/sshrun.sh
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
unmount=0
if [ "${1:-}" = -u ]; then unmount=1; shift; fi
MNT=${1:-$HOME/sdcard}

if [ $unmount = 1 ]; then
  fusermount3 -u "$MNT"
  exit
fi
command -v sshfs >/dev/null || { echo "sshfs not found: sudo apt install sshfs" >&2; exit 1; }
if mountpoint -q "$MNT"; then echo "$MNT: already mounted"; exit; fi
mkdir -p "$MNT"
# The owner the board reports is root; the files have to look like ours.
SSH_ASKPASS=$HERE/validate/zephyr/askpass.sh SSH_ASKPASS_REQUIRE=force DISPLAY=${DISPLAY:-:0} \
  sshfs "${KLAUSSCPU_SSH_USER:-admin}@${KLAUSSCPU_BOARD_IP:-192.168.68.59}:/SD:" "$MNT" \
  -o "uid=$(id -u),gid=$(id -g),reconnect,ServerAliveInterval=15" \
  -o StrictHostKeyChecking=no,UserKnownHostsFile=/dev/null,PubkeyAuthentication=no
echo "SD card mounted at $MNT"
