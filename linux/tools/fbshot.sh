#!/usr/bin/env bash
# linux/tools/fbshot.sh [user@host] [out.png] — a screenshot of the board's
# /dev/fb0 over SSH: `fbtest shot` on the board (the visible page, and the
# palette in 8 bpp modes), converted here by fb2png.py.  Also writes
# <out>-screen.png, the 640x480 picture as the VGA port shows it (4 bits per
# channel, 320-pixel modes doubled and centred).
#   BOARD  default root@192.168.68.59
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
HOST=${1:-${BOARD:-root@192.168.68.59}}
OUT=${2:-fb.png}
SHOT=${OUT%.png}.klfb
ssh "$HOST" fbtest shot >"$SHOT"
head -1 "$SHOT"
python3 -I "$HERE/fb2png.py" "$SHOT" "$OUT"
python3 -I "$HERE/fb2png.py" --screen "$SHOT" "${OUT%.png}-screen.png"
