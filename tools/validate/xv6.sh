#!/usr/bin/env bash
# xv6.sh <outdir> [quick|full]
#
# Build xv6 (xv6/, XV6_PLAN.md) and run it on the klausscc emulator with a
# scripted shell session (klausscc --uart-in, paced by the "$ " prompt with
# --uart-prompt, so the run ends when the last command's prompt is back):
#   quick (default)  boot, init -> sh, echo, pipes, redirection, mkdir/ln/rm,
#                    ls, wc, grep, ';' lists, forktest
#   full             quick, then usertests -q (about 2 minutes)
# Each expected line must appear in the UART output, in order
# (<outdir>/<step>.out has the run, <outdir>/<step>.expect the lines).
# Exits 1 on any failure.
set -uo pipefail
. "$(dirname "$0")/common.sh"
[ $# -ge 1 ] || die "usage: xv6.sh <outdir> [quick|full]"
OUT=$(realpath -m "$1"); MODE=${2:-quick}
case $MODE in quick|full) ;; *) die "unknown mode $MODE" ;; esac
"$KLAUSSCC" --help 2>/dev/null | grep -q -- '--uart-prompt' ||
  die "$KLAUSSCC has no --uart-prompt: build klausscc's mmu branch (d0035b2 or later) and set KLAUSSCC"
rm -rf "$OUT"; mkdir -p "$OUT"
fail=0
bad() { echo "FAIL $*"; fail=$((fail + 1)); }

make -C "$RT/xv6" >"$OUT/build.log" 2>&1 || { echo "FAIL xv6 build (see $OUT/build.log)"; exit 1; }
cp "$RT/xv6/kernel/kernel" "$OUT/kernel.elf"

# expect_in_order <expect> <out>: every line of <expect> is a whole line of
# <out> (CRs, trailing blanks and the prompt prefix "$ " ignored), in order.
expect_in_order() {
  awk 'NR == FNR { sub(/[ \t]+$/, ""); want[n++] = $0; next }
       { sub(/\r$/, ""); sub(/[ \t]+$/, ""); line = $0; while (sub(/^\$ /, "", line)) {}
         if (i < n && line == want[i]) i++ }
       END { if (i < n) { print "missing: " want[i]; exit 1 } }' "$1" "$2"
}

# run_step <name> <max instructions> <timeout s>: <name>.in typed at the prompt.
run_step() {
  timeout "$3" "$KLAUSSCC" -i "$OUT/kernel.elf" --emulate --max-instructions "$2" \
    --uart-in "$OUT/$1.in" --uart-prompt '$ ' </dev/null >"$OUT/$1.out" 2>&1
  grep -aq 'stop = InputDone' "$OUT/$1.out" || bad "$1: the session did not finish ($(grep -ao 'stop = [A-Za-z]*' "$OUT/$1.out"))"
  local why
  why=$(expect_in_order "$OUT/$1.expect" "$OUT/$1.out") || bad "$1: $why"
  grep -aq 'panic' "$OUT/$1.out" && bad "$1: kernel panic"
  echo "$1: $(grep -ao 'Emulator finished: [0-9]* instructions' "$OUT/$1.out")"
}

cat >"$OUT/quick.in" <<'EOF'
echo hello world
cat README | wc
grep xv6 README | wc
mkdir d
echo foo > d/f
cat d/f
ln d/f d/g
cat d/g
rm d/f
ls d
echo a; echo b
wc README README
forktest
EOF
cat >"$OUT/quick.expect" <<'EOF'
xv6 kernel is booting
init: starting sh
echo hello world
hello world
cat README | wc
48 336 2441
grep xv6 README | wc
4 45 274
cat d/f
foo
cat d/g
foo
ls d
.              1 24 64
..             1 1 1024
g              2 25 4
echo a; echo b
a
b
wc README README
48 336 2441 README
48 336 2441 README
forktest
fork test OK
EOF
run_step quick 2000000000 120

if [ "$MODE" = full ]; then
  printf 'usertests -q\n' >"$OUT/usertests.in"
  printf 'usertests -q\nusertests starting\nALL TESTS PASSED\n' >"$OUT/usertests.expect"
  run_step usertests 40000000000 1200
fi

if [ $fail -eq 0 ]; then echo "xv6 $MODE: PASS"; else echo "xv6 $MODE: $fail failure(s)"; fi
[ $fail -eq 0 ]
