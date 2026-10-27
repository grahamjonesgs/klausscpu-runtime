#!/bin/bash
# get-benchmarks.sh — fetch the upstream CoreMark and Embench-IoT sources into
# baremetal/benchmarks/{coremark,embench-iot}/ at pinned commits.
# Run once from anywhere before `make coremark` / `make embench` in baremetal/.
#
# Neither source tree is vendored into this repo (CoreMark is under its own
# Apache-2.0-style licence; Embench-IoT is GPL-3.0 with a licence per
# benchmark).  Both clones are gitignored; only the KlaussCPU port files
# (coremark-port/, embench-port/) live here.

set -euo pipefail
cd "$(dirname "$0")"

# name  url  ref(tag)  commit
fetch() {
    local dest=$1 url=$2 tag=$3 commit=$4
    if [ -d "$dest/.git" ]; then
        have=$(git -C "$dest" rev-parse HEAD)
        if [ "$have" = "$commit" ]; then
            echo "==> $dest already at $tag ($commit) — skipping"
            return
        fi
        echo "==> $dest is at $have, want $commit — re-fetching"
        rm -rf "$dest"
    fi
    echo "==> Cloning $url $tag into $dest ..."
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$tag" "$url" "$dest"
    have=$(git -C "$dest" rev-parse HEAD)
    if [ "$have" != "$commit" ]; then
        echo "ERROR: $dest $tag is $have, expected $commit" >&2
        exit 1
    fi
}

# CoreMark v1.01 (EEMBC release tag)
fetch coremark    https://github.com/eembc/coremark.git     v1.01      \
      cfa9ab377835911f23d9b0831c7be302ed1f58de
# Embench-IoT 1.0 (the current non-RC release)
fetch embench-iot https://github.com/embench/embench-iot.git embench-1.0 \
      0466a18e4f6b47e19598d7c6ba72916d54b68f65

echo ""
echo "==> Done.  Now:  cd .. && make coremark  /  make embench"
