#!/bin/bash
# fetch-doomgeneric.sh — clone the doomgeneric engine sources for the Doom app.
#
# doomgeneric is GPLv2 (id Software Doom source + a thin platform layer) and is
# cloned, not vendored, into doomgeneric-src/ (gitignored).  Only our Zephyr
# port (src/) and build files are tracked in this repo.  Run once from here:
#
#     ./fetch-doomgeneric.sh
#
# Then build the app per CLAUDE.md and put a Doom IWAD (e.g. the freely
# redistributable shareware doom1.wad) on the SD card at /SD:/doom1.wad.

set -euo pipefail
cd "$(dirname "$0")"

SRC=doomgeneric-src
if [ -d "$SRC" ]; then
    echo "==> $SRC already present, skipping clone"
else
    echo "==> Cloning doomgeneric..."
    git clone --depth 1 https://github.com/ozkl/doomgeneric.git "$SRC"
fi
echo "==> Done.  Engine sources in $SRC/doomgeneric/"
