#!/usr/bin/env bash
# linux/userland/tests/dynlink/run.sh — DYNLINK_PLAN M5: dynamic linking
# tests on the klausscc emulator.
#
# Builds the test libraries and programs with the dynlink toolchain against
# a sysroot that has libc.so, checks the ELF files on the host
# (llvm-readelf), builds a kernel whose initramfs (minish) carries them,
# boots it on klausscc, types the test commands at minish and checks the
# output.  Exit 0 only if every check passes.
#
#   DYN_BIN       toolchain with dynamic linking (default ../klausscpu-llvm/build/bin)
#   DYN_SYSROOT   musl sysroot with lib/libc.so, Scrt1.o (default
#                 linux-ws/dyn/sysroot)
#   KLAUSSCPU_LLVM_BIN  toolchain for the kernel and minish (default
#                 ../klausscpu-llvm/build/bin)
#   LINUX_BUILD   kernel build directory (default linux-ws/build-dynlink-m5)
#   DT_OUT        test build/output directory (default linux-ws/dynlink-m5)
#   KLAUSSCC      emulator (default ../klausscc/target/release/klausscc)
#   DT_RUNS       start-up timing runs per program (default 200)
#   DT_SKIP_KERNEL=1  reuse the kernel already in LINUX_BUILD (only when the
#                 test binaries did not change: the initramfs is built in)
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../../.." && pwd)
SRC=$(cd "$RT/.." && pwd)
DYN_BIN=${DYN_BIN:-$SRC/klausscpu-llvm/build/bin}
DYN_SYSROOT=${DYN_SYSROOT:-$RT/linux-ws/dyn/sysroot}
export KLAUSSCPU_LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$SRC/klausscpu-llvm/build/bin}
export LINUX_BUILD=${LINUX_BUILD:-$RT/linux-ws/build-dynlink-m5}
OUT=${DT_OUT:-$RT/linux-ws/dynlink-m5}
KLAUSSCC=${KLAUSSCC:-$SRC/klausscc/target/release/klausscc}
RUNS=${DT_RUNS:-200}

[ -f "$DYN_SYSROOT/lib/libc.so" ] || { echo "no $DYN_SYSROOT/lib/libc.so (set DYN_SYSROOT)" >&2; exit 2; }
mkdir -p "$OUT"
cd "$OUT"
FAILS=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }

# ---- build ----
CC=("$DYN_BIN/clang" --target=klausscpu-unknown-linux-musl --sysroot="$DYN_SYSROOT" -O2 -Wall -I"$HERE")
# -fsemantic-interposition: clang's -fPIC default (half no semantic
# interposition) lets the optimiser fold libdt1's call to its own
# default-visibility dt1_interp; the test is of the loader's binding.
"${CC[@]}" -fPIC -fsemantic-interposition -shared -Wl,-soname,libdt1.so "$HERE/libdt1.c" -o libdt1.so
"${CC[@]}" -fPIC -shared -Wl,-soname,libdt2.so "$HERE/libdt2.c" -L. -ldt1 -o libdt2.so
"${CC[@]}" -fPIC -shared -Wl,-soname,libdt3.so "$HERE/libdt3.c" -o libdt3.so
"${CC[@]}" -fPIC -shared -Wl,-soname,libpre.so "$HERE/libpre.c" -o libpre.so
"${CC[@]}" -fPIE -pie -rdynamic "$HERE/dtmain.c" -L. -ldt1 -o dt-main
"${CC[@]}" -fPIE -pie "$HERE/dtldlp.c" -L. -ldt3 -o dt-ldlp
"${CC[@]}" -fPIE -pie "$HERE/dtldlp.c" -L. -ldt3 -Wl,-rpath,'$ORIGIN/../dtlib' -o dt-rpath
"${CC[@]}" -fPIE -pie "$HERE/true.c" -o dt-true-dyn
"${CC[@]}" -fPIE -pie "$HERE/true.c" -L. -ldt1 -o dt-true-dt1
"${CC[@]}" -static "$HERE/true.c" -o dt-true-static
"${CC[@]}" -static "$HERE/dtstart.c" -o dt-start
cp -L "$DYN_SYSROOT/lib/libc.so" ld-musl-klausscpu.so.1
# Broken copies of libdt3.so for the negative tests.
python3 - <<'PY'
import struct
good = open('libdt3.so', 'rb').read()
def patch(name, off, data):
    b = bytearray(good); b[off:off + len(data)] = data; open(name, 'wb').write(b)
patch('libbad-mach.so', 18, struct.pack('<H', 0xf3))      # e_machine EM_RISCV
patch('libbad-abi.so', 36, bytes([2]))                     # e_flags ABI version 2
patch('libbad-class.so', 4, bytes([2]))                    # ELFCLASS64
open('libbad-trunc.so', 'wb').write(good[:200])
PY
echo "built in $OUT"

