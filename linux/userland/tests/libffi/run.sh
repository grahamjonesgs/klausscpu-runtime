#!/usr/bin/env bash
# linux/userland/tests/libffi/run.sh [testsuite] [ctypes]: the libffi port
# (linux/buildroot/external/patches/libffi) on the klausscc emulator.
#
# testsuite: libffi's own testsuite (libffi.call, libffi.closures,
#   libffi.complex at -O0 and -O2, libffi.bhaible's test-call and
#   test-callback) plus this directory's abi.c (KlaussCPU ABI corners: the
#   32-bit sign extension, aggregates split between R3 and the stack, sret,
#   the variadic home area, closures called from compiled code), built as
#   PIE programs against the shared libffi and musl, run under the dynamic
#   loader in the emulator by ffirun (one boot), and checked on the host:
#   exit status 0 and, as DejaGnu does, the test's dg-output patterns
#   (check.py).  Not run, reported UNSUPPORTED: the C++ tests (no C++
#   runtime in the sysroot) and libffi.go (no Go closures in the port).
# ctypes: Buildroot's Python 3.14 (br-output-dyn: python3, libpython,
#   the standard library without its tests, _ctypes and libffi) in the
#   initramfs, running ctypes_smoke.py (a second boot).
#
#   BR_OUT        Buildroot output with libffi and python3 built
#                 (default linux-ws/br-output-dyn)
#   FFI_SRC       libffi source tree with the port and testsuite/
#                 (default $BR_OUT/build/libffi-3.4.8)
#   FFI_INC       directory with ffi.h, ffitarget.h (default $BR_OUT/staging/usr/include)
#   FFI_CONFIG    directory with fficonfig.h (default $FFI_SRC/klausscpu-buildroot-linux-musl,
#                 libffi's build directory)
#   FFI_LIB       directory with libffi.so.8 (default $BR_OUT/staging/usr/lib)
#   DYN_BIN, DYN_SYSROOT, KLAUSSCPU_LLVM_BIN, KLAUSSCC as in ../dynlink/run.sh
#   LINUX_BUILD   kernel build directory (default linux-ws/build-libffi)
#   FT_OUT        build/output directory (default linux-ws/libffi-test)
#   FT_OPTS       optimisation levels (default "-O0 -O2")
#   FT_FILTER     only the tests whose name matches this grep -E pattern
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../../.." && pwd)
SRC=$(cd "$RT/.." && pwd)
DYN_BIN=${DYN_BIN:-$SRC/klausscpu-llvm/build/bin}
DYN_SYSROOT=${DYN_SYSROOT:-$RT/linux-ws/dyn/sysroot}
export KLAUSSCPU_LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$SRC/klausscpu-llvm/build/bin}
export LINUX_BUILD=${LINUX_BUILD:-$RT/linux-ws/build-libffi}
BR_OUT=${BR_OUT:-$RT/linux-ws/br-output-dyn}
FFI_SRC=${FFI_SRC:-$BR_OUT/build/libffi-3.4.8}
FFI_INC=${FFI_INC:-$BR_OUT/staging/usr/include}
FFI_CONFIG=${FFI_CONFIG:-$FFI_SRC/klausscpu-buildroot-linux-musl}
FFI_LIB=${FFI_LIB:-$BR_OUT/staging/usr/lib}
OUT=${FT_OUT:-$RT/linux-ws/libffi-test}
KLAUSSCC=${KLAUSSCC:-$SRC/klausscc/target/release/klausscc}
OPTS=${FT_OPTS:--O0 -O2}
MODES=${*:-testsuite ctypes}

for f in "$DYN_SYSROOT/lib/libc.so" "$FFI_INC/ffi.h" "$FFI_CONFIG/fficonfig.h" "$FFI_LIB/libffi.so.8" \
	"$FFI_SRC/testsuite/libffi.call/ffitest.h"; do
	[ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
done
mkdir -p "$OUT"
cd "$OUT"
FAILS=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }
RE="$DYN_BIN/llvm-readelf"
CC=("$DYN_BIN/clang" --target=klausscpu-unknown-linux-musl --sysroot="$DYN_SYSROOT")

