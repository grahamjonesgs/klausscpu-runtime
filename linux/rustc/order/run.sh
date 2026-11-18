#!/usr/bin/env bash
# linux/rustc/order/run.sh — RUST_ON_BOARD_PLAN.md R8 c: profile the board's
# rustc in the klausscc emulator for a call-graph link order of
# librustc_driver (as CLANG_ON_BOARD_PLAN.md M6 did for clang, with
# klausscc tools/tlbord).  Each workload boots its own emulator (the guest
# has the board's 128 MiB, so rustc pages from the emulated SD card as it
# does on the board).  The guest:
#   initramfs  minish, perfstat, mntdata (from ~/.cache/tlbord/in, built by
#              tlbord/run.sh), the sources in /w, and /lib/ld-musl-klausscpu.so.1
#              -> /data/lib/libc.so
#   SD card    rust.img's layout (bin/rustc, lib/librustc_driver-*.so, std)
#              plus the shared libraries it needs (musl, libc++, libc++abi,
#              libunwind) in lib/, mounted read-only on /data
# Each compile runs once under perfstat; KLAUSSCC_PROFILE writes that
# window's per-PC counts and calls to $O/prof-<name>.1{,.calls}.
#   RUSTC_CROSS_BUILD  the bootstrap build dir of the rustc to profile
#                      (default klausscpu-rust/build-cross); its UNSTRIPPED
#                      driver is what symbolize-so.py reads
#   NAMES   workloads (default "hello threads stdcheck"): hello -O0, threads
#           -O2, stdcheck -O0, each `--emit=obj` (no link step)
#   W       work dir (default ~/.cache/rustord); output in $W/$TAG
#   TAG     default "base"
#   KLAUSSCC, LINUX_BUILD (default linux-ws/build-rustord, made from
#           build-tlbord's config with this initramfs)
# Then: symbolize-so.py per workload, klausscc tools/tlbord/gen-orders.py,
# and cdsx.py (the steps are in relink.sh).
{
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../.." && pwd)
SRC=$(cd "$RT/.." && pwd)
W=${W:-$HOME/.cache/rustord}
TAG=${TAG:-base}
KLAUSSCC=${KLAUSSCC:-$SRC/klausscc/target/release/klausscc}
X=${RUSTC_CROSS_BUILD:-$SRC/klausscpu-p5-rust-build/klausscpu-rust/build-cross}
NAMES=${NAMES-hello threads stdcheck}
T=klausscpu-unknown-linux-musl
B=$SRC/klausscpu-llvm/build/bin
TLB=$HOME/.cache/tlbord/in
export KLAUSSCPU_LLVM_BIN=$B
export LINUX_BUILD=${LINUX_BUILD:-$RT/linux-ws/build-rustord}
O=$W/$TAG
mkdir -p $W/in $O
for f in perfstat mntdata; do [ -x $TLB/$f ] || { echo "no $TLB/$f (run klausscc tools/tlbord/run.sh once)" >&2; exit 1; }; done
if [ ! -f $W/in/done ]; then
  cd $W/in
  cp $TLB/perfstat $TLB/mntdata .
  printf 'fn main() { println!("Hello from rustc on KlaussCPU"); }\n' >hello.rs
  cp $SRC/klausscpu-llvm/llvm/lib/Target/KlaussCPU/rust/linux-std/stdcheck/src/main.rs stdcheck.rs
  sed -n '/^cat >"\$W\/src\/threads.rs" <<.EOF.$/,/^EOF$/p' $RT/linux/rustc/identity.sh | sed '1d;$d' >threads.rs
  grep -q 'thread::spawn' threads.rs
  cat >initramfs.list <<L
dir /tmp 1777 0 0
dir /w 0755 0 0
dir /data 0755 0 0
dir /lib 0755 0 0
slink /lib/ld-musl-klausscpu.so.1 /data/lib/libc.so 0777 0 0
file /bin/perfstat $W/in/perfstat 0755 0 0
file /bin/mntdata $W/in/mntdata 0755 0 0
file /w/hello.rs $W/in/hello.rs 0644 0 0
file /w/threads.rs $W/in/threads.rs 0644 0 0
file /w/stdcheck.rs $W/in/stdcheck.rs 0644 0 0
L
  [ -d $LINUX_BUILD ] || cp -a $RT/linux-ws/build-tlbord $LINUX_BUILD
  (cd $RT && INITRAMFS_PROFILE=minish INITRAMFS_EXTRA="$W/in/initramfs.list" linux/build.sh defconfig &&
    linux-ws/linux/scripts/config --file $LINUX_BUILD/.config --set-str CMDLINE "console=hvc0 rdinit=/init rootfstype=ramfs" &&
    INITRAMFS_PROFILE=minish INITRAMFS_EXTRA="$W/in/initramfs.list" linux/build.sh vmlinux) >$W/kernel.log 2>&1 || { tail -30 $W/kernel.log; exit 2; }
  touch $W/in/done
fi
# The SD card: rust.img's layout from this build, plus the shared libraries.
OUTIMG=$O/rust.img RUSTC_CROSS_BUILD=$X "$RT/linux/rustc/build-rust-image.sh" >$O/image.log 2>&1 || { tail $O/image.log; exit 2; }
rm -rf $O/sd; mkdir -p $O/sd
debugfs -R "rdump / $O/sd" $O/rust.img >/dev/null 2>&1
rm -rf $O/sd/lost+found
SYS=$RT/linux-ws/dyn/sysroot/lib
cp -L $SYS/libc.so $SYS/libc++.so.1 $SYS/libc++abi.so.1 $SYS/libunwind.so.1 $O/sd/lib/
rm -f $O/sd.img $O/rust.img
mke2fs -q -t ext4 -d $O/sd -L rustord $O/sd.img 512M
rm -rf $O/sd
# The unstripped driver, for symbolize-so.py.
D=$(ls $X/$T/stage2/lib/librustc_driver-*.so)
"$B/llvm-nm" --defined-only -n -S "$D" >$O/driver.nm
"$B/llvm-readelf" -lW "$D" >$O/driver.phdrs
R=/data/bin/rustc
cmd() {
  case $1 in
  hello) echo "$R --edition 2024 --crate-type bin -C opt-level=0 --emit=obj -o /tmp/o.o /w/hello.rs" ;;
  threads) echo "$R --edition 2024 --crate-type bin -C opt-level=2 --emit=obj -o /tmp/o.o /w/threads.rs" ;;
  stdcheck) echo "$R --edition 2024 --crate-type bin -C opt-level=0 --emit=obj -o /tmp/o.o /w/stdcheck.rs" ;;
  *) echo "unknown workload $1" >&2; exit 1 ;;
  esac
}
for n in $NAMES; do
  cmd $n >/dev/null
  { echo mount; echo /bin/mntdata; echo "/bin/perfstat $(cmd $n)"; echo "echo TO-DONE-$n"; } >$O/$n.in
  cp $O/sd.img $O/sd-$n.img
  (
    start=$(date +%s)
    env KLAUSSCC_CACHESIM=profile KLAUSSCC_PROFILE=$O/prof-$n \
      "$KLAUSSCC" -i "$LINUX_BUILD/vmlinux" --emulate --max-instructions 200000000000 \
      --sd-native --sd-image $O/sd-$n.img \
      --uart-in $O/$n.in --uart-prompt '$ ' </dev/null >$O/$n.out 2>$O/$n.err || true
    rm -f $O/sd-$n.img
    echo "$TAG $n: $(( $(date +%s) - start )) s"
  ) &
done
wait
rm -f $O/sd.img
exit
}
