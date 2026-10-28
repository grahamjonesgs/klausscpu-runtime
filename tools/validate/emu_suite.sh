#!/usr/bin/env bash
# emu_suite.sh <outdir> [OPT]
#
# Build every bare-metal program (baremetal/, `make all`) at OPT (default -O1)
# in a staged copy under <outdir>, run each ELF on the klausscc emulator and
# keep the captures:
#   <outdir>/<prog>.out      emulator output (UART capture, instruction count)
#   <outdir>/summary.txt     "<prog> rc=<status> <bytes>B" per program
#   <outdir>/build.log       the make output; build_errors.txt counts 'error:'
#   <outdir>/stats.tsv       prog, emulator instructions ('nohalt' when the
#                            program did not halt), .text bytes
#   <outdir>/elf/            the ELFs
# Compare two runs (or a run and baselines/emu-<OPT>) with compare.sh.
set -uo pipefail
. "$(dirname "$0")/common.sh"
[ $# -ge 1 ] || die "usage: emu_suite.sh <outdir> [OPT]"
OUT=$(realpath -m "$1"); OPT=${2:--O1}
MAXI=${EMU_MAX_INSTRUCTIONS:-800000000}
rm -rf "$OUT"; mkdir -p "$OUT/elf"
stage_baremetal "$OUT"
make -C "$STAGED_BM" -j"$JOBS" -k all OPT="$OPT" ${PICOLIBC:+PICOLIBC=$PICOLIBC} >"$OUT/build.log" 2>&1
grep -c 'error:' "$OUT/build.log" >"$OUT/build_errors.txt"
cp "$STAGED_BM"/*.elf "$OUT/elf/" 2>/dev/null
echo "toolchain $(toolchain_id) OPT=$OPT" >"$OUT/toolchain.txt"
export -f emu; export KLAUSSCC MAXI OUT
ls "$OUT/elf" | sed -n 's/\.elf$//p' |
  xargs -P "$JOBS" -I{} bash -c 'emu "$MAXI" "$OUT/elf/{}.elf" "$OUT/{}.out" 300; echo "{} rc=$? $(wc -c < "$OUT/{}.out")B"' |
  sort >"$OUT/summary.txt"
: >"$OUT/stats.tsv"
for e in "$OUT"/elf/*.elf; do
  n=$(basename "$e" .elf)
  i=$(emu_instructions "$OUT/$n.out")
  grep -aq 'stop = Halt' "$OUT/$n.out" || i=nohalt
  printf '%s\t%s\t%s\n' "$n" "${i:-none}" \
    "$("$LLVM_BIN/llvm-size" -A "$e" | awk '$1==".text"{print $2}')" >>"$OUT/stats.tsv"
done
rm -rf "$OUT/tree"
exit 0
