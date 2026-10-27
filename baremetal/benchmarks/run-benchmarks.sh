#!/bin/bash
# run-benchmarks.sh — build CoreMark + Embench-IoT and run them on the board.
#
# Usage:  benchmarks/run-benchmarks.sh [OPT ...]        (default: -O2)
#         e.g. benchmarks/run-benchmarks.sh -O2 -Os -O1
#
# For each OPT it builds into bench-build/<OPT>/, runs every program on the
# FPGA board over the UART (klausscc -s -m), and writes into
# bench-results/<OPT>/:
#   <prog>.out     the full UART capture per program
#   coremark.csv   CSV lines from the CoreMark runs (performance, validation)
#   embench.csv    name,cycles,instr,verify,status,text_bytes
#   summary.txt    human-readable tables, with the Embench scores from
#                  benchmarks/embench-score.py
#
# Environment:
#   KLAUSSCPU_LLVM_BIN  toolchain bin dir (required, as for every build here)
#   KLAUSSCC            klausscc binary   (default: klausscc on PATH)
#   BOARD_LOCK          if set, every board command runs under `flock $BOARD_LOCK`
#   RUN_COREMARK=0      skip CoreMark;  RUN_EMBENCH=0  skip Embench
#   COREMARK_ITERATIONS, EMBENCH_CPU_MHZ   passed through to make
#   BENCH_TIMEOUT_CM (default 180 s) / BENCH_TIMEOUT_EMB (default 120 s)
#
# Exit status: 0 if every program verified, 1 otherwise.

set -uo pipefail
cd "$(dirname "$0")/.."            # baremetal/

KLAUSSCC=${KLAUSSCC:-klausscc}
TO_CM=${BENCH_TIMEOUT_CM:-180}
TO_EMB=${BENCH_TIMEOUT_EMB:-120}
OPTS=("$@"); [ ${#OPTS[@]} -eq 0 ] && OPTS=(-O2)
MAKEVARS=()
[ -n "${COREMARK_ITERATIONS:-}" ] && MAKEVARS+=("COREMARK_ITERATIONS=$COREMARK_ITERATIONS")
[ -n "${EMBENCH_CPU_MHZ:-}" ] && MAKEVARS+=("EMBENCH_CPU_MHZ=$EMBENCH_CPU_MHZ")
BENCHES=$(make -s --no-print-directory -f - <<'EOF' 2>/dev/null
include benchmarks/benchmarks.mk
all: ; @echo $(EMBENCH_BENCHES)
EOF
)
[ -z "$BENCHES" ] && BENCHES="aha-mont64 crc32 cubic edn huffbench matmult-int minver nbody nettle-aes nettle-sha256 nsichneu picojpeg qrduino sglib-combined slre st statemate ud wikisort"

board() {   # board <timeout> <elf> <out>
    local cmd=(timeout "$1" "$KLAUSSCC" -i "$2" -s -m)
    if [ -n "${BOARD_LOCK:-}" ]; then cmd=(flock "$BOARD_LOCK" "${cmd[@]}"); fi
    "${cmd[@]}" </dev/null >"$3" 2>&1
}

fail=0
for OPT in "${OPTS[@]}"; do
    D=$(echo "$OPT" | tr -d ' -')
    B=bench-build/$D
    R=bench-results/$D
    mkdir -p "$R"
    targets=()
    [ "${RUN_COREMARK:-1}" != 0 ] && targets+=(coremark)
    [ "${RUN_EMBENCH:-1}" != 0 ] && targets+=(embench)
    echo "==> [$OPT] building ${targets[*]}"
    if ! make -j"$(nproc)" "${targets[@]}" OPT="$OPT" "${MAKEVARS[@]}" >"$R/build.log" 2>&1; then
        echo "BUILD FAILED — see $R/build.log"; fail=1; continue
    fi
    {
        echo "toolchain: $(git -C "$KLAUSSCPU_LLVM_BIN/../.." rev-parse --short=12 HEAD 2>/dev/null || echo unknown)  OPT=$OPT"
    } >"$R/summary.txt"

    if [ "${RUN_COREMARK:-1}" != 0 ]; then
        : >"$R/coremark.csv"
        for p in coremark coremark_validate; do
            echo "==> [$OPT] $p"
            board "$TO_CM" "$B/$p.elf" "$R/$p.out"
            line=$(grep -a '^CSV,coremark' "$R/$p.out" | tr -d '\r')
            [ -z "$line" ] && line="CSV,coremark,,,,,,NO-OUTPUT"
            echo "$p,${line#CSV,coremark,}" >>"$R/coremark.csv"
            case "$line" in *,PASS) ;; *) fail=1 ;; esac
        done
        {
            echo
            echo "CoreMark (iterations,cycles,instr,CoreMark*1000,CoreMark/MHz*1000,status)"
            cat "$R/coremark.csv"
            grep -a -h 'CoreMark 1.0 :' "$R/coremark.out" | tr -d '\r'
        } >>"$R/summary.txt"
    fi

    if [ "${RUN_EMBENCH:-1}" != 0 ]; then
        echo "name,cycles,instr,verify,status,text_bytes" >"$R/embench.csv"
        for b in $BENCHES; do
            echo "==> [$OPT] embench $b"
            board "$TO_EMB" "$B/embench/$b.elf" "$R/emb-$b.out"
            line=$(grep -a '^CSV,embench' "$R/emb-$b.out" | tr -d '\r')
            [ -z "$line" ] && line="CSV,embench,$b,,,,NO-OUTPUT"
            text=$("$KLAUSSCPU_LLVM_BIN/llvm-size" -A "$B/embench-size/$b.elf" | awk '$1==".text"{print $2}')
            echo "${line#CSV,embench,},$text" >>"$R/embench.csv"
            case "$line" in *,PASS) ;; *) fail=1 ;; esac
        done
        {
            echo
            echo "Embench-IoT 1.0 (CPU_MHZ=${EMBENCH_CPU_MHZ:-1})"
            python3 -I benchmarks/embench-score.py --cpu-mhz "${EMBENCH_CPU_MHZ:-1}" "$R/embench.csv"
        } >>"$R/summary.txt"
    fi
    cat "$R/summary.txt"
done
exit $fail
