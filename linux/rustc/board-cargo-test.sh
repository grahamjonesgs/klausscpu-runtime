#!/usr/bin/env bash
# linux/rustc/board-cargo-test.sh — RUST_ON_BOARD_PLAN.md R7: cargo and
# rustc on the board, in /data/rustc-cargo-test (holding both board locks):
#   hello    `cargo new` + `cargo run --release` (no dependencies)
#   derive   a workspace with a local proc-macro crate (#[derive(Hello)],
#            only the proc_macro API): rustc builds the macro as a shared
#            library for the board and loads it (dlopen)
#   online   one crates.io dependency (itoa): cargo fetches the index and
#            the crate over HTTPS (curl + the board's OpenSSL); skipped with
#            CARGO_TEST_OFFLINE=1
# Prints the time of each build and exits 1 if one fails.
set -euo pipefail
BOARD=${KLAUSSCPU_BOARD:-root@192.168.68.59}
KEY=${KLAUSSCPU_BOARD_KEY:-$HOME/.ssh/klausscpu_board_ed25519}
OPTS=(-o BatchMode=yes -o ConnectTimeout=20 -o ServerAliveInterval=30
	-o UserKnownHostsFile="$HOME/.cache/klausscpu-board-known_hosts" -o StrictHostKeyChecking=accept-new)
[ -f "$KEY" ] && OPTS+=(-i "$KEY" -o IdentitiesOnly=yes)
ONLINE=1
[ "${CARGO_TEST_OFFLINE:-0}" = 1 ] && ONLINE=0
exec flock "/run/user/$(id -u)/klausscpu-board.lock" flock "$HOME/.cache/klausscpu-board.lock" \
	ssh "${OPTS[@]}" "$BOARD" "ONLINE=$ONLINE sh -l" <<'BOARD'
set -u
fail=0
step() { # step <name> <command...>
	name=$1; shift
	start=$(date +%s)
	if "$@" >"/data/rustc-cargo-test/$name.log" 2>&1; then r=PASS; else r=FAIL; fail=1; fi
	echo "$r $name ($(( $(date +%s) - start )) s)"
	[ $r = PASS ] || tail -15 "/data/rustc-cargo-test/$name.log"
}
rm -rf /data/rustc-cargo-test && mkdir -p /data/rustc-cargo-test && cd /data/rustc-cargo-test
rustc -vV | head -1; cargo -V
free -m | sed -n 2,3p

# 1. hello
cargo new -q hello
step hello-build sh -c 'cd hello && cargo build --release'
step hello-run sh -c 'cd hello && ./target/release/hello | grep -qx "Hello, world!"'

# 2. a proc-macro crate in the same workspace
mkdir -p derive/hello_macro/src derive/app/src
cat >derive/Cargo.toml <<'EOF'
[workspace]
members = ["hello_macro", "app"]
resolver = "2"
EOF
cat >derive/hello_macro/Cargo.toml <<'EOF'
[package]
name = "hello_macro"
version = "0.1.0"
edition = "2024"
[lib]
proc-macro = true
EOF
cat >derive/hello_macro/src/lib.rs <<'EOF'
use proc_macro::{TokenStream, TokenTree};

/// #[derive(Hello)] on `struct Name ...` gives `impl Name { fn hello() -> &str }`.
#[proc_macro_derive(Hello)]
pub fn derive_hello(input: TokenStream) -> TokenStream {
    let mut tokens = input.into_iter();
    let name = loop {
        match tokens.next() {
            Some(TokenTree::Ident(i)) if i.to_string() == "struct" => match tokens.next() {
                Some(TokenTree::Ident(n)) => break n.to_string(),
                _ => panic!("expected a struct name"),
            },
            Some(_) => continue,
            None => panic!("#[derive(Hello)] needs a struct"),
        }
    };
    format!(
        "impl {name} {{ pub fn hello() -> &'static str {{ \"Hello from {name}, derived on KlaussCPU\" }} }}"
    )
    .parse()
    .unwrap()
}
EOF
cat >derive/app/Cargo.toml <<'EOF'
[package]
name = "app"
version = "0.1.0"
edition = "2024"
[dependencies]
hello_macro = { path = "../hello_macro" }
EOF
cat >derive/app/src/main.rs <<'EOF'
use hello_macro::Hello;

#[derive(Hello)]
struct Board;

fn main() {
    println!("{}", Board::hello());
}
EOF
step derive-build sh -c 'cd derive && cargo build --release'
step derive-run sh -c 'cd derive && ./target/release/app | grep -qx "Hello from Board, derived on KlaussCPU"'

# 3. a crates.io dependency
if [ "$ONLINE" = 1 ]; then
	cargo new -q online
	printf 'itoa = "1"\n' >>online/Cargo.toml
	cat >online/src/main.rs <<'EOF'
fn main() {
    let mut buf = itoa::Buffer::new();
    println!("itoa says {}", buf.format(1_234_567_u64));
}
EOF
	step online-build sh -c 'cd online && cargo build --release'
	step online-run sh -c 'cd online && ./target/release/online | grep -qx "itoa says 1234567"'
fi
free -m | sed -n 2,3p
exit $fail
BOARD
