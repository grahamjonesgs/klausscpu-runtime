#!/bin/sh
# SSH_ASKPASS helper for sshrun.sh: the ssh_shell app's password.
echo "${KLAUSSCPU_SSH_PASS:-klausscpu}"
