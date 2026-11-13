#!/usr/bin/env bash
# linux/userland/tests/cxx/upstream.sh — LLVM's own libunwind and libc++abi test
# suites (the *.pass.cpp programs; CLANG_ON_BOARD_PLAN.md M2 "upstream suites")
# on the klausscc emulator.
#
# upstream_plan.py evaluates each test's lit REQUIRES/UNSUPPORTED/
# ADDITIONAL_COMPILE_FLAGS lines for klausscpu-unknown-linux-musl (C++23, or
# C++14 for tests that need an old dialect); excluded tests are UNSUPPORTED
# with the lit line as reason.  The rest are compiled static (clang++ against
# the sysroot, include paths as the libc++abi lit config: libcxx/test/support,
# libcxx/src), run through libffi's ffirun in batches (one boot per batch;
# the initramfs lives in the kernel's ~121 MB RAM), and reported as
#   PASS / FAIL / UNSUPPORTED / COMPILE-FAIL
# with a summary and the output of every failure.  Exit 0 if nothing FAILs or
# COMPILE-FAILs.
#   UP_OPTS      optimisation levels (default "-O2"; e.g. "-O0 -O2")
#   UP_FILTER    egrep on test names (default: all)
#   UP_BATCH_MB  initramfs payload per boot (default 45)
#   LINUX_BUILD  kernel build dir (default linux-ws/build-cxx-upstream)
#   UP_OUT       output dir (default linux-ws/cxx-upstream)
#   UP_TIMEOUT   per-program timeout in seconds (default 600: the threaded guard test needs more than 120 s on the emulator)
#   UP_NO_RUN=1  compile only
#   UP_REUSE=1   keep bin/ and build.txt from an earlier run (no recompilation)
#   DYN_BIN, DYN_SYSROOT, KLAUSSCPU_LLVM_BIN, KLAUSSCC as in run.sh
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
RT=$(cd "$HERE/../../../.." && pwd)
SRC=$(cd "$RT/.." && pwd)
DYN_BIN=${DYN_BIN:-$SRC/klausscpu-llvm/build/bin}
DYN_SYSROOT=${DYN_SYSROOT:-$RT/linux-ws/dyn/sysroot}
LLVM=${LLVM_SRC:-$SRC/klausscpu-llvm}
export KLAUSSCPU_LLVM_BIN=${KLAUSSCPU_LLVM_BIN:-$DYN_BIN}
export LINUX_BUILD=${LINUX_BUILD:-$RT/linux-ws/build-cxx-upstream}
OUT=${UP_OUT:-$RT/linux-ws/cxx-upstream}
KLAUSSCC=${KLAUSSCC:-$SRC/klausscc/target/release/klausscc}
OPTS=${UP_OPTS:--O2}
FILTER=${UP_FILTER:-.}
BATCH=$(( ${UP_BATCH_MB:-45} * 1048576 ))
mkdir -p "$OUT/bin"
cd "$OUT"
if [ -n "${UP_REUSE:-}" ]; then rm -rf batch.* results.txt fail-*.txt; else rm -rf bin/* batch.* results.txt build.txt fail-*.txt; fi
: >results.txt
CXX=("$DYN_BIN/clang++" --target=klausscpu-unknown-linux-musl --sysroot="$DYN_SYSROOT" -Wno-unknown-attributes
	-I"$LLVM/libcxx/test/support" -I"$LLVM/libcxx/src" -D_LIBCPP_ENABLE_CXX17_REMOVED_UNEXPECTED_FUNCTIONS
	-funwind-tables -Wl,--export-dynamic -pthread)
CC=("$DYN_BIN/clang" --target=klausscpu-unknown-linux-musl --sysroot="$DYN_SYSROOT" -static -O2)
sed "s/^#define TIMEOUT .*/#define TIMEOUT ${UP_TIMEOUT:-600}/" "$HERE/../libffi/ffirun.c" >ffirun.c
"${CC[@]}" ffirun.c -o ffirun || { echo "cannot build ffirun" >&2; exit 2; }

python3 "$HERE/upstream_plan.py" "$LLVM" | grep -E "^(RUN|SKIP)	[^	]*(${FILTER//\//.})" >plan.txt || true
# ---- build ----
build_one() { # name path flags srcname
	local name=$1 path=$2 flags=$3 tool=("${CXX[@]}") std=
	case $path in *.c) tool=("$DYN_BIN/clang" --target=klausscpu-unknown-linux-musl --sysroot="$DYN_SYSROOT"
		-I"$LLVM/libcxx/test/support" -funwind-tables -Wl,--export-dynamic -pthread) ;; esac
	local extra=
	case $path in *.compile.pass.*) extra="-c" ;; esac
	# $flags unquoted on purpose (word splitting)
	if [ "$extra" = -c ]; then
		"${tool[@]}" $flags $OPT -c "$path" -o "bin/$name.o" >"bin/$name.log" 2>&1 && rm -f "bin/$name.o"
	else
		"${tool[@]}" $flags $OPT -static "$path" -o "bin/$name" >"bin/$name.log" 2>&1
	fi
}
for OPT in $OPTS; do
	[ -z "${UP_REUSE:-}" ] || break
	export OPT
	while IFS=$'\t' read -r kind tname path rest; do
		[ "$kind" = RUN ] || continue
		name=$(echo "${tname%.pass.*}$OPT" | tr '/.' '--')
		( OPT=$OPT build_one "$name" "$path" "$rest" && echo "BUILT	$name	$tname$OPT	$path" ||
			echo "CFAIL	$name	$tname$OPT	$path	$(grep -m1 -E 'error|undefined|Assertion|PLEASE submit|Stack dump' "bin/$name.log" | cut -c1-200)" ) >>build.txt &
		while [ "$(jobs -rp | wc -l)" -ge "$(nproc)" ]; do wait -n || true; done
	done <plan.txt
