#!/usr/bin/env bash
# linux/rustc/build-rust-image.sh — linux-ws/rust.img: rustc for the board
# (RUST_ON_BOARD_PLAN.md R4/R5), an ext4 image like toolchain.img, to be
# mounted read-only at /opt/rust:
#   bin/rustc
#   bin/cargo                             (when built: x.py build ... cargo;
#                                         RUST_ON_BOARD_PLAN.md R7)
#   lib/librustc_driver-<hash>.so         (rustc's RUNPATH is $ORIGIN/../lib)
#   lib/rustlib/klausscpu-unknown-linux-musl/lib/   std and its crates
# rustc finds its sysroot from its own path, and links with clang from
# /opt/llvm (on PATH in login shells).  Both binaries are stripped.
#   RUSTC_CROSS_BUILD  bootstrap's build directory of the cross rustc
#                      (default ~/Documents/src/klausscpu-p5-rust-build/
#                      klausscpu-rust/build-cross, from bootstrap-cross.toml)
#   OUTIMG             default linux-ws/rust.img
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
B=${KLAUSSCPU_LLVM_BIN:-$(cd "$RT/../klausscpu-llvm/build/bin" && pwd)}
X=${RUSTC_CROSS_BUILD:-$HOME/Documents/src/klausscpu-p5-rust-build/klausscpu-rust/build-cross}
T=klausscpu-unknown-linux-musl
OUTIMG=${OUTIMG:-$RT/linux-ws/rust.img}
S2=$X/$T/stage2
STD=$X/x86_64-unknown-linux-gnu/stage1-std/$T/dist/deps
[ -x "$S2/bin/rustc" ] || { echo "no cross rustc in $S2 (RUST_ON_BOARD_PLAN.md R3)" >&2; exit 1; }
ST=$(mktemp -d "${TMPDIR:-$HOME/.cache}/rust-img.XXXXXX")
trap 'rm -rf "$ST" "$ST.tar"' EXIT
# mktemp makes the directory 0700, and that mode becomes the image's root
# (/opt/rust): only root could reach rustc and cargo.
chmod 755 "$ST"
mkdir -p "$ST/bin" "$ST/lib/rustlib/$T/lib"
"$B/llvm-strip" --strip-all -o "$ST/bin/rustc" "$S2/bin/rustc"
CARGO=$X/x86_64-unknown-linux-gnu/stage2-tools/$T/release/cargo
[ -x "$CARGO" ] && "$B/llvm-strip" --strip-all -o "$ST/bin/cargo" "$CARGO"
for f in "$S2"/lib/librustc_driver-*.so; do
  "$B/llvm-strip" --strip-all -o "$ST/lib/$(basename "$f")" "$f"
done
# std and the crates it is built from (the stage1 std build's own outputs, so
# none of the compiler's rlibs that bootstrap also puts in this directory).
# The full metadata is in the .rmeta files (bootstrap builds std with
# -Zembed-metadata=no: the .rlib holds only a stub).
cp "$STD"/*.rlib "$STD"/*.rmeta "$STD"/libstd-*.so "$ST/lib/rustlib/$T/lib/"
du -sh "$ST/bin" "$ST/lib/rustlib" "$ST" | sed 's/^/rust: /'
used=$(du -sm "$ST" | cut -f1)
SIZE=${SIZE:-$(( (used * 110 / 100) + 8 ))}
tar --owner=0 --group=0 --numeric-owner -C "$ST" -cf "$ST.tar" .
rm -f "$OUTIMG"
mke2fs -q -t ext4 -O ^has_journal -L klausscpu-rust -m 0 -d "$ST.tar" "$OUTIMG" "${SIZE}M"
e2fsck -fn "$OUTIMG" >/dev/null
echo "rust: $OUTIMG (${SIZE} MiB, ${used} MiB used, sha256 $(sha256sum "$OUTIMG" | cut -d' ' -f1))"