# ffirun is static: it works whatever the dynamic loader does.
"${CC[@]}" -static -O2 -Wall "$HERE/ffirun.c" -o ffirun

# boot <name>: a minish kernel whose initramfs adds <name>.list, running
# "ffirun /test/list"; output in <name>.out.
boot() {
	local name=$1
	echo "building the kernel in $LINUX_BUILD (log: $OUT/$name-kernel.log)"
	(cd "$RT" && INITRAMFS_PROFILE=minish INITRAMFS_EXTRA="$OUT/$name.list" linux/build.sh defconfig &&
		INITRAMFS_PROFILE=minish INITRAMFS_EXTRA="$OUT/$name.list" linux/build.sh vmlinux) \
		>"$OUT/$name-kernel.log" 2>&1 || { tail -30 "$OUT/$name-kernel.log"; echo "kernel build failed" >&2; exit 2; }
	printf 'mount\n/bin/ffirun /test/list\necho FT-ALL-DONE\n' >"$name.in"
	echo "running klausscc (output: $OUT/$name.out)"
	local start; start=$(date +%s)
	"$KLAUSSCC" -i "$LINUX_BUILD/vmlinux" --emulate --max-instructions "${FT_MAX_INSN:-200000000000}" \
		--uart-in "$name.in" --uart-prompt '$ ' </dev/null >"$name.out" 2>&1 || true
	echo "emulator: $(( $(date +%s) - start )) s, $(grep -ao 'Emulator finished: [0-9]* instructions' "$name.out" || echo '?')"
	grep -q '^FT-ALL-DONE' "$name.out" && pass "$name-complete" || fail "$name-complete: the command list did not finish"
}

# The common part of the initramfs: the loader, libffi, ffirun.
common_list() {
	cat <<EOF
dir /lib 0755 0 0
dir /test 0755 0 0
file /lib/ld-musl-klausscpu.so.1 $OUT/ld-musl-klausscpu.so.1 0755 0 0
file /lib/libffi.so.8 $OUT/libffi.so.8 0755 0 0
file /bin/ffirun $OUT/ffirun 0755 0 0
file /test/list $OUT/list 0644 0 0
EOF
}
cp -L "$DYN_SYSROOT/lib/libc.so" ld-musl-klausscpu.so.1
cp -L "$FFI_LIB/libffi.so.8" libffi.so.8

# ---- host checks of libffi.so ----
"$RE" -d libffi.so.8 | grep -q TEXTREL && fail "host-libffi-no-textrel" || pass "host-libffi-no-textrel"
"$RE" -h libffi.so.8 | grep -qE 'Flags: +0x3$' && pass "host-libffi-abi3" || fail "host-libffi-abi3: $("$RE" -h libffi.so.8 | grep Flags)"
for s in ffi_call ffi_prep_cif ffi_prep_cif_var ffi_prep_closure_loc ffi_closure_alloc ffi_closure_free \
	ffi_type_complex_double ffi_get_struct_offsets; do
	"$RE" --dyn-syms -W libffi.so.8 | grep -qE " $s(@|$)" && pass "host-export-$s" || fail "host-export-$s"
done
"$RE" --dyn-syms -W libffi.so.8 | grep -qE ' ffi_call_go(@|$)' && fail "host-no-go-closures" || pass "host-no-go-closures"
# The trampoline template is 16 bytes and assembled as ffi.c expects
# (leapc.s r14,0; ldidx64 r13,r14,16; ldidx64 r14,r14,24; jmpr r13).
tramp=$("$DYN_BIN/llvm-objdump" -d libffi.so.8 |
	awk '/<ffi_klausscpu_trampoline>:/ {f=1; next} f && NF==0 {exit} f {for (i = 2; i <= 9; i++) printf "%s", $i}')
[ "$tramp" = 000ea04be02d305be03e305b0d008060 ] && pass "host-trampoline-code" || fail "host-trampoline-code: $tramp"

