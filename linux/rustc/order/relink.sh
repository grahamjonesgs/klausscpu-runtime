#!/usr/bin/env bash
# linux/rustc/order/relink.sh [LINK ARGS...] — relink the board rustc's
# librustc_driver with extra link arguments (RUST_ON_BOARD_PLAN.md R8 c) and
# nothing else rebuilt.  Without arguments: the call-graph order in
# driver-order.txt.gz.  The arguments reach only the driver's link
# (linux/rustc/klausscpu-clang, RUSTC_DRIVER_LINK_ARGS), so no crate hash or
# symbol name changes.
#   RUSTC_TREE  default ~/Documents/src/klausscpu-p5-rust-build/klausscpu-rust
#               (bootstrap-cross.toml, build-cross)
# Then linux/rustc/build-rust-image.sh.
#
# The order fits the rustc tree it was profiled from (klausscpu-rust
# 03743a7a6 with rustc-patches 0005-0006): a different tree renames symbols
# and the order then silently does nothing.  To make a new one:
#   1. build without an order;
#   2. order/run.sh (emulator profiles of hello, threads and stdcheck, about
#      6 minutes);
#   3. in ~/.cache/rustord/base: order/symbolize-so.py driver.nm prof-<n>.1 <n>
#      for each workload, then klausscc tools/tlbord/gen-orders.py ord hello
#      threads stdcheck;
#   4. relink.sh -Wl,--call-graph-ordering-file=ord/cg-all.txt (lld cdsort);
#   5. klausscc tools/tlbord/cdsx.py ord/cg-all.txt <llvm-nm --defined-only -n
#      -S of that driver> ord/funcs-hot.txt order.txt;
#   6. gzip -9 it to driver-order.txt.gz and run relink.sh.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../.." && pwd)
TREE=${RUSTC_TREE:-$HOME/Documents/src/klausscpu-p5-rust-build/klausscpu-rust}
if [ $# = 0 ]; then
	ORDER=$TREE/build-cross/driver-order.txt
	gunzip -c "$HERE/driver-order.txt.gz" >"$ORDER"
	set -- "-Wl,--symbol-ordering-file=$ORDER" -Wl,--no-warn-symbol-ordering
fi
cd "$TREE"
rm -f build-cross/x86_64-unknown-linux-gnu/stage2-rustc/klausscpu-unknown-linux-musl/release/deps/librustc_driver-*.so
RUSTC_DRIVER_LINK_ARGS="$*" \
KLAUSSCPU_UNKNOWN_LINUX_MUSL_OPENSSL_DIR=$RT/linux-ws/br-output-dyn/staging/usr \
	./x.py build --config bootstrap-cross.toml --stage 2 compiler/rustc library cargo
