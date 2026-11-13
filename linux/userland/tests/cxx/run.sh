#!/usr/bin/env bash
# linux/userland/tests/cxx/run.sh — the C++ runtime (libunwind, libc++abi,
# libc++; CLANG_ON_BOARD_PLAN.md M2) on the klausscc emulator.
#
# Builds this directory's tests with plain clang++ against the sysroot that
# linux/cxx/build.sh filled, at several optimisation levels, static and as
# PIEs with the shared libraries, puts them into a minish initramfs, boots it
# on klausscc and runs them with libffi's ffirun (one boot), then checks each
# program's exit status and output on the host.  Exit 0 only if every check
# passes.
#   eh.cpp + eh_lib.cpp  exceptions (eh_lib a shared library in the PIEs)
#   stdlib.cpp           containers, strings, threads, iostreams, filesystem
#   terminate.cpp        std::terminate: noexcept, and an uncaught exception
#
#   DYN_BIN, DYN_SYSROOT, KLAUSSCPU_LLVM_BIN, KLAUSSCC as in ../dynlink/run.sh
#   LINUX_BUILD   kernel build directory (default linux-ws/build-cxx)
#   CT_OUT        build/output directory (default linux-ws/cxx-test)
#   CT_OPTS       optimisation levels (default "-O0 -O2 -Oz")
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../../.." && pwd)
SRC=$(cd "$RT/.." && pwd)
DYN_BIN=${DYN_BIN:-$SRC/klausscpu-llvm/build/bin}
DYN_SYSROOT=${DYN_SYSROOT:-$RT/linux-ws/dyn/sysroot}
export KLAUSSCPU_LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$SRC/klausscpu-llvm/build/bin}
export LINUX_BUILD=${LINUX_BUILD:-$RT/linux-ws/build-cxx}
OUT=${CT_OUT:-$RT/linux-ws/cxx-test}
KLAUSSCC=${KLAUSSCC:-$SRC/klausscc/target/release/klausscc}
OPTS=${CT_OPTS:--O0 -O2 -Oz}

for f in "$DYN_SYSROOT/lib/libc.so" "$DYN_SYSROOT/lib/libc++.so.1" "$DYN_SYSROOT/lib/libunwind.a" \
	"$DYN_SYSROOT/include/c++/v1/vector"; do
	[ -e "$f" ] || { echo "missing $f (linux/cxx/build.sh)" >&2; exit 2; }
