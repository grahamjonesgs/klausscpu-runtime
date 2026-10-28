#!/usr/bin/env bash
# cfi_run.sh csmith <first-seed> <count> <outdir> [level ...]
# cfi_run.sh corpus <outdir> [level ...]
# cfi_run.sh selftest <outdir>
#
# Unwind info must describe every instruction: build programs with -g and
# replay each one's emulator trace against a shadow call stack
# (cfi_check.py): at every PC in a function with an FDE, the CFA, return
# address, R15 and R4-R7 the .debug_frame row recovers must be the caller's.
#
#   csmith  each Csmith program (the csmith_run.sh screen: a seed whose host
#           build shows UB is discarded) at each level (default -O2 -O3 and
#           -O3 without ENTER/LEAVE, whose epilogue is SETSP R15; POP R15; a
#           level's extra flags are joined by '+'), plus -g
#   corpus  the bare-metal corpus (emu_suite.sh's `make all`) at each level
#           (default -O3) plus -g, each run for CFI_MAX instructions
#           (default 20M) -- the long and idle programs are only partly
#           checked
#   selftest  the checker itself: a hand-written function whose CFI is right
#           must pass, with a LEAVERET epilogue and with SETSP R15; POP R15,
#           and three copies with one fault each (a callee-saved spill
#           without its rule, a wrong CFA offset, a callee-saved rule kept
#           after SETSP R15 left its slot below SP) must each be flagged
#
# Writes <outdir>/results.tsv (seed or program, level, ok|BAD|check-fail|
# cc-fail|ld-fail, instructions, PCs checked) and keeps the source, ELF and
# checker report of every seed that is not ok in <outdir>/fail/.  Prints the
# counts; exits 1 if anything is BAD, fails to build, or could not be
# checked (check-fail: the checker crashed or was killed, the emulator
# stopped other than by halting or reaching its cap, or no PC was checked).
set -uo pipefail
. "$(dirname "$0")/common.sh"
CHECK=$VALIDATE_DIR/cfi_check.py
MODE=${1:-}; shift || true
summary() {
  awk -F'\t' '{c[$3]++; n+=$5} END {for (k in c) printf "%s %d, ", k, c[k]; printf "%d PCs checked\n", n}' "$OUT/results.tsv"
  ! grep -qP '\t(BAD|check-fail|cc-fail|ld-fail|build-fail)\t' "$OUT/results.tsv"
}
# check <elf> <name> <level> <cap>: one results line; the report in <name><level>.cfi
check() {
  local r=$OUT/reports/$2$3.cfi st
  python3 "$CHECK" --quiet --show 2 --max-instructions "$4" "$1" >"$r" 2>&1
  case $? in 0) st=ok ;; 1) st=BAD ;; *) st=check-fail ;; esac
  printf '%s\t%s\t%s\t%s\t%s\n' "$2" "$3" "$st" \
    "$(sed -n 's/.*: \([0-9]*\) instructions.*/\1/p' "$r")" \
    "$(sed -n 's/.* \([0-9]*\) checked .*/\1/p' "$r")"
}
case $MODE in
csmith)
  . "$VALIDATE_DIR/csmith_lib.sh"
  [ $# -ge 3 ] || die "usage: cfi_run.sh csmith <first-seed> <count> <outdir> [level ...]"
  FIRST=$1; COUNT=$2; OUT=$(realpath -m "$3"); shift 3
  LEVELS=${*:-"-O2 -O3 -O3+-mllvm+-klausscpu-enter-leave=false"}
  mkdir -p "$OUT/work" "$OUT/fail" "$OUT/reports"
  csmith_rt "$OUT"
  one() {
    local seed=$1 W=$OUT/work/$1 L line
    mkdir -p "$W"; cd "$W" || return
    if csmith_gen "$seed" >/dev/null; then
      for L in $LEVELS; do
        if csmith_build "$L+-g"; then
          line=$(check "t$L+-g.elf" "$seed" "$L" "${EMU_MAX_INSTRUCTIONS:-50000000}")
        else
          line=$(printf '%s\t%s\t%s\t-\t0' "$seed" "$L" "$ST")
        fi
        echo "$line"
        case $line in *$'\t'ok$'\t'*) ;; *)
          mkdir -p "$OUT/fail/$seed$L"
          cp t.c "t$L+-g.elf" ./*.err "$OUT/reports/$seed$L.cfi" "$OUT/fail/$seed$L/" 2>/dev/null ;;
        esac
      done
    fi
    cd "$OUT" && rm -rf "$W"
  }
  export -f one check csmith_gen csmith_build
  export OUT LEVELS CSMITH CSI CFLAGS LLVM_BIN RTOBJS PICO RT CHECK CSMITH_OPTS
  seq "$FIRST" $((FIRST + COUNT - 1)) | xargs -P "$JOBS" -I{} bash -c 'one {}' >>"$OUT/results.tsv"
  rm -rf "$OUT/work"
  summary
  ;;
corpus)
  [ $# -ge 1 ] || die "usage: cfi_run.sh corpus <outdir> [level ...]"
  OUT=$(realpath -m "$1"); shift
  LEVELS=${*:-"-O3"}
  mkdir -p "$OUT/reports"
  : >"$OUT/results.tsv"
  for L in $LEVELS; do
    D=$OUT/build$L
    rm -rf "$D"; mkdir -p "$D/elf"
    stage_baremetal "$D"
    make -C "$STAGED_BM" -j"$JOBS" -k all OPT="${L//+/ } -g" ${PICOLIBC:+PICOLIBC=$PICOLIBC} >"$D/build.log" 2>&1
    cp "$STAGED_BM"/*.elf "$D/elf/" 2>/dev/null
    rm -rf "$D/tree"
    export -f check; export OUT CHECK LLVM_BIN KLAUSSCC L D
    # shellcheck disable=SC2016
    ls "$D/elf" | sed -n 's/\.elf$//p' |
      xargs -P "$JOBS" -I{} bash -c 'check "$D/elf/{}.elf" {} "$L" "${CFI_MAX:-20000000}"' >>"$OUT/results.tsv"
    grep -q 'error:' "$D/build.log" && printf 'make\t%s\tbuild-fail\t-\t0\n' "$L" >>"$OUT/results.tsv"
  done
  summary
  ;;
selftest)
  . "$VALIDATE_DIR/csmith_lib.sh"
  [ $# -ge 1 ] || die "usage: cfi_run.sh selftest <outdir>"
  OUT=$(realpath -m "$1")
  mkdir -p "$OUT/reports"
  : >"$OUT/results.tsv"
  csmith_rt "$OUT"
  cat >"$OUT/main.c" <<'EOF2'
long cfi_f(long);
int printf(const char *, ...);
int main(void) {
  register long keep __asm__("r4") = 0x5a5a1234;
  __asm__ volatile("" : "+r"(keep));
  long r = cfi_f(3);
  __asm__ volatile("" : "+r"(keep));
  printf("r = %ld %lx\n", r, keep);
  return 0;
}
EOF2
  # cfi_f saves R4, overwrites it and calls a frameless leaf; FAULT picks
  # what is wrong with its CFI (none and split: nothing).
  epilogue() {
    case $1 in
      split) printf '\tsetsp r15\n\t.cfi_def_cfa_register sp\n\t.cfi_same_value r4\n\tpop r15\n\t.cfi_def_cfa_offset 8\n\t.cfi_same_value r15\n\tret\n' ;;
      late) printf '\tsetsp r15\n\t.cfi_def_cfa_register sp\n\tpop r15\n\t.cfi_def_cfa_offset 8\n\t.cfi_same_value r15\n\t.cfi_same_value r4\n\tret\n' ;;
      *) printf '\tleaveret\n' ;;
    esac
  }
  fn() {
    cat <<EOF2
	.cfi_sections .debug_frame
	.text
	.globl cfi_f
	.p2align 2
	.type cfi_f,@function
cfi_f:
	.cfi_startproc
	enter 1
	.cfi_def_cfa r15, $([ "$1" = cfa ] && echo 24 || echo 16)
	.cfi_offset r15, -16
	stidx64 r4, r15, -8
$([ "$1" = rule ] || echo "	.cfi_offset r4, -24")
	setr r4, 7
	callrel cfi_leaf
	addr r12, r12, r4
	ldidx64 r4, r15, -8
$(epilogue "$1")
	.cfi_endproc
	.p2align 2
	.type cfi_leaf,@function
cfi_leaf:
	.cfi_startproc
	addi r12, r0, 1
	ret
	.cfi_endproc
EOF2
  }
  "$LLVM_BIN/clang" $CFLAGS -O1 -g -c "$OUT/main.c" -o "$OUT/main.o" || die "selftest main.c"
  for F in none split rule cfa late; do
    fn $F >"$OUT/f-$F.s"
    "$LLVM_BIN/clang" -target klausscpu-unknown-elf -g -c "$OUT/f-$F.s" -o "$OUT/f-$F.o" &&
      "$LLVM_BIN/ld.lld" -T "$RT/klausscpu.ld" --gc-sections -o "$OUT/f-$F.elf" $RTOBJS \
        "$OUT/main.o" "$OUT/f-$F.o" "$PICO/lib/libc.a" || die "selftest build ($F)"
    line=$(check "$OUT/f-$F.elf" "fault-$F" -g 1000000)
    case $F:$line in
      none:*$'\t'ok$'\t'*|split:*$'\t'ok$'\t'*) echo "$line" ;;
      rule:*$'\t'BAD$'\t'*|cfa:*$'\t'BAD$'\t'*|late:*$'\t'BAD$'\t'*) echo "$line" ;;
      *) printf 'fault-%s\t-g\tMISSED\t-\t0\n' "$F" ;;
    esac
  done >>"$OUT/results.tsv"
  awk -F'\t' '{c[$3]++} END {for (k in c) printf "%s %d, ", k, c[k]; printf "checker self-test\n"}' "$OUT/results.tsv"
  ! grep -qP '\t(MISSED|check-fail)\t' "$OUT/results.tsv"
  ;;
*) die "usage: cfi_run.sh csmith <first> <count> <outdir> [level ...] | corpus <outdir> [level ...] | selftest <outdir>" ;;
esac