# ---- host checks of the ELF files ----
RE="$DYN_BIN/llvm-readelf"
relocs() { "$RE" -r "$1" | awk '/R_KLAUSSCPU_/ {print $3}' | sort | uniq -c | tr '\n' ' '; }
has_reloc() { "$RE" -r "$1" | grep -c >/dev/null "$2"; }
for f in libdt1.so libdt2.so libpre.so dt-main; do
	echo "INFO $f relocations: $(relocs $f)"
done
for t in R_KLAUSSCPU_RELATIVE R_KLAUSSCPU_ABS64 R_KLAUSSCPU_GLOB_DAT R_KLAUSSCPU_JUMP_SLOT \
	R_KLAUSSCPU_DTPMOD64 R_KLAUSSCPU_DTPOFF64; do
	has_reloc libdt1.so $t && pass "host-libdt1-$t" || fail "host-libdt1-$t: not in libdt1.so"
done
for t in R_KLAUSSCPU_RELATIVE R_KLAUSSCPU_GLOB_DAT R_KLAUSSCPU_JUMP_SLOT R_KLAUSSCPU_TPOFF64; do
	has_reloc dt-main $t && pass "host-dt-main-$t" || fail "host-dt-main-$t: not in dt-main"
done
"$RE" -rW libdt1.so | grep -c >/dev/null 'JUMP_SLOT.* dt1_interp' && pass "host-libdt1-interp-via-plt" \
	|| fail "host-libdt1-interp-via-plt: libdt1's call to dt1_interp is not through the PLT"
has_reloc dt-main R_KLAUSSCPU_COPY && fail "host-dt-main-no-copy: COPY relocation in a -fPIE PIE" \
	|| pass "host-dt-main-no-copy"
for f in libdt1.so libdt2.so dt-main; do
	if "$RE" -d $f | grep -c >/dev/null TEXTREL; then fail "host-$f-no-textrel"; else pass "host-$f-no-textrel"; fi
	al=$("$RE" -lW $f | awk '$1=="LOAD" {print $NF}' | sort -u | tr '\n' ' ')
	[ "$al" = "0x10000 " ] && pass "host-$f-align: PT_LOAD p_align $al" || fail "host-$f-align: PT_LOAD p_align $al"
	"$RE" -lW $f | grep -c >/dev/null GNU_RELRO && pass "host-$f-relro" || fail "host-$f-relro: no PT_GNU_RELRO"
	"$RE" -d $f | grep -c >/dev/null 'FLAGS.*BIND_NOW' && pass "host-$f-bind-now" || fail "host-$f-bind-now"
done
"$RE" -lW dt-main | grep -c >/dev/null 'Requesting program interpreter: /lib/ld-musl-klausscpu.so.1' \
	&& pass "host-dt-main-interp" || fail "host-dt-main-interp"
"$RE" -d dt-rpath | grep -c >/dev/null 'RUNPATH.*\$ORIGIN/../dtlib' && pass "host-dt-rpath-runpath" \
	|| fail "host-dt-rpath-runpath: $("$RE" -d dt-rpath | grep -i path)"
# .data.rel.ro of libdt1 lies inside PT_GNU_RELRO.
python3 - "$RE" <<'PY' && pass "host-libdt1-datarelro-in-relro" || fail "host-libdt1-datarelro-in-relro"
import subprocess, sys
re_ = sys.argv[1]
secs = subprocess.run([re_, '-SW', 'libdt1.so'], capture_output=True, text=True).stdout
ph = subprocess.run([re_, '-lW', 'libdt1.so'], capture_output=True, text=True).stdout
drr = [l.split() for l in secs.splitlines() if '.data.rel.ro' in l]
rel = [l.split() for l in ph.splitlines() if l.split()[:1] == ['GNU_RELRO']]
if not drr or not rel: sys.exit(1)
f = drr[0][drr[0].index('.data.rel.ro') + 1:]
addr, size = int(f[1], 16), int(f[3], 16)
r = rel[0]; va, msz = int(r[2], 16), int(r[5], 16)
print(f"INFO .data.rel.ro {addr:#x}+{size:#x} GNU_RELRO {va:#x}+{msz:#x}")
sys.exit(0 if va <= addr and addr + size <= va + msz else 1)
PY

