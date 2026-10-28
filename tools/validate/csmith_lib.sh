# shellcheck shell=bash
# csmith_lib.sh -- shared by csmith_run.sh and csmith_board.sh (source after
# common.sh).
#
# Csmith programs are checked against a host gcc -O0 build of the same source.
# A seed is discarded (status host-ub) when the host build under
# -fsanitize=undefined,address,float-cast-overflow traps or prints a different
# checksum: such programs have undefined behaviour the host can see, so any
# difference on KlaussCPU would mean nothing.  Packed structs are off
# (--no-packed-struct): they give misaligned accesses that the host cannot
# flag when its own layout happens to be aligned.
#
# Csmith's --float programs often pass a float * where an int32_t * is
# expected (128 of seeds 121300-121449; no integer-only seed in 121000-121149
# does).  Reading a float through an int32_t lvalue breaks C's aliasing rules
# and neither sanitizer sees it, and TBAA then legitimately changes the
# result: seed 121439 --float gives a different checksum at -O1/-O2/-Os, the
# same on host clang -O1 for x86, and the right one with
# -fno-strict-aliasing.  So a seed whose source has incompatible pointer
# types (gcc -Werror=incompatible-pointer-types) is built for KlaussCPU with
# -fno-strict-aliasing (SEED_CFLAGS; the seeds are listed in
# <outdir>/no-strict-aliasing.txt), and every other seed keeps TBAA.
#
#   CSMITH        csmith binary            (default: csmith on PATH)
#   CSMITH_INC    csmith runtime headers   (default /usr/include/csmith)
#   CSMITH_OPTS   extra csmith options     (e.g. --float for the FP runs)
#   PICOLIBC      C library install        (default: the one matching the
#                 toolchain's ABI, see picolibc_for_toolchain)

CSMITH=${CSMITH:-$(command -v csmith || true)}
[ -n "$CSMITH" ] || die "csmith not found (set CSMITH)"
CSI=${CSMITH_INC:-/usr/include/csmith}
[ -e "$CSI/csmith.h" ] || die "no csmith.h in $CSI (set CSMITH_INC)"
PICO=$(picolibc_for_toolchain)
[ -e "$PICO/lib/libc.a" ] || die "no libc.a in $PICO"
CRT_SRC=$LLVM_SRC/compiler-rt/lib/builtins
[ -e "$CRT_SRC/udivsi3.c" ] || die "no compiler-rt builtins sources in $CRT_SRC (set KLAUSSCPU_LLVM_SRC)"
CFLAGS="-target klausscpu-unknown-elf -isystem $PICO/include -nostdlib -fPIC -D__IEEE_LITTLE_ENDIAN -D_LDBL_EQ_DBL -w -Wno-error=incompatible-pointer-types"

# csmith_rt <outdir> -- build the runtime objects once per output dir with
# the toolchain under test (at -O1) and set RTOBJS.
csmith_rt() {
  local R=$1/rt s n crtflags
  crtflags="-target klausscpu-unknown-elf -O1 -nostdlibinc -ffreestanding -I$CRT_SRC -D__SOFTFP__ -w"
  mkdir -p "$R"
  if [ ! -e "$R/done" ]; then
    for s in crt0 uart_stubs syscalls stdio_handles; do
      "$LLVM_BIN/clang" $CFLAGS -O1 -I"$RT" -c "$RT/src/$s.c" -o "$R/$s.o" || die "runtime object $s"
    done
    "$LLVM_BIN/clang" $CFLAGS -O1 -c "$RT/src/setjmp.S" -o "$R/setjmp.o" || die "setjmp.S"
    "$LLVM_BIN/clang" $crtflags -c "$RT/src/fp_mode_stub.c" -o "$R/fp_mode_stub.o" || die "fp_mode_stub"
    for n in udivsi3 divsi3 udivdi3 divdi3 umoddi3 moddi3 udivmoddi4 udivti3 umodti3 udivmodti4 multi3 \
             addsf3 subsf3 mulsf3 divsf3 negsf2 comparesf2 floatsisf floatunsisf fixsfsi fixsfdi fixunssfsi \
             fixunssfdi adddf3 subdf3 muldf3 divdf3 negdf2 comparedf2 floatsidf floatunsidf fixdfsi fixdfdi \
             fixunsdfsi fixunsdfdi extendsfdf2 truncdfsf2 floatdidf floatundidf floatdisf floatundisf; do
      "$LLVM_BIN/clang" $crtflags -c "$CRT_SRC/$n.c" -o "$R/crt-$n.o" || die "compiler-rt $n"
    done
    touch "$R/done"
  fi
  RTOBJS="$R/fp_mode_stub.o $R/crt0.o $R/uart_stubs.o $R/syscalls.o $R/stdio_handles.o $R/setjmp.o $(echo "$R"/crt-*.o)"
}

