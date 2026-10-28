# shellcheck shell=bash
# common.sh -- sourced by every script in tools/validate/.
#
# Resolves the tools and trees the harness needs, from the environment first
# and then from the usual sibling-checkout layout:
#
#   KLAUSSCPU_LLVM_BIN  toolchain bin dir      (default <runtime>/../klausscpu-llvm/build/bin)
#   KLAUSSCPU_LLVM_SRC  klausscpu-llvm source  (default $KLAUSSCPU_LLVM_BIN/../..)
#   KLAUSSCC            klausscc binary        (default <runtime>/../klausscc/target/release/klausscc,
#                                               then klausscc on PATH)
#   KLAUSSCPU_RUNTIME   runtime checkout       (default: the one holding this script)
#   BOARD_LOCK          lock file every board command runs under (flock)
#                                              (default ${XDG_RUNTIME_DIR:-/tmp}/klausscpu-board.lock)
#   JOBS                parallel jobs          (default: nproc)
#
# Nothing here writes into the runtime checkout: the bare-metal builds run in
# a staged copy under the output directory (stage_baremetal).

VALIDATE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
RT=$(cd "${KLAUSSCPU_RUNTIME:-$VALIDATE_DIR/../..}" && pwd)

die() { echo "validate: $*" >&2; exit 2; }
note() { echo "validate: $*" >&2; }

if [ -z "${KLAUSSCPU_LLVM_BIN:-}" ]; then
  if [ -x "$RT/../klausscpu-llvm/build/bin/clang" ]; then
    KLAUSSCPU_LLVM_BIN=$(cd "$RT/../klausscpu-llvm/build/bin" && pwd)
  else
    die "KLAUSSCPU_LLVM_BIN is not set and $RT/../klausscpu-llvm/build/bin has no clang"
  fi
fi
export KLAUSSCPU_LLVM_BIN
LLVM_BIN=$KLAUSSCPU_LLVM_BIN
[ -x "$LLVM_BIN/clang" ] || die "no clang in KLAUSSCPU_LLVM_BIN=$LLVM_BIN"
LLVM_SRC=${KLAUSSCPU_LLVM_SRC:-$(cd "$LLVM_BIN/../.." && pwd)}

if [ -z "${KLAUSSCC:-}" ]; then
  if [ -x "$RT/../klausscc/target/release/klausscc" ]; then
    KLAUSSCC=$(cd "$RT/../klausscc/target/release" && pwd)/klausscc
  elif command -v klausscc >/dev/null 2>&1; then
    KLAUSSCC=$(command -v klausscc)
  else
    die "KLAUSSCC is not set, ../klausscc is not built and klausscc is not on PATH"
  fi
fi
export KLAUSSCC

BOARD_LOCK=${BOARD_LOCK:-${XDG_RUNTIME_DIR:-/tmp}/klausscpu-board.lock}
export BOARD_LOCK
JOBS=${JOBS:-$(nproc)}

# board <timeout-seconds> <elf> <out-file>
# Load <elf> over the UART and capture the board's output until the CPU halts
# or the timeout expires, holding $BOARD_LOCK for the whole run.  Returns
# klausscc's status (124 on timeout).
board() {
  flock "$BOARD_LOCK" timeout "$1" "$KLAUSSCC" -i "$2" -s -m </dev/null >"$3" 2>&1
}

# emu <max-instructions> <elf> <out-file> [timeout]
emu() {
  timeout "${4:-600}" "$KLAUSSCC" -i "$2" --emulate --max-instructions "$1" </dev/null >"$3" 2>&1
}

# uart_filter <capture> -- the program's UART text with the lines that change
# legitimately with code generation or timing removed (cycle counts, timers,
# addresses, the loader's progress lines, interrupt-count races; Zephyr log
# timestamps and colour codes are stripped).  Everything
# else (self-check results, computed values) must not change.
uart_filter() {
  local f=$1
  {
    if grep -aq -- '--- Captured UART output ---' "$f"; then
      sed -n '/--- Captured UART output ---/,$p' "$f"
    elif grep -aq 'Load Complete OK' "$f"; then
      sed -n '/Load Complete OK/,$p' "$f"
    else
      cat "$f"
    fi
  } | tr -d '\r' | sed -E 's/\x1b\[[0-9;]*m//g; s/^\[[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{3},[0-9]{3}\] //' \
    | grep -avE '^[0-9]{2}:[0-9]{2}:[0-9]{2}|DMIPS|tick count|Sent |bytes|baud' \
    | grep -avE 'time=|cycles|CPI|instr=|Calibrated|iterations|Dhrystones|DMIPS|us/run|ms/run|FASTPATH|^mix|^cyc:|^cache:|^branch:|^CSV|^HAZ,|^haz:|Microseconds|per Second|Emulator finished|Arr_2_Glob|^Cycles|^Instructions +:|/Dhrystone|time +:|CLOCK_MS|addr +=|INT_VEC|Runs +:|0x0000[0-9a-f]{4}$' \
    | grep -avE 'Sending to board|Completed with|heap_start|^ +sample: |^ +(wbacks|stalls|rd|wr|hits|miss|evict|total)[a-z_]* *:|bigdir|entries|ISR increments; plain counter lost'
}