done
mkdir -p "$OUT/bin"
cd "$OUT"
rm -f bin/* list
FAILS=0
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }
CXX=("$DYN_BIN/clang++" --target=klausscpu-unknown-linux-musl --sysroot="$DYN_SYSROOT" -Wall -I"$HERE")
"$DYN_BIN/clang" --target=klausscpu-unknown-linux-musl --sysroot="$DYN_SYSROOT" -static -O2 -Wall \
	"$HERE/../libffi/ffirun.c" -o ffirun

# ---- build ----
# name|kind|flags: kind static = one static program; pie = a PIE linked to
# libehlib<opt>.so (eh) or to the shared C++ libraries (others).
JOBS=()
for o in $OPTS; do
	JOBS+=("eh-static$o|eh-static|$o" "eh-pie$o|eh-pie|$o")
done
JOBS+=("eh-noenter-O2|eh-static|-O2 -mllvm -klausscpu-enter-leave=false"
	"eh-noshrink-O2|eh-static|-O2 -mllvm -klausscpu-shrink-wrap=false -mno-omit-leaf-frame-pointer"
	"stdlib-static-O2|stdlib-static|-O2" "stdlib-static-O0|stdlib-static|-O0" "stdlib-pie-O2|stdlib-pie|-O2"
	"terminate-O2|terminate|-O2")
build() {
	local name=$1 kind=$2 flags=$3 lib
	case $kind in
	eh-static) "${CXX[@]}" $flags "$HERE/eh.cpp" "$HERE/eh_lib.cpp" -o "bin/$name" ;;
	eh-pie)
		lib=libehlib${flags// /}.so
		"${CXX[@]}" $flags -fPIC -shared -Wl,-soname,"$lib" "$HERE/eh_lib.cpp" -o "bin/$lib" &&
			"${CXX[@]}" $flags -fPIE -pie "$HERE/eh.cpp" -Lbin -l:"$lib" -o "bin/$name" ;;
	stdlib-static) "${CXX[@]}" -std=c++23 $flags "$HERE/stdlib.cpp" -o "bin/$name" ;;
	stdlib-pie) "${CXX[@]}" -std=c++23 $flags -fPIE -pie "$HERE/stdlib.cpp" -o "bin/$name" ;;
	terminate) "${CXX[@]}" $flags "$HERE/terminate.cpp" -o "bin/$name" ;;
	esac
}
for j in "${JOBS[@]}"; do
	IFS='|' read -r name kind flags <<<"$j"
	( build "$name" "$kind" "$flags" >"bin/$name.log" 2>&1 && pass "build-$name" ||
		{ echo "FAIL build-$name"; sed 's/^/  /' "bin/$name.log" | head -20; } ) &
	while [ "$(jobs -rp | wc -l)" -ge "$(nproc)" ]; do wait -n || true; done
done
wait || true
FAILS=$((FAILS + $(grep -c . <(for j in "${JOBS[@]}"; do IFS='|' read -r n _ <<<"$j"; [ -x "bin/$n" ] || echo "$n"; done) || true)))

for j in "${JOBS[@]}"; do
	IFS='|' read -r name kind _ <<<"$j"
	[ -x "bin/$name" ] || continue
	case $kind in
	terminate)
		echo "$name-noexcept /test/bin/$name noexcept"
		echo "$name-uncaught /test/bin/$name uncaught" ;;
	*) echo "$name /test/bin/$name" ;;
	esac
done >list

# ---- initramfs ----
{
	cat <<EOF
dir /lib 0755 0 0
dir /tmp 1777 0 0
dir /test 0755 0 0
dir /test/bin 0755 0 0
file /bin/ffirun $OUT/ffirun 0755 0 0
file /test/list $OUT/list 0644 0 0
EOF
	echo "file /lib/ld-musl-klausscpu.so.1 $(realpath "$DYN_SYSROOT/lib/libc.so") 0755 0 0"
	for l in libc++.so.1 libc++abi.so.1 libunwind.so.1; do
		echo "file /lib/$l $(realpath "$DYN_SYSROOT/lib/$l") 0755 0 0"
	done
	for f in bin/*; do
		case $f in *.log) continue ;; esac
		case $f in
		*.so) echo "file /lib/$(basename "$f") $OUT/$f 0755 0 0" ;;
		*) echo "file /test/bin/$(basename "$f") $OUT/$f 0755 0 0" ;;
		esac
	done
} >cxx.list

# ---- kernel and emulator ----
echo "building the kernel in $LINUX_BUILD (log: $OUT/kernel.log)"
(cd "$RT" && INITRAMFS_PROFILE=minish INITRAMFS_EXTRA="$OUT/cxx.list" linux/build.sh defconfig &&
	INITRAMFS_PROFILE=minish INITRAMFS_EXTRA="$OUT/cxx.list" linux/build.sh vmlinux) \
	>"$OUT/kernel.log" 2>&1 || { tail -30 "$OUT/kernel.log"; echo "kernel build failed" >&2; exit 2; }
printf 'mount\n/bin/ffirun /test/list\necho CT-ALL-DONE\n' >cmds.in
echo "running klausscc (output: $OUT/emu.out)"
start=$(date +%s)
"$KLAUSSCC" -i "$LINUX_BUILD/vmlinux" --emulate --max-instructions "${CT_MAX_INSN:-400000000000}" \
	--uart-in cmds.in --uart-prompt '$ ' </dev/null >emu.out 2>&1 || true
echo "emulator: $(( $(date +%s) - start )) s, $(grep -ao 'Emulator finished: [0-9]* instructions' emu.out || echo '?')"

# ---- check ----
tr -d '\r' <emu.out >emu.txt	# the UART's CRLF
FAILS=$FAILS "$HERE/check.sh" emu.txt list