# csmith_gen <seed> -- in the current directory: generate t.c, build and run
# the host reference.  Prints nothing and sets REF (and SEED_CFLAGS, see
# above) on success; otherwise
# prints the seed's status line (gen-fail, host-cc-fail, host-timeout,
# host-ub) and returns 1.
csmith_gen() {
  local seed=$1 san=""
  # shellcheck disable=SC2086
  "$CSMITH" --no-packed-struct ${CSMITH_OPTS:-} --seed "$seed" >t.c 2>/dev/null ||
    { printf '%s\t-\tgen-fail\t-\t-\n' "$seed"; return 1; }
  gcc -w -fpermissive -O0 -I"$CSI" t.c -o host 2>/dev/null ||
    { printf '%s\t-\thost-cc-fail\t-\t-\n' "$seed"; return 1; }
  SEED_CFLAGS=
  gcc -fsyntax-only -w -Werror=incompatible-pointer-types -I"$CSI" t.c 2>/dev/null ||
    { SEED_CFLAGS=-fno-strict-aliasing; echo "$seed" >>"$OUT/no-strict-aliasing.txt"; }
  REF=$(timeout 10 ./host | grep -o 'checksum = [0-9A-F]*' | cut -d' ' -f3)
  [ -n "$REF" ] || { printf '%s\t-\thost-timeout\t-\t-\n' "$seed"; return 1; }
  gcc -w -fpermissive -O0 -I"$CSI" -fsanitize=undefined,address,float-cast-overflow \
      -fno-sanitize-recover=all t.c -o hsan 2>/dev/null &&
    san=$(ASAN_OPTIONS=detect_leaks=0 timeout 60 ./hsan 2>/dev/null | grep -o 'checksum = [0-9A-F]*' | cut -d' ' -f3)
  [ "$san" = "$REF" ] || { printf '%s\t-\thost-ub\t%s\t%s\n' "$seed" "$REF" "$san"; return 1; }
  return 0
}

# csmith_build <opt> -- compile and link t.c into t<opt>.elf; on failure set
# ST to cc-fail or ld-fail and return 1.
csmith_build() {
  local O=$1
  # shellcheck disable=SC2086
  if ! timeout 300 "$LLVM_BIN/clang" $CFLAGS $O ${SEED_CFLAGS:-} -I"$CSI" -c t.c -o "t$O.o" 2>"cc$O.err"; then
    ST=cc-fail; return 1
  fi
  # shellcheck disable=SC2086
  if ! "$LLVM_BIN/ld.lld" -T "$RT/klausscpu.ld" --gc-sections -o "t$O.elf" $RTOBJS "t$O.o" "$PICO/lib/libc.a" 2>"ld$O.err"; then
    ST=ld-fail; return 1
  fi
  return 0
}

checksum_of() { grep -ao 'checksum = [0-9A-F]*' "$1" | head -1 | cut -d' ' -f3; }

# csmith_classify <emulator-capture> -- sets G (checksum, or -), NI
# (instructions, or -) and ST from how the run ended:
#   ok / MISMATCH / no-output   halted; checksum equal to REF / different / none
#   instr-cap                   ran past the instruction cap (no verdict yet)
#   stop-<reason>               any other end (Trap, InvalidOpcode, ...; "none"
#                               when the emulator was killed by its timeout):
#                               always a failure, even after a good checksum
csmith_classify() {
  local s
  G=$(checksum_of "$1"); G=${G:--}
  NI=$(emu_instructions "$1"); NI=${NI:--}
  s=$(stop_of "$1")
  case $s in
    Halt) if [ "$G" = - ]; then ST=no-output; elif [ "$G" = "$REF" ]; then ST=ok; else ST=MISMATCH; fi ;;
    InstructionCap) ST=instr-cap ;;
    *) ST=stop-$s ;;
  esac
}

# csmith_recheck <opt> -- t<opt>.elf ran past the instruction cap while
# another level of the same seed halted with the right checksum.  That is
# usually a slow -O0 build of a loop the optimiser removed (seed 120376
# --float: 103K instructions at -O1, 10.4G at -O0), but it could be a
# miscompiled infinite loop, so the level is run again with a much larger cap
# (EMU_RETRY_MAX, default 2e10, about 2 minutes; EMU_RETRY_TIMEOUT seconds,
# default 1800).  Sets G, NI and ST as csmith_classify does, except that a
# good result is ok-long and a run that still does not halt is HANG (a
# failure).
csmith_recheck() {
  local O=$1
  emu "${EMU_RETRY_MAX:-20000000000}" "t$O.elf" "emu$O.long.out" "${EMU_RETRY_TIMEOUT:-1800}"
  csmith_classify "emu$O.long.out"
  case $ST in ok) ST=ok-long ;; instr-cap|stop-none) ST=HANG ;; esac
}