done
wait
awk -F'\t' '$1=="SKIP"{printf "UNSUPPORTED\t%s\t%s\n",$2,$4}' plan.txt >>results.txt
awk -F'\t' '$1=="CFAIL"{printf "COMPILE-FAIL\t%s\t%s\n",$3,$5}' build.txt | sort >>results.txt
echo "built: $(grep -c ^BUILT build.txt), compile failures: $(grep -c ^CFAIL build.txt)"

# ---- batches ----
[ -z "${UP_NO_RUN:-}" ] && {
	n=0; size=0; : >batch.0.list
	flush() {
		[ -s "batch.$n.list" ] || return 0
		run_batch "$n"
		n=$((n + 1)); size=0; : >"batch.$n.list"
	}
	run_batch() {
		local b=$1 l=batch.$1.list name tname path
		{
			cat <<EOT
dir /tmp 1777 0 0
dir /test 0755 0 0
dir /test/bin 0755 0 0
file /bin/ffirun $OUT/ffirun 0755 0 0
file /test/list $OUT/batch.$b.run 0644 0 0
EOT
			while IFS=$'\t' read -r name tname path; do
				echo "file /test/bin/$name $OUT/bin/$name 0755 0 0"
			done <"$l"
		} >batch.$b.cpio
		while IFS=$'\t' read -r name tname path; do echo "$name /test/bin/$name"; done <"$l" >batch.$b.run
		echo "batch $b: $(wc -l <"$l") programs; building the kernel (log: $OUT/kernel.$b.log)"
		(cd "$RT" && INITRAMFS_PROFILE=minish INITRAMFS_EXTRA="$OUT/batch.$b.cpio" linux/build.sh defconfig &&
			INITRAMFS_PROFILE=minish INITRAMFS_EXTRA="$OUT/batch.$b.cpio" linux/build.sh vmlinux) \
			>"$OUT/kernel.$b.log" 2>&1 </dev/null || { tail -20 "$OUT/kernel.$b.log"; echo "kernel build failed" >&2; return 1; }
		printf 'mount\n/bin/ffirun /test/list\necho CT-ALL-DONE\n' >batch.$b.in
		"$KLAUSSCC" -i "$LINUX_BUILD/vmlinux" --emulate --max-instructions "${UP_MAX_INSN:-600000000000}" \
			--uart-in batch.$b.in --uart-prompt '$ ' </dev/null >batch.$b.out 2>&1 || true
		tr -d '\r' <batch.$b.out >batch.$b.txt
		grep -q '^CT-ALL-DONE' batch.$b.txt || echo "WARNING: batch $b did not finish"
	}
	grep ^BUILT build.txt | sort -t$'\t' -k3 | while IFS=$'\t' read -r _ name tname path; do
		[ -x "bin/$name" ] && echo "$name	$tname	$path	$(stat -c %s "bin/$name")"
	done >built.txt
	while IFS=$'\t' read -r name tname path sz; do
		if [ $((size + sz)) -gt "$BATCH" ] && [ -s "batch.$n.list" ]; then flush; fi
		printf '%s\t%s\t%s\n' "$name" "$tname" "$path" >>"batch.$n.list"
		size=$((size + sz))
	done <built.txt
	flush
	# ---- results ----
	for lst in batch.*.list; do
		b=${lst#batch.}; b=${b%.list}
		while IFS=$'\t' read -r name tname path; do
			[ -n "$name" ] || continue
			sec=$(sed -n "/^@@BEGIN $name\$/,/^@@END $name /p" "batch.$b.txt")
			end=$(grep "^@@END $name " <<<"$sec" || true)
			if grep -q 'exit=0$' <<<"$end"; then printf 'PASS\t%s\n' "$tname" >>results.txt
			else printf 'FAIL\t%s\t%s\n' "$tname" "${end:-no @@END (hang/timeout/emulator limit)}" >>results.txt
				echo "$sec" >"fail-$name.txt"; fi
		done <"$lst"
	done
}
# compile-only tests count as PASS when they built
while IFS=$'\t' read -r _ name tname path; do
	case $path in *.compile.pass.*) printf 'PASS\t%s (compile only)\n' "$tname" >>results.txt ;; esac
done < <(grep ^BUILT build.txt)
sort -o results.txt results.txt
echo "==== results ($OUT/results.txt) ===="
cat results.txt
for k in PASS FAIL UNSUPPORTED COMPILE-FAIL; do echo "$k: $(grep -c "^$k" results.txt)"; done
for f in fail-*.txt; do [ -e "$f" ] && { echo "---- $f"; head -40 "$f"; }; done
! grep -qE '^(FAIL|COMPILE-FAIL)' results.txt