# ---- initramfs extras ----
cat > extra.list <<EOF
dir /lib 0755 0 0
dir /opt 0755 0 0
dir /opt/bin 0755 0 0
dir /opt/dtlib 0755 0 0
dir /opt/badlib 0755 0 0
file /lib/ld-musl-klausscpu.so.1 $OUT/ld-musl-klausscpu.so.1 0755 0 0
file /lib/libdt1.so $OUT/libdt1.so 0755 0 0
file /lib/libdt2.so $OUT/libdt2.so 0755 0 0
file /lib/libpre.so $OUT/libpre.so 0755 0 0
file /lib/libdt3-copy.so $OUT/libdt3.so 0755 0 0
file /lib/libbad-mach.so $OUT/libbad-mach.so 0755 0 0
file /lib/libbad-abi.so $OUT/libbad-abi.so 0755 0 0
file /lib/libbad-class.so $OUT/libbad-class.so 0755 0 0
file /lib/libbad-trunc.so $OUT/libbad-trunc.so 0755 0 0
file /opt/dtlib/libdt3.so $OUT/libdt3.so 0755 0 0
file /opt/badlib/libdt3.so $OUT/libbad-mach.so 0755 0 0
file /bin/dt-main $OUT/dt-main 0755 0 0
file /bin/dt-ldlp $OUT/dt-ldlp 0755 0 0
file /opt/bin/dt-rpath $OUT/dt-rpath 0755 0 0
file /bin/dt-true-dyn $OUT/dt-true-dyn 0755 0 0
file /bin/dt-true-dt1 $OUT/dt-true-dt1 0755 0 0
file /bin/dt-true-static $OUT/dt-true-static 0755 0 0
file /bin/dt-start $OUT/dt-start 0755 0 0
EOF

# ---- kernel ----
if [ "${DT_SKIP_KERNEL:-0}" != 1 ]; then
	echo "building the kernel in $LINUX_BUILD (log: $OUT/kernel.log)"
	(cd "$RT" && INITRAMFS_PROFILE=minish INITRAMFS_EXTRA="$OUT/extra.list" linux/build.sh defconfig &&
		INITRAMFS_PROFILE=minish INITRAMFS_EXTRA="$OUT/extra.list" linux/build.sh vmlinux) >"$OUT/kernel.log" 2>&1 \
		|| { tail -30 "$OUT/kernel.log"; echo "kernel build failed" >&2; exit 2; }
fi

# ---- emulator ----
MODES="core tls dlopen misc preload ldlp badelf"
{
	echo "mount"	# /proc: musl expands the main program's $ORIGIN with /proc/self/exe
	for m in $MODES; do echo "/bin/dt-main $m"; done
	echo "/lib/ld-musl-klausscpu.so.1 /bin/dt-main core-direct"
	echo "/lib/ld-musl-klausscpu.so.1 --list /bin/dt-main"
	echo "/bin/musltest"
	echo "/bin/dt-start $RUNS /bin/dt-true-static /bin/dt-true-dyn /bin/dt-true-dt1"
	echo "echo DT-ALL-DONE"
} > cmds.in
echo "running klausscc (output: $OUT/emu.out)"
start=$(date +%s)
"$KLAUSSCC" -i "$LINUX_BUILD/vmlinux" --emulate --max-instructions "${DT_MAX_INSN:-40000000000}" \
	--uart-in cmds.in --uart-prompt '$ ' </dev/null >emu.out 2>&1 || true
echo "emulator: $(( $(date +%s) - start )) s"

# ---- check the output ----
O=emu.out
grep -E '^(PASS|FAIL|INFO|TIME|DONE)|killed by signal|\[exit [0-9]+\]|Error|error' $O | grep -v '^\[ ' || true
grep -q '^DT-ALL-DONE' $O && pass "emu-complete" || fail "emu-complete: the command list did not finish"
for m in $MODES core-direct preload-child; do
	grep -q "^DONE $m failures=0" $O && pass "mode-$m" || fail "mode-$m: $(grep "^DONE $m " $O || echo 'no DONE line')"
done
n=$(grep -c '^PASS ctor-dtor-order' $O || true)
[ "$n" = 2 ] && pass "dtor-order-reported" || fail "dtor-order-reported: $n PASS lines (want 2: core, core-direct)"
n=$(grep -c '^FAIL ' $O || true)
[ "$n" = 0 ] && pass "no-guest-fail-lines" || fail "no-guest-fail-lines: $n FAIL lines in the guest output"
grep -q 'killed by signal' $O && fail "no-crash: $(grep 'killed by signal' $O | head -3 | tr '\n' ' ')" || pass "no-crash"
grep -qE '^PASS (ldlp-child|rpath-child)' $O && [ "$(grep -cE '^PASS (ldlp-child|rpath-child)' $O)" = 2 ] \
	&& pass "ldlp-children" || fail "ldlp-children"
grep -q 'libdt1.so => /lib/libdt1.so' $O && pass "ldd-list" || fail "ldd-list"
grep -q '^musltest: PASS' $O && pass "musltest-static-control" || fail "musltest-static-control"
grep -q '^PASS start-time' $O && pass "start-time" || fail "start-time"
grep '^TIME' $O || true
echo "RESULT: $FAILS failing checks"
[ "$FAILS" = 0 ]
