#!/usr/bin/env bash
# oracle.sh <outdir>
#
# Snapshot the toolchain's output bytes, to prove that a change which should
# not alter generated code (a refactor, a TableGen migration, a docs change)
# really does not:
#   corpus-O1/-O2/-Os  every bare-metal .o/.elf (.comment stripped) and an
#                      llvm-objdump -d of every .elf
#   mc/                llvm-mc objects, disassembly and -show-encoding of the
#                      ISA coverage inputs (oracle-inputs/) and of
#                      llvm/test/MC/KlaussCPU/*.s, with short forms on and off
#                      (far.s, a branch/call over 140000 NOPs, is generated)
#                      plus llvm-mc --disassemble of the Disassembler tests
#   llc/               llc -O0/-O1/-O2 of every llvm/test/CodeGen/KlaussCPU/*.ll
# Compare two snapshots with oracle_diff.sh.  Needs the klausscpu-llvm source
# tree (KLAUSSCPU_LLVM_SRC, default $KLAUSSCPU_LLVM_BIN/../..).
set -uo pipefail
. "$(dirname "$0")/common.sh"
[ $# -eq 1 ] || die "usage: oracle.sh <outdir>"
OUT=$(realpath -m "$1"); rm -rf "$OUT"; mkdir -p "$OUT"
B=$LLVM_BIN
fail=0   # a corpus build or write that failed (e.g. a full disk): exit 1
[ -d "$LLVM_SRC/llvm/test/CodeGen/KlaussCPU" ] || die "no klausscpu-llvm tests under $LLVM_SRC"
for OPT in -O1 -O2 -Os; do
  D=$OUT/corpus$OPT; mkdir -p "$D"
  stage_baremetal "$OUT/stage"
  make -C "$STAGED_BM" -j"$JOBS" -k all OPT="$OPT" >"$D/build.log" 2>&1 ||
    { note "oracle: the $OPT corpus build failed (see $D/build.log)"; fail=1; }
  for f in "$STAGED_BM"/*.o "$STAGED_BM"/*.elf; do
    [ -e "$f" ] || continue
    "$B/llvm-objcopy" --remove-section=.comment "$f" "$D/$(basename "$f")" 2>>"$D/objcopy.err" || fail=1
  done
  for f in "$STAGED_BM"/*.elf; do "$B/llvm-objdump" -d "$f" >"$D/$(basename "$f" .elf).dis" 2>&1 || fail=1; done
  # objdump prints the file name it was given: make it relative.
  sed -i "s|$STAGED_BM/||" "$D"/*.dis || fail=1
done
rm -rf "$OUT/stage"
M=$OUT/mc; mkdir -p "$M"
G=$OUT/gen; mkdir -p "$G"
{ printf '.text\n.globl _start\n_start:\n  jmpnz far\n  jmpo far\n  jmpz near\nnear:\n'
  yes '  nop' | head -140001
  printf 'far:\n  ret\n  callrel _start\n  call far\n  leapc r1, far\n'; } >"$G/far.s"
for f in "$VALIDATE_DIR"/oracle-inputs/*.s "$G/far.s" "$LLVM_SRC"/llvm/test/MC/KlaussCPU/*.s; do
  [ -e "$f" ] || continue
  n=$(basename "$f" .s)
  for sf in true false; do
    (cd "$(dirname "$f")" && "$B/llvm-mc" -triple=klausscpu-unknown-elf -klausscpu-short-forms=$sf \
        -filetype=obj "$n.s" -o "$M/$n.$sf.o") >"$M/$n.$sf.obj.log" 2>&1
    # objdump runs in $M so that its header names the file without the outdir.
    [ -e "$M/$n.$sf.o" ] && "$B/llvm-objcopy" --remove-section=.comment "$M/$n.$sf.o" &&
      (cd "$M" && "$B/llvm-objdump" -dr "$n.$sf.o") >"$M/$n.$sf.dis" 2>&1
    # Run in the input's directory so diagnostics name the file without a path.
    (cd "$(dirname "$f")" && "$B/llvm-mc" -triple=klausscpu-unknown-elf -klausscpu-short-forms=$sf \
        -show-encoding "$n.s") >"$M/$n.$sf.enc" 2>&1
  done
done
rm -rf "$G"
for f in "$LLVM_SRC"/llvm/test/MC/Disassembler/KlaussCPU/*.txt; do
  (cd "$(dirname "$f")" && "$B/llvm-mc" -triple=klausscpu --disassemble "$(basename "$f")") >"$M/dis.$(basename "$f").out" 2>&1
done
C=$OUT/llc; mkdir -p "$C"
for f in "$LLVM_SRC"/llvm/test/CodeGen/KlaussCPU/*.ll; do
  for O in 0 1 2; do "$B/llc" -mtriple=klausscpu -O$O "$f" -o "$C/$(basename "$f" .ll).O$O.s" 2>/dev/null; done
done
echo "toolchain $(toolchain_id)" >"$OUT/toolchain.log" || fail=1
if [ $fail != 0 ]; then echo "oracle: INCOMPLETE snapshot in $OUT (see the messages above)"; exit 1; fi
echo "oracle written to $OUT ($(find "$OUT" -type f | wc -l) files)"
