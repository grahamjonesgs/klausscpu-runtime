#!/usr/bin/env bash
# linux/fetch.sh — set up the Linux port workspace (linux-ws/, gitignored):
# a shallow clone of the pinned stable kernel, the port's arch/klausscpu
# symlinked in, and linux/patches/*.patch applied in order.  Safe to rerun:
# an existing checkout is reset to the pinned tag and re-patched.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
TAG=v7.3.3
SRC=$WS/linux
mkdir -p "$WS"
if [ ! -d "$SRC/.git" ]; then
  git clone --depth 1 --branch "$TAG" \
    https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git "$SRC"
fi
cd "$SRC"
git -c advice.detachedHead=false checkout -q -f "$TAG"
git clean -qfd -e arch/klausscpu
rm -f arch/klausscpu
ln -s "$HERE/arch/klausscpu" arch/klausscpu
for p in "$HERE"/patches/*.patch; do
  [ -e "$p" ] || continue
  git apply "$p"
  echo "applied $(basename "$p")"
done
echo "linux-ws ready: $SRC ($TAG)"