# ---- testsuite ----
if [[ " $MODES " == *" testsuite "* ]]; then
	TS=$FFI_SRC/testsuite
	TCC=("${CC[@]}" -fPIE -pie -W -Wall -Wno-unused-parameter -Wno-unused-variable -Wno-uninitialized
		-Wno-unused-but-set-variable -Wno-format -I"$FFI_INC" -I"$FFI_SRC/include" -I"$FFI_CONFIG"
		-L"$OUT" -lffi -lm)
	mkdir -p bin
	rm -f bin/* build.jobs unsupported.txt
	for d in libffi.call libffi.closures libffi.complex; do
		for t in "$TS/$d"/*.c; do
			b=$(basename "$t" .c)
			for o in $OPTS; do
				echo "$d/$b$o $t $o -I$TS/$d"
			done
		done
		for t in "$TS/$d"/*.cc; do
			[ -e "$t" ] && echo "$d/$(basename "$t") (C++: no C++ runtime in the sysroot)" >>unsupported.txt
		done
	done >build.jobs
	for t in "$TS"/libffi.go/*.c; do
		echo "libffi.go/$(basename "$t") (Go closures: not in the port)" >>unsupported.txt
	done
	for o in $OPTS; do
		for b in test-call test-callback; do
			echo "libffi.bhaible/$b$o $TS/libffi.bhaible/$b.c $o -I$TS/libffi.bhaible -Wno-uninitialized"
		done
		echo "klausscpu/abi$o $HERE/abi.c $o $HERE/abi_helpers.S"
	done >>build.jobs
	if [ -n "${FT_FILTER:-}" ]; then
		grep -E "^[^ ]*(${FT_FILTER})" build.jobs >build.jobs.f || true
		mv build.jobs.f build.jobs
	fi
	cp -L "$FFI_LIB/libffi.so.8" libffi.so	# for -lffi
	# build <name> <src> <opt> <flags...>: bin/<name with / -> __>, or
	# bin/<...>.compile-error with the compiler's messages.
	build_one() {
		local name=$1 src=$2; shift 2
		local exe=bin/${name//\//__}
		"${TCC[@]}" "$@" "$src" -o "$exe" >"$exe.log" 2>&1 || mv "$exe.log" "$exe.compile-error"
	}
	echo "building $(wc -l <build.jobs) test programs"
	while read -r name src rest; do
		# shellcheck disable=SC2086
		build_one "$name" "$src" $rest &
		while [ "$(jobs -rp | wc -l)" -ge "$(nproc)" ]; do wait -n || true; done
	done <build.jobs
	wait || true
	{
		while read -r name src rest; do
			exe=bin/${name//\//__}
			[ -x "$exe" ] && echo "$name /test/bin/$(basename "$exe")"
		done <build.jobs
	} >list
	{
		common_list
		echo "dir /test/bin 0755 0 0"
		for f in bin/*; do
			[ -x "$f" ] && [ -f "$f" ] && echo "file /test/bin/$(basename "$f") $OUT/$f 0755 0 0"
		done
	} >testsuite.list
	boot testsuite
	python3 -I "$HERE/check.py" testsuite.out build.jobs "$TS" "$HERE" bin unsupported.txt >check.txt || true
	grep -v '^PASS \|^SUMMARY' check.txt || true
	summary=$(grep '^SUMMARY' check.txt || echo "SUMMARY none")
	echo "$summary"
	# Every job must have run: pass + fail = the jobs built, and at least one
	# (an FT_FILTER matching nothing or an empty testsuite is not a pass).
	np=$(sed -n 's/.* pass=\([0-9]*\).*/\1/p' <<<"$summary")
	nf=$(sed -n 's/.* fail=\([0-9]*\).*/\1/p' <<<"$summary")
	nj=$(wc -l <build.jobs)
	if [ -n "$np" ] && [ "$np" -gt 0 ] && [ "$nf" = 0 ] && [ $((np + nf)) = "$nj" ]; then
		pass "testsuite: $summary"
	else
		fail "testsuite: $summary (jobs $nj)"
	fi
