#!/usr/bin/env bash
# linux/userland/tests/tlb/model/run.sh: the TLB what-if workloads in the
# emulator with klausscc's KLAUSSCC_TLBSIM=1 model (adapted from
# ../../libffi/run.sh's ctypes mode): Python, sysloop and sqlite3 from the
# shared Buildroot build in a minish initramfs, each under perfstat, which
# windows the model.  Output: $W/tlbsim.err (TLBSIM lines), tlbsim.out (the
# console); analyze.py $W/tlbsim.err prints the tables (LINUX_PLAN.md "TLB
# size and refill").  KLAUSSCC_ARGS e.g. "--tlb-entries 16".
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../../../.." && pwd)
W=${TLBSIM_OUT:-$RT/linux-ws/tlbsim}
BR=$RT/linux-ws/br-output-dyn/target
mkdir -p "$W"; cp "$HERE/tsrun.c" "$HERE/true.c" "$W/"
export KLAUSSCPU_LLVM_BIN=$RT/../klausscpu-llvm/build/bin
export LINUX_BUILD=$W/build
KLAUSSCC=${KLAUSSCC:-$RT/../klausscc/target/release/klausscc}
PYV=python3.14
cd $W
CC=("$KLAUSSCPU_LLVM_BIN/clang" --target=klausscpu-unknown-linux-musl --sysroot="$RT/linux-ws/sysroot" -static -O2)
"${CC[@]}" -o tsrun tsrun.c
"${CC[@]}" -o true true.c
"${CC[@]}" -o perfstat "$RT/linux/userland/perfstat.c"
"${CC[@]}" -o sysloop "$RT/linux/userland/sysloop.c"
printf 's=0\nfor i in range(50000): s+=i*i\n' > loop.py
cat > list <<'L'
py0     PYTHONHASHSEED=0 /bin/perfstat /usr/bin/python3 -S -c pass
py0b    PYTHONHASHSEED=0 /bin/perfstat /usr/bin/python3 -S -c pass
pyloop  PYTHONHASHSEED=0 /bin/perfstat /usr/bin/python3 -S /test/loop.py
fork    /bin/perfstat /bin/sysloop fork 200
fork2   /bin/perfstat /bin/sysloop fork 200
exec    /bin/perfstat /bin/sysloop exec 200 /bin/true
fault   /bin/perfstat /bin/sysloop fault 2000
sqlite  </test/sqlite-test.sql /bin/perfstat /usr/bin/sqlite3 /tmp/t.db
reopen  /bin/perfstat /usr/bin/sqlite3 /tmp/t.db SELECT_1
L
sed -i 's/SELECT_1/"SELECT 1;"/' list
# the tokenizer splits on blanks: use a one-word statement
sed -i 's/"SELECT 1;"/.tables/' list
{
cat <<LISTEOF
dir /lib 0755 0 0
dir /test 0755 0 0
file /lib/ld-musl-klausscpu.so.1 $BR/usr/lib/ld-musl-klausscpu.so.1 0755 0 0
file /bin/tsrun $W/tsrun 0755 0 0
file /bin/perfstat $W/perfstat 0755 0 0
file /bin/sysloop $W/sysloop 0755 0 0
file /bin/true $W/true 0755 0 0
file /test/list $W/list 0644 0 0
file /test/loop.py $W/loop.py 0644 0 0
file /test/sqlite-test.sql $RT/linux/userland/tests/sqlite-test.sql 0644 0 0
dir /usr 0755 0 0
dir /usr/bin 0755 0 0
dir /usr/lib 0755 0 0
file /usr/bin/$PYV $BR/usr/bin/$PYV 0755 0 0
slink /usr/bin/python3 $PYV 0777 0 0
file /usr/bin/sqlite3 $BR/usr/bin/sqlite3 0755 0 0
LISTEOF
for l in $BR/usr/lib/libpython3*.so* $BR/usr/lib/libz.so.1* $BR/usr/lib/libreadline.so.8* $BR/usr/lib/libncurses.so.6*; do
  [ -e "$l" ] || continue
  if [ -L "$l" ]; then echo "slink /usr/lib/$(basename $l) $(readlink $l) 0777 0 0"
  else echo "file /usr/lib/$(basename $l) $l 0755 0 0"; fi
done
T=$BR/usr/lib
EX=(-path "$PYV/test" -prune -o -path "$PYV/ensurepip" -prune -o -path "$PYV/idlelib" -prune -o -path "$PYV/tkinter" -prune -o -path "$PYV/turtledemo" -prune -o -path "$PYV/config-*" -prune -o -path "$PYV/site-packages" -prune -o)
(cd $T && find $PYV "${EX[@]}" -type d -print) | sort | while read -r d; do echo "dir /usr/lib/$d 0755 0 0"; done
(cd $T && find $PYV "${EX[@]}" -type f -print) | sort | while read -r f; do echo "file /usr/lib/$f $T/$f 0644 0 0"; done
} > tlbsim.list
(cd $RT && INITRAMFS_PROFILE=minish INITRAMFS_EXTRA="$W/tlbsim.list" linux/build.sh defconfig && INITRAMFS_PROFILE=minish INITRAMFS_EXTRA="$W/tlbsim.list" linux/build.sh vmlinux) > kernel.log 2>&1 || { tail -30 kernel.log; exit 2; }
printf 'mount\n/bin/tsrun /test/list\necho TS-ALL-DONE\n' > tlbsim.in
start=$(date +%s)
KLAUSSCC_TLBSIM=${SIM:-1} "$KLAUSSCC" ${KLAUSSCC_ARGS:-} -i "$LINUX_BUILD/vmlinux" --emulate --max-instructions 200000000000 \
  --uart-in tlbsim.in --uart-prompt '$ ' </dev/null >tlbsim.out 2>tlbsim.err || true
echo "emulator: $(( $(date +%s) - start )) s"
grep -a "Emulator finished" tlbsim.out