# uart_lines <capture> -- how many filtered lines to compare: a run stopped by
# the instruction cap (a program that never halts on the emulator) is compared
# on its first 12 lines only.
uart_lines() {
  if grep -aq 'stop = InstructionCap' "$1"; then echo 12; else echo 100000; fi
}

# emu_instructions <capture> -- instructions the emulator executed.
emu_instructions() {
  grep -ao 'finished: [0-9]*' "$1" | grep -o '[0-9]*' | head -1
}

# stage_baremetal <dir>
# Make <dir>/tree a build tree for baremetal/: a copy of baremetal/ (without
# build products) beside symlinks to every other top-level entry of the
# runtime checkout, so the Makefile's ROOT (baremetal/..) resolves to it.
# Builds then never touch the checkout and can run side by side.  The
# benchmark sources (gitignored) are linked from the checkout when present,
# and fetched into the copy otherwise.
stage_baremetal() {
  local T=$1/tree e
  rm -rf "$T"; mkdir -p "$T"
  for e in "$RT"/* "$RT"/.[!.]*; do
    [ -e "$e" ] || continue
    case $(basename "$e") in baremetal|.git) continue ;; esac
    ln -s "$e" "$T/"
  done
  mkdir -p "$T/baremetal"
  (cd "$RT/baremetal" && tar cf - --exclude='*.o' --exclude='*.d' --exclude='*.elf' \
      --exclude='*.llext' --exclude='*.nm' --exclude=bench-build --exclude=bench-results \
      --exclude=benchmarks/coremark --exclude=benchmarks/embench-iot .) | (cd "$T/baremetal" && tar xf -)
  for e in coremark embench-iot; do
    if [ -d "$RT/baremetal/benchmarks/$e" ]; then ln -s "$RT/baremetal/benchmarks/$e" "$T/baremetal/benchmarks/$e"; fi
  done
  [ -e "$RT/picolibc-install/lib/libc.a" ] || [ -n "${PICOLIBC:-}" ] ||
    die "$RT/picolibc-install is missing: run ./build-picolibc.sh, or 'validate.sh deps <checkout>' to link another checkout's"
  STAGED_BM=$T/baremetal
}

# toolchain_id -- the klausscpu-llvm commit clang was built from, plus a note
# when the source tree has uncommitted changes.
toolchain_id() {
  local tc dirty
  tc=$("$LLVM_BIN/clang" --version 2>/dev/null | grep -o 'klausscpu-llvm[^ ]* [0-9a-f]*' | awk '{print substr($2,1,12)}')
  dirty=$(git -C "$LLVM_SRC" status --porcelain --untracked-files=no 2>/dev/null | grep -q . && echo "+dirty")
  echo "${tc:-unknown}${dirty}"
}

# abi_version -- the ABI version (low byte of e_flags) the toolchain emits.
abi_version() {
  local d fl; d=$(mktemp -d)
  echo 'int f(void){return 0;}' > "$d/a.c"
  if "$LLVM_BIN/clang" -target klausscpu-unknown-elf -c "$d/a.c" -o "$d/a.o" 2>/dev/null; then
    fl=$("$LLVM_BIN/llvm-readelf" -h "$d/a.o" | awk '/Flags:/{print $2}')
    echo $(( ${fl:-0} & 0xff ))
  fi
  rm -rf "$d"
}

# picolibc_for_toolchain -- the picolibc install that matches the toolchain's
# ABI: picolibc-install, or picolibc-install-oldabi for an ABI-0 toolchain
# (lld refuses to mix ABI versions).  $PICOLIBC overrides.
picolibc_for_toolchain() {
  if [ -n "${PICOLIBC:-}" ]; then echo "$PICOLIBC"; return; fi
  if [ "$(abi_version)" = 0 ]; then echo "$RT/picolibc-install-oldabi"; else echo "$RT/picolibc-install"; fi
}
