#!/usr/bin/env bash
# linux/buildroot/fetch.sh — Buildroot (pinned LTS release) into
# linux-ws/buildroot, with linux/buildroot/patches/*.patch applied (what
# can't live in the BR2_EXTERNAL tree, linux/buildroot/external: the
# klausscpu architecture and the gnuconfig entry).  Run again after
# changing the patches: it starts from a fresh tree.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
WS=${LINUX_WS:-$RT/linux-ws}
V=2026.02.3
SHA=5a59e7501b0b4ec52c41f4bfa79412320e0b37eae5f719605a258e8d0c6fc7fb
SRC=$WS/buildroot
tgz=$WS/buildroot-$V.tar.xz
[ -f "$tgz" ] || curl -fsSL --retry 5 -o "$tgz" "https://buildroot.org/downloads/buildroot-$V.tar.xz"
echo "$SHA  $tgz" | sha256sum -c --quiet || { echo "$tgz: bad SHA-256" >&2; rm -f "$tgz"; exit 1; }
rm -rf "$SRC"; mkdir -p "$SRC"
tar -xJf "$tgz" -C "$SRC" --strip-components=1
for p in "$HERE"/patches/*.patch; do
  [ -e "$p" ] || continue
  patch -d "$SRC" -p1 -s <"$p" || { echo "patch failed: $p" >&2; exit 1; }
done
echo "buildroot: $SRC (Buildroot $V$(ls "$HERE"/patches/*.patch >/dev/null 2>&1 && echo ", $(ls "$HERE"/patches/*.patch | wc -l) patches"))"
