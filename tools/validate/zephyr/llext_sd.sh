#!/usr/bin/env bash
# zephyr/llext_sd.sh <outdir> [build] [backup] [upload] [run]   (default: all four)
#
# Rebuild the LLEXT extensions that live on the board's SD card and check
# them on the board through the ssh_shell app's `run` and `svc` commands.
#
# build   the bare-metal extensions (`make ext-demos`, in a staged copy of
#         baremetal/: hello, adventure, expr, bst, crypto, queens, test_64bit,
#         test_switch, perf_baseline, dhrystone, test_fp) and the ssh_shell app
#         with its two HTTP API backends (apibackend.llext, ctrl.llext);
#         everything goes to <outdir>/llext/, the kernel to <outdir>/elf/.
# backup  download every *.llext on the card to <outdir>/backup/.
# upload  PUT <outdir>/llext/*.llext to the card's root.  A file that would
#         overwrite one on the card is refused unless the backup step has
#         saved it (llext_sd.py).
# run     `run` each program over SSH (adventure with typed commands) and
#         compare its output with the bare-metal baseline of the same program
#         (baselines/board-O1/<prog>.txt, else emu-O1/<prog>.txt, after
#         uart_filter; adventure, which the emulator stops while it waits for
#         input, on the baseline's lines); `svc load` each backend and query
#         its HTTP route.
#
# The board steps run in one session: the ssh_shell kernel from the build
# step (or <outdir>/elf/ssh_shell.elf) is loaded under flock $BOARD_LOCK and
# kept running until they finish.  Board address: KLAUSSCPU_BOARD_IP.
# Extensions on the card that this script does not build (the Rust ones,
# hello_rs.llext and ext_hello.llext, from klausscpu-llvm's
# llvm/lib/Target/KlaussCPU/rust/) are left alone.
# Prints PASS/FAIL per check; exits 1 if any failed.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../common.sh"
[ $# -ge 1 ] || die "usage: zephyr/llext_sd.sh <outdir> [build] [backup] [upload] [run]"
OUT=$(realpath -m "$1"); shift
STEPS=${*:-build backup upload run}
PY="python3 -I $HERE/llext_sd.py"
RUNS="hello expr bst crypto queens test_64bit test_switch test_fp dhrystone perf_baseline adventure"
ADVENTURE_INPUT=l,g,n,i,q
mkdir -p "$OUT/llext" "$OUT/elf" "$OUT/logs"
fail=0
has() { [[ " $STEPS " == *" $1 "* ]]; }

if has build; then
  stage_baremetal "$OUT/stage"
  if make -C "$STAGED_BM" -j"$JOBS" ext-demos >"$OUT/logs/build-ext.log" 2>&1; then
    cp "$STAGED_BM"/*.llext "$OUT/llext/" && echo "PASS build ext-demos ($(cd "$STAGED_BM" && echo *.llext))"
  else
    echo "FAIL build ext-demos (see $OUT/logs/build-ext.log)"; fail=1
  fi
  rm -f "$OUT/elf/ssh_shell.elf"
  if "$HERE/build.sh" ssh_shell "$OUT/build/ssh_shell" >"$OUT/logs/build-ssh_shell.log" 2>&1 &&
     cp "$OUT/build/ssh_shell/zephyr/zephyr.elf" "$OUT/elf/ssh_shell.elf" &&
     cp "$OUT/build/ssh_shell/apibackend.llext" "$OUT/build/ssh_shell/ctrl.llext" "$OUT/llext/"; then
    echo "PASS build ssh_shell + apibackend.llext ctrl.llext"
  else
    echo "FAIL build ssh_shell (see $OUT/logs/build-ssh_shell.log)"; fail=1
  fi
  echo "toolchain $(toolchain_id), ABI $(abi_version)" >"$OUT/toolchain.txt"
fi

# body -- filter: a capture or baseline without the loader's and emulator's
# framing lines and without trailing blank lines.
body() {
  sed -e '/^--- Captured UART output ---$/d' -e '/^Load Complete OK$/d' -e '/^CPU halted\.$/d' \
      -e 's/--- end UART ---$//' | awk '{ l[NR] = $0 } END { n = NR; while (n > 0 && l[n] ~ /^[ \t]*$/) n--; for (i = 1; i <= n; i++) print l[i] }'
}

# The board part, run under the lock while ssh_shell runs.
board_steps() {
  local r=0 name ref got
  if has backup; then
    $PY backup "$OUT/backup" >"$OUT/backup.log" 2>&1 || r=1
    $PY list "$OUT/card-before.tsv" >/dev/null 2>&1
    echo "$( [ $r = 0 ] && echo PASS || echo FAIL) backup: $(grep -c '^ok' "$OUT/backup.log") extensions saved in $OUT/backup"
  fi
  if has upload; then
    if $PY upload "$OUT/backup" "$OUT"/llext/*.llext >"$OUT/upload.log" 2>&1; then
      echo "PASS upload: $(grep -c '^ok' "$OUT/upload.log") files"
    else
      echo "FAIL upload (see $OUT/upload.log)"; r=1
    fi
    $PY list "$OUT/card-after.tsv" >/dev/null 2>&1
  fi
  if has run; then
    mkdir -p "$OUT/run"
    for name in $RUNS; do
      if [ "$name" = adventure ]; then $PY run "$OUT/run" "$name.llext:$ADVENTURE_INPUT"
      else $PY run "$OUT/run" "$name.llext"; fi >>"$OUT/run.log" 2>&1
      # the program's own output: after "Running main", up to "Program exited"
      sed -n '/^Running main @/,/^Program exited:/{/^Running main @/d;/^Program exited:/d;p}' \
        "$OUT/run/$name.llext.ssh" >"$OUT/run/$name.txt" 2>/dev/null
      got=$(uart_filter "$OUT/run/$name.txt" | body)
      ref=$VALIDATE_DIR/baselines/board-O1/$name.txt
      [ -e "$ref" ] || ref=$VALIDATE_DIR/baselines/emu-O1/$name.txt
      ref=$(body <"$ref")
      if ! grep -aq '^Program exited: 0$' "$OUT/run/$name.llext.ssh" 2>/dev/null; then
        echo "FAIL run $name: $(grep -a "^$name.llext " "$OUT/run.log" | tail -1 | cut -d' ' -f2-)"; r=1
      elif [ "$name" = adventure ]; then
        # the baseline stops at the first prompt: match up to it
        if [ "$(head -n "$(wc -l <<<"$ref")" <<<"$got" | sed '$s/^> .*/> /')" = "$(sed '$s/^> .*/> /' <<<"$ref")" ]; then
          echo "PASS run $name: exit 0, output starts as the baseline's"
        else echo "FAIL run $name: output differs from the baseline's (see $OUT/run/$name.txt)"; r=1; fi
      elif [ "$got" = "$ref" ]; then
        echo "PASS run $name: exit 0, output = baseline's"
      else
        echo "FAIL run $name: output differs from the baseline's:"
        diff <(echo "$ref") <(echo "$got") | head -20 | sed 's/^/    /'; r=1
      fi
    done
    while IFS=$'\t' read -r name url re; do
      if $PY svc "$OUT/run" "$name" "$url" "$re" >>"$OUT/run.log" 2>&1; then
        echo "PASS svc $name: $url matches $re"
      else
        echo "FAIL svc $name: $url ($(grep -a "^$name " "$OUT/run.log" | tail -1 | cut -d' ' -f2-))"; r=1
      fi
    done <<'EOF'
apibackend.llext	/api/status	"uptime_ms":[0-9]+,"switches":[0-9]+,"leds":[0-9]+,"seg":[0-9]+,"perf":\{"cycles":[0-9]+
ctrl.llext	/ctrl/ping	"pong":true,"uptime_ms":[0-9]+
ctrl.llext	/ctrl/echo?msg=klausscpu	"echo":"klausscpu"
EOF
  fi
  return $r
}

if has backup || has upload || has run; then
  ELF=$OUT/elf/ssh_shell.elf
  [ -e "$ELF" ] || die "no $ELF (run the build step first)"
  (
    flock 9
    timeout 1800 "$KLAUSSCC" -i "$ELF" -s -m </dev/null >"$OUT/logs/board-uart.log" 2>&1 &
    pid=$!
    up=0
    for _ in $(seq 120); do
      grep -aq 'SSH listening' "$OUT/logs/board-uart.log" && { up=1; break; }
      kill -0 $pid 2>/dev/null || break
      sleep 1
    done
    if [ $up = 1 ]; then sleep 4; board_steps; r=$?
    else echo "FAIL board: ssh_shell did not come up (see $OUT/logs/board-uart.log)"; r=1; fi
    kill $pid 2>/dev/null; wait $pid 2>/dev/null
    if grep -aq 'CRASH DUMP\|FATAL ERROR' "$OUT/logs/board-uart.log"; then
      echo "FAIL board: the kernel crashed (see $OUT/logs/board-uart.log)"; r=1
    fi
    exit $r
  ) 9>"$BOARD_LOCK" || fail=1
fi
exit $fail
