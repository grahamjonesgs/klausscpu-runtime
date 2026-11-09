#!/usr/bin/env bash
# linux.sh <outdir> [shell|m3] [emu|board]
#
# Build the KlaussCPU Linux port (LINUX_PLAN.md: linux/fetch.sh's v7.3.3
# checkout, musl from linux/musl/build.sh) into <outdir>/build and run it on
# the klausscc emulator (emu, the default) or the board (board: loaded over
# the UART and typed with -m, under flock $BOARD_LOCK; needs a bitstream
# with the stage 3 TLB):
#   shell (default)  initramfs "minish": a scripted session at the "$ "
#                    prompt (klausscc --uart-in/--uart-prompt): built-ins,
#                    /proc, tmpfs, and fork+execve of the musl test program
#   m3               the libc-free milestone-3 /init (no input)
# Each expected line must appear in the UART output, in order
# (<outdir>/<step>.out has the run, <outdir>/<step>.expect the lines).
# Needs linux-ws/linux (linux/fetch.sh).  Exits 1 on any failure.
set -uo pipefail
. "$(dirname "$0")/common.sh"
[ $# -ge 1 ] || die "usage: linux.sh <outdir> [shell|m3] [emu|board]"
OUT=$(realpath -m "$1"); MODE=${2:-shell}; WHERE=${3:-emu}
case $MODE in shell|m3) ;; *) die "unknown mode $MODE" ;; esac
case $WHERE in emu|board) ;; *) die "unknown target $WHERE" ;; esac
"$KLAUSSCC" --help 2>/dev/null | grep -q -- '--uart-prompt' ||
  die "$KLAUSSCC has no --uart-prompt: build klausscc's mmu branch and set KLAUSSCC"
[ -d "$RT/linux-ws/linux/.git" ] || die "no kernel checkout: run linux/fetch.sh"
rm -rf "$OUT"; mkdir -p "$OUT"
fail=0
bad() { echo "FAIL $*"; fail=$((fail + 1)); }

export KLAUSSCPU_LLVM_BIN=$LLVM_BIN
if [ "$MODE" = shell ]; then
  "$RT/linux/musl/build.sh" >"$OUT/musl.log" 2>&1 || { echo "FAIL musl build (see $OUT/musl.log)"; exit 1; }
  export INITRAMFS_PROFILE=minish
else
  export INITRAMFS_PROFILE=m3
fi
export LINUX_BUILD=$OUT/build
"$RT/linux/build.sh" defconfig >"$OUT/build.log" 2>&1 &&
  "$RT/linux/build.sh" vmlinux >>"$OUT/build.log" 2>&1 ||
  { echo "FAIL kernel build (see $OUT/build.log)"; exit 1; }
cp "$OUT/build/vmlinux" "$OUT/vmlinux"

# expect_in_order <expect> <out>: every line of <expect> is a whole line of
# <out> (CRs, trailing blanks, a "[ time]" printk prefix and the prompt
# prefix "$ " ignored), in order.
expect_in_order() {
  awk 'BEGIN { n = 0; i = 0 }
       NR == FNR { sub(/[ \t]+$/, ""); want[n++] = $0; next }
       { sub(/\r$/, ""); sub(/[ \t]+$/, ""); line = $0
         sub(/^\[ *[0-9]+\.[0-9]+\] /, "", line); while (sub(/^\$ /, "", line)) {}
         if (i < n && line == want[i]) i++ }
       END { if (i < n) { print "missing: " want[i]; exit 1 } }' "$1" "$2"
}

# run_step <name> <max instructions> <timeout s> [prompt]: <name>.in typed
# at the prompt (no input without a prompt).
run_step() {
  local input=()
  [ -n "${4:-}" ] && input=(--uart-in "$OUT/$1.in" --uart-prompt "$4")
  if [ "$WHERE" = board ]; then
    flock "$BOARD_LOCK" timeout "$(( $3 * 3 + 120 ))" "$KLAUSSCC" -i "$OUT/vmlinux" -s -m \
      "${input[@]}" </dev/null >"$OUT/$1.out" 2>&1
  else
    timeout "$3" "$KLAUSSCC" -i "$OUT/vmlinux" --emulate --max-instructions "$2" \
      "${input[@]}" </dev/null >"$OUT/$1.out" 2>&1
    if [ -n "${4:-}" ]; then
      grep -aq 'stop = InputDone' "$OUT/$1.out" || bad "$1: the session did not finish ($(grep -ao 'stop = [A-Za-z]*' "$OUT/$1.out"))"
    fi
    echo "$1: $(grep -ao 'Emulator finished: [0-9]* instructions' "$OUT/$1.out")"
  fi
  local why
  why=$(expect_in_order "$OUT/$1.expect" "$OUT/$1.out") || bad "$1: $why"
  grep -aq 'Kernel panic\|Oops' "$OUT/$1.out" && bad "$1: kernel panic"
}

if [ "$MODE" = shell ]; then
  cat >"$OUT/shell.in" <<'EOF2'
echo hello from minish
uname
ls /bin
mount
cat /proc/self/comm
/bin/musltest
mkdir /tmp/d
ls /tmp
rm /tmp/d
ls /tmp
EOF2
  cat >"$OUT/shell.expect" <<'EOF2'
Run /init as init process
minish: KlaussCPU Linux shell (built-ins: echo cd pwd ls cat mkdir rm uname mount ps exit; or a /path/to/program)
echo hello from minish
hello from minish
uname
Linux (none) 7.3.3-dirty klausscpu
ls /bin
mount
cat /proc/self/comm
init
/bin/musltest
musltest: malloc+memset ok, strlen=99999
musltest: signal handler ran: yes
musltest: setjmp/longjmp ok
musltest: child exited with 5
musltest: PASS
mkdir /tmp/d
ls /tmp
d
rm /tmp/d
ls /tmp
EOF2
  run_step shell 3000000000 600 '$ '
else
  cat >"$OUT/m3.expect" <<'EOF2'
Run /init as init process
init: hello from user space
init: getpid = 1
init: data_word = 1235
init: child sees data_word = 99
init: child exit status 7
init: parent still sees data_word = 1235
init: milestone 3 done, idling
EOF2
  run_step m3 600000000 600
fi

if [ $fail -eq 0 ]; then echo "linux $MODE $WHERE: PASS"; else echo "linux $MODE $WHERE: $fail failure(s)"; fi
[ $fail -eq 0 ]
