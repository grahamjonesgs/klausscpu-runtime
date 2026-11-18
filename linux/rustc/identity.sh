#!/usr/bin/env bash
# linux/rustc/identity.sh — RUST_ON_BOARD_PLAN.md R6: the board's rustc
# (/opt/rust) must produce byte-identical objects to the cross rustc built
# from the same tree (bootstrap-cross.toml's x86 stage1, with the same std).
# Each source is compiled on both at each opt level:
#   rustc --edition 2024 --crate-type bin -C opt-level=N -C codegen-units=1
#         --emit=obj -o <name>.o <name>.rs
# from a directory holding just <name>.rs, so the paths in the objects agree.
# The board part holds both board locks and works in /data/rustc-identity.
# Prints a line per object and exits 1 if any differ.
#   IDENT_SOURCES  the .rs files (default: hello, stdcheck, the threads demo)
#   IDENT_OPT      opt levels (default "0 2")
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../.." && pwd)
X=${RUSTC_CROSS_BUILD:-$HOME/Documents/src/klausscpu-p5-rust-build/klausscpu-rust/build-cross}
HOSTRUSTC=$X/x86_64-unknown-linux-gnu/stage1/bin/rustc
LLVMR=$RT/../klausscpu-llvm/llvm/lib/Target/KlaussCPU/rust
W=${IDENT_WORK:-$HOME/.cache/rustc-identity}
BOARD=${KLAUSSCPU_BOARD:-root@192.168.68.59}
KEY=${KLAUSSCPU_BOARD_KEY:-$HOME/.ssh/klausscpu_board_ed25519}
OPTS=(-o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=30
	-o UserKnownHostsFile="$HOME/.cache/klausscpu-board-known_hosts" -o StrictHostKeyChecking=accept-new)
[ -f "$KEY" ] && OPTS+=(-i "$KEY" -o IdentitiesOnly=yes)
locked() { flock "/run/user/$(id -u)/klausscpu-board.lock" flock "$HOME/.cache/klausscpu-board.lock" "$@"; }
rm -rf "$W" && mkdir -p "$W/src" "$W/host"
printf 'fn main() { println!("Hello from rustc on KlaussCPU"); }\n' >"$W/src/hello.rs"
cp "$LLVMR/linux-std/stdcheck/src/main.rs" "$W/src/stdcheck.rs"
cat >"$W/src/threads.rs" <<'EOF'
use std::{env, thread};
fn main() {
    let user = env::var("USER").unwrap_or_else(|_| "world".into());
    println!("Hello, {user}, from Rust on KlaussCPU!");
    let handles: Vec<_> = (1..=4)
        .map(|i| thread::spawn(move || (i, (1..=i * 5).sum::<u64>())))
        .collect();
    for h in handles {
        let (i, sum) = h.join().unwrap();
        println!("  thread {i}: 1 + ... + {} = {sum}", i * 5);
    }
}
EOF
SOURCES=${IDENT_SOURCES:-hello stdcheck threads}
OPT=${IDENT_OPT:-0 2}
cmd() { # the compile command for <name> <opt>, run in a directory with <name>.rs
	echo "rustc --edition 2024 --crate-type bin -C opt-level=$2 -C codegen-units=1 --emit=obj -o $1-O$2.o $1.rs"
}
for s in $SOURCES; do
	for o in $OPT; do
		d=$W/host/$s-O$o && mkdir -p "$d" && cp "$W/src/$s.rs" "$d/"
		(cd "$d" && "$HOSTRUSTC" --target klausscpu-unknown-linux-musl $(cmd "$s" "$o" | cut -d' ' -f2-))
	done
done
# The board: the same tree of directories, the same commands (its rustc's
# host is the target, so no --target).
script="set -e; export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/opt/llvm/bin:/opt/rust/bin
rm -rf /data/rustc-identity; mkdir -p /data/rustc-identity; cd /data/rustc-identity"
for s in $SOURCES; do
	for o in $OPT; do
		script="$script
mkdir $s-O$o; cp /data/rustc-identity-src/$s.rs $s-O$o/; cd $s-O$o
start=\$(date +%s); $(cmd "$s" "$o"); echo \"time $s-O$o \$((\$(date +%s) - start)) s\"
sha256sum $s-O$o.o; cd .."
	done
done
locked ssh "${OPTS[@]}" "$BOARD" "rm -rf /data/rustc-identity-src && mkdir -p /data/rustc-identity-src"
locked scp -q "${OPTS[@]}" "$W"/src/*.rs "$BOARD:/data/rustc-identity-src/"
locked ssh "${OPTS[@]}" "$BOARD" "$script" | tee "$W/board.txt"
fail=0
for s in $SOURCES; do
	for o in $OPT; do
		h=$(sha256sum "$W/host/$s-O$o/$s-O$o.o" | cut -d' ' -f1)
		b=$(awk -v f="$s-O$o.o" '$2 == f { print $1 }' "$W/board.txt")
		if [ "$h" = "$b" ]; then
			echo "SAME $s-O$o.o"
		else
			echo "DIFF $s-O$o.o (host $h, board ${b:-none})"
			fail=1
		fi
	done
done
exit $fail
