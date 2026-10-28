#!/usr/bin/env bash
# zephyr/suite.sh <outdir> [build] [emu] [board]   (default: all three)
#
# build  pristine-build every app ($ZEPHYR_APPS) into <outdir>/build/<app>,
#        copy zephyr.elf to <outdir>/elf/<app>.elf (logs in <outdir>/logs/);
#        the build dir is removed afterwards unless KEEP_BUILD=1
# emu    run each ELF on the emulator for 200M instructions (<outdir>/emu/)
#        and check expect-emu.txt: the expected lines, and a stop at the
#        instruction cap (a trap or fault fails)
# board  run each ELF on the board under flock $BOARD_LOCK (<outdir>/board/):
#        the plain apps until their timeout; the GUI apps with a VNC grab once
#        their VNC server is up (<app>.png, needs python3 PIL); ssh_shell with
#        an SSH session (ssh_session.txt).  Checks expect-board.txt.
#        Board address: KLAUSSCPU_BOARD_IP (default 192.168.68.59).
# Prints PASS/FAIL per check; exits 1 if any failed.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/../common.sh"
[ $# -ge 1 ] || die "usage: zephyr/suite.sh <outdir> [build] [emu] [board]"
OUT=$(realpath -m "$1"); shift
STEPS=${*:-build emu board}
APPS=${ZEPHYR_APPS:-"hello dbg_synchronization irq_soak llext_demo mandel gui_demo gui_lvgl doom ssh_shell"}
IP=${KLAUSSCPU_BOARD_IP:-192.168.68.59}
mkdir -p "$OUT/elf" "$OUT/logs"
fail=0
check() {   # check <expect-file> <dir> -- every pattern must match its capture
  local app re f base
  while IFS=$'\t' read -r app re; do
    case $app in ''|'#'*) continue ;; esac
    base=${app%%.*}; [ "$base" = ssh_session ] && base=ssh_shell
    [[ " $APPS " == *" $base "* ]] || continue
    f=$2/$app.out; [ "$app" = ssh_session ] && f=$2/ssh_session.txt
    case $app in *.vnc) f=$2/$app.txt ;; esac
    if tr -d '\r' <"$f" 2>/dev/null | grep -aEq -- "$re"; then echo "PASS $app: $re"
    else echo "FAIL $app: $re (in $f)"; fail=1; fi
  done <"$1"
}
for step in $STEPS; do
  case $step in
  build)
    for a in $APPS; do
      rm -f "$OUT/elf/$a.elf"
      if "$HERE/build.sh" "$a" "$OUT/build/$a" >"$OUT/logs/build-$a.log" 2>&1 &&
         cp "$OUT/build/$a/zephyr/zephyr.elf" "$OUT/elf/$a.elf"; then
        echo "PASS build $a"
        [ "${KEEP_BUILD:-0}" = 1 ] || rm -rf "$OUT/build/$a"
      else
        echo "FAIL build $a (see $OUT/logs/build-$a.log)"; fail=1
      fi
    done
    echo "toolchain $(toolchain_id)" >"$OUT/toolchain.txt"
    ;;
  emu)
    mkdir -p "$OUT/emu"
    for a in $APPS; do
      [ -e "$OUT/elf/$a.elf" ] || continue
      emu 200000000 "$OUT/elf/$a.elf" "$OUT/emu/$a.out" 600 &
    done
    wait
    for a in $APPS; do
      [ -e "$OUT/emu/$a.out" ] || continue
      if grep -aq 'stop = InstructionCap' "$OUT/emu/$a.out"; then echo "PASS $a: ran to the instruction cap"
      else echo "FAIL $a: $(grep -ao 'stop = [A-Za-z]*' "$OUT/emu/$a.out" || echo 'no stop line')"; fail=1; fi
    done
    check "$HERE/expect-emu.txt" "$OUT/emu"
    ;;
  board)
    O=$OUT/board; mkdir -p "$O"
    waitfor() {   # waitfor <file> <regex> <pid> -- until the line appears or <pid> exits
      while kill -0 "$3" 2>/dev/null; do grep -aq "$2" "$1" 2>/dev/null && return 0; sleep 1; done; return 1
    }
    timeout_of() {
      case $1 in
        hello) echo 40 ;; dbg_synchronization|llext_demo) echo 45 ;; irq_soak) echo 80 ;;
        mandel) echo 100 ;; gui_demo) echo 75 ;; gui_lvgl) echo 110 ;; doom) echo 130 ;; ssh_shell) echo 150 ;;
        *) echo 60 ;;
      esac
    }
    for a in $APPS; do
      [ -e "$OUT/elf/$a.elf" ] || continue
      t=$(timeout_of "$a")
      case $a in
        mandel|gui_demo|gui_lvgl|doom)
          board "$t" "$OUT/elf/$a.elf" "$O/$a.out" &
          if waitfor "$O/$a.out" 'VNC server on' $!; then
            sleep 15
            timeout 60 python3 -I "$HERE/vncgrab.py" "$IP" "$O/$a.png" >"$O/$a.vnc.txt" 2>&1
          fi
          wait ;;
        ssh_shell)
          board "$t" "$OUT/elf/$a.elf" "$O/$a.out" &
          if waitfor "$O/$a.out" 'SSH listening' $!; then
            sleep 3
            DELAY=4 "$HERE/sshrun.sh" 'kernel version' 'kernel uptime' >"$O/ssh_session.txt" 2>&1
          fi
          wait ;;
        *) board "$t" "$OUT/elf/$a.elf" "$O/$a.out" ;;
      esac
      echo "$a $(wc -c <"$O/$a.out")B" >>"$O/summary.txt"
    done
    check "$HERE/expect-board.txt" "$O"
    if grep -al 'FATAL ERROR' "$O"/*.out 2>/dev/null; then echo "FAIL: a Zephyr fatal error (above)"; fail=1; fi
    python3 -I "$VALIDATE_DIR/perf.py" extract zephyr "$O" zephyr >"$OUT/perf.tsv"
    ;;
  *) die "unknown step $step" ;;
  esac
done
exit $fail