fi

# ---- Python ctypes ----
if [[ " $MODES " == *" ctypes "* ]]; then
	T=$BR_OUT/target
	PYV=$(cd "$T/usr/lib" && ls -d python3.* | head -1)
	[ -f "$T/usr/lib/$PYV/lib-dynload/_ctypes.cpython-${PYV#python}"*.so ] 2>/dev/null ||
		ls "$T/usr/lib/$PYV/lib-dynload/" | grep -q '^_ctypes\.' ||
		{ fail "ctypes-built: no _ctypes in $T/usr/lib/$PYV/lib-dynload"; echo "RESULT: $FAILS failing checks"; exit 1; }
	pass "ctypes-built"
	# _ctypes_test (CPython's own test helper library) is not installed
	# by Buildroot; the smoke test uses libc and libm only.
	# PYTHONHASHSEED: the emulated machine has no entropy, so getrandom
	# would block and Python's start-up fails (so not -I, which ignores it).
	echo "py PYTHONHASHSEED=0 /usr/bin/python3 -s -S /test/ctypes_smoke.py" >list
	{
		common_list
		echo "dir /usr 0755 0 0"
		echo "dir /usr/bin 0755 0 0"
		echo "dir /usr/lib 0755 0 0"
		echo "file /usr/bin/$PYV $T/usr/bin/$PYV 0755 0 0"
		echo "slink /usr/bin/python3 $PYV 0777 0 0"
		echo "file /test/ctypes_smoke.py $HERE/ctypes_smoke.py 0644 0 0"
		for l in "$T"/usr/lib/libpython3*.so*; do
			[ -e "$l" ] || continue
			if [ -L "$l" ]; then echo "slink /usr/lib/$(basename "$l") $(readlink "$l") 0777 0 0"
			else echo "file /usr/lib/$(basename "$l") $l 0755 0 0"; fi
		done
		# The standard library without what the test cannot need.
		(cd "$T/usr/lib" && find "$PYV" -path "$PYV/test" -prune -o -path "$PYV/ensurepip" -prune \
			-o -path "$PYV/idlelib" -prune -o -path "$PYV/tkinter" -prune -o -path "$PYV/turtledemo" -prune \
			-o -path "$PYV/config-*" -prune -o -path "$PYV/site-packages" -prune \
			-o -type d -print) | sort | while read -r d; do echo "dir /usr/lib/$d 0755 0 0"; done
		(cd "$T/usr/lib" && find "$PYV" -path "$PYV/test" -prune -o -path "$PYV/ensurepip" -prune \
			-o -path "$PYV/idlelib" -prune -o -path "$PYV/tkinter" -prune -o -path "$PYV/turtledemo" -prune \
			-o -path "$PYV/config-*" -prune -o -path "$PYV/site-packages" -prune \
			-o -type f -print) | sort | while read -r f; do echo "file /usr/lib/$f $T/usr/lib/$f 0644 0 0"; done
	} >ctypes.list
	boot ctypes
	O=ctypes.out
	sed -n '/^@@BEGIN py/,/^@@END py/p' $O | grep -E '^(PASS|FAIL|INFO)' || true
	n=$(sed -n '/^@@BEGIN py/,/^@@END py/p' $O | grep -c '^PASS ' || true)
	f=$(sed -n '/^@@BEGIN py/,/^@@END py/p' $O | grep -c '^FAIL ' || true)
	grep -q '^@@END py exit=0' $O && pass "ctypes-exit" || fail "ctypes-exit: $(grep '^@@END py' $O || echo 'no end line')"
	grep -q '^ALL-CTYPES-DONE' $O && pass "ctypes-script-done" || fail "ctypes-script-done"
	[ "$f" = 0 ] && [ "$n" -gt 0 ] && pass "ctypes-checks: $n pass" || fail "ctypes-checks: $n pass, $f fail"
fi
echo "RESULT: $FAILS failing checks"
[ "$FAILS" = 0 ]
