#!/usr/bin/env bash
# lldb_e2e.sh <outdir> [emu] [board]   (default: emu)
#
# lldb end to end against klausscc's gdbserver, scripted with `lldb -b -s`:
#   dbgtest   baremetal/programs/dbgtest.c with -g at each of $E2E_OPTS
#             (default -O0 -O1 -O2), session lldb/e2e.lldb: breakpoints on
#             lines and functions, step-in / finish / next, backtraces
#             through frameless, shrink-wrapped and tail-calling functions,
#             frame variables and registers of outer frames (callee-saved
#             registers restored from the CFI), expressions, the exit status
#   zephyr    the synchronization sample with debug info and thread names
#             (zephyr/build.sh dbg_synchronization), session lldb/zephyr.lldb
#             with the RTOS OS plugin (debug/klausscpu_rtos.py): a backtrace
#             through the timer interrupt handler, the thread list with a
#             stack per thread, expressions over kernel structures
#   freertos  freertos/demo with -g, session lldb/freertos.lldb: the same
#             through the tick interrupt and the task list
# on the emulator (`klausscc --gdbserver`) and/or the board (`--gdbserver
# --board -s`: core 1 over its USB-JTAG; needs a DEBUG_EN=1 bitstream, a
# klausscc built with --features board, and holds $BOARD_LOCK per session).
# E2E_PROGS picks a subset of "dbgtest zephyr freertos".
#   <outdir>/<where>/<prog>[-O<n>].{lldb,txt,server.txt,uart.txt}
#   <outdir>/results.txt   a PASS/FAIL line per check
# Prints the PASS/FAIL lines; exits 1 if any failed.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/common.sh"
[ $# -ge 1 ] || die "usage: lldb_e2e.sh <outdir> [emu] [board]"
OUT=$(realpath -m "$1"); shift
WHERE=${*:-emu}
OPTS=${E2E_OPTS:-"-O0 -O1 -O2"}
PROGS=${E2E_PROGS:-"dbgtest zephyr freertos"}
PORT=${E2E_PORT:-5640}
LLDB=$LLVM_BIN/lldb
[ -x "$LLDB" ] || die "no lldb in $LLVM_BIN (build klausscpu-llvm with lldb in LLVM_ENABLE_PROJECTS)"
OSPLUGIN=$RT/debug/klausscpu_rtos.py
mkdir -p "$OUT"
RES=$OUT/results.txt; : >"$RES"
fail=0

result() {   # result PASS|FAIL <label> <detail>
  echo "$1 $2: $3" | tee -a "$RES"
  [ "$1" = PASS ] || fail=1
}

# ---- builds -----------------------------------------------------------------
build_dbgtest() {   # build_dbgtest <opt> -> $OUT/build/dbgtest<opt>/dbgtest.elf
  local d=$OUT/build/dbgtest$1
  rm -rf "$d"; mkdir -p "$d"
  stage_baremetal "$d"
  make -C "$STAGED_BM" dbgtest.elf OPT="$1 -g" ${PICOLIBC:+PICOLIBC=$PICOLIBC} >"$d/build.log" 2>&1 &&
    cp "$STAGED_BM/dbgtest.elf" "$d/dbgtest.elf"
  rm -rf "$d/tree"
  [ -f "$d/dbgtest.elf" ]
}
build_freertos() {   # a staged copy of freertos/ (its objects go next to the sources)
  local d=$OUT/build/freertos T e
  T=$d/tree; rm -rf "$d"; mkdir -p "$T/freertos"
  for e in "$RT"/* "$RT"/.[!.]*; do
    [ -e "$e" ] || continue
    case $(basename "$e") in freertos|.git) continue ;; esac
    ln -s "$e" "$T/"
  done
  (cd "$RT/freertos" && tar cf - --exclude='*.o' --exclude='*.lo' --exclude='*.d' --exclude='*.elf' \
      --exclude='*.bin' --exclude=.git --exclude=./obj --exclude=./wolfssl .) | (cd "$T/freertos" && tar xf -)
  make -C "$T/freertos" -j"$JOBS" demo.elf DBG=-g >"$d/build.log" 2>&1 &&
    cp "$T/freertos/demo.elf" "$d/demo.elf"
  rm -rf "$T"
  [ -f "$d/demo.elf" ]
}
build_zephyr() {
  local d=$OUT/build/zephyr
  mkdir -p "$d"
  "$HERE/zephyr/build.sh" dbg_synchronization "$d/build" >"$OUT/build/zephyr.log" 2>&1 &&
    cp "$d/build/zephyr/zephyr.elf" "$d/zephyr.elf"
  rm -rf "$d/build"
  [ -f "$d/zephyr.elf" ]
}

# ---- one debugger session -----------------------------------------------------
# session <where> <elf> <script> <log-prefix> [sed substitutions...]
# Serve <elf>, run lldb on <script> (with @ELF@, @PORT@, @OSPLUGIN@ and the
# given substitutions applied), wait for the server.  On the board the whole
# session holds $BOARD_LOCK.
session() {
  local where=$1 elf=$2 script=$3 log=$4; shift 4
  PORT=$((PORT + 1))
  local -a sub=(-e "s|@ELF@|$elf|g" -e "s|@PORT@|$PORT|g" -e "s|@OSPLUGIN@|$OSPLUGIN|g")
  local s; for s in "$@"; do sub+=(-e "$s"); done
  sed "${sub[@]}" "$script" >"$log.lldb"
  if [ "$where" = board ]; then
    (
      flock 9
      run_session "$elf" "$log" --board -s
    ) 9>"$BOARD_LOCK"
  else
    run_session "$elf" "$log"
  fi
}
run_session() {   # run_session <elf> <log-prefix> [klausscc args]
  local elf=$1 log=$2 server _; shift 2
  "$KLAUSSCC" --input "$elf" --gdbserver "$PORT" "$@" </dev/null >"$log.uart.txt" 2>"$log.server.txt" &
  server=$!
  for _ in $(seq 600); do
    grep -q 'waiting for a debugger' "$log.server.txt" 2>/dev/null && break
    kill -0 "$server" 2>/dev/null || break
    sleep 0.1
  done
  timeout 600 "$LLDB" -b -s "$log.lldb" >"$log.txt" 2>&1
  # A session that ends with `detach` leaves the program running.
  sleep 0.5; kill "$server" 2>/dev/null; wait "$server" 2>/dev/null
}

# ---- checks -----------------------------------------------------------------
has() {     # has <file> <ERE> -- the output has a line matching
  grep -aEq -- "$2" "$1"
}
expect() {  # expect <label> <file> <ERE>...: PASS if every pattern matches
  local label=$1 f=$2 re miss=; shift 2
  for re in "$@"; do has "$f" "$re" || miss+="${miss:+; }'$re'"; done
  if [ -z "$miss" ]; then result PASS "$label" "ok"
  else result FAIL "$label" "missing $miss (see $f)"; fi
}
block() {   # block <file> <command> -- the output of the first "(lldb) <command>"
            # and of the commands after it, up to the next frame select, bt,
            # finish, breakpoint or detach
  S="(lldb) $2" awk 'f && /^\(lldb\) (frame select|bt|finish|breakpoint|detach)/ {exit}
                     f {print} !f && $0 == ENVIRON["S"] {f=1}' "$1"
}
same_line() {  # same_line <file>: in an interrupt backtrace, frame #2 (the
               # interrupted code) is shown at the line of its PC, not PC-1
  local fr pc
  fr=$(grep -a -m1 -E '^ +frame #2: ' "$1" | sed -nE 's/.* at ([^ ]+:[0-9]+(:[0-9]+)?).*/\1/p')
  pc=$(grep -a -m1 -E '^ +pc = ' "$1" | sed -nE 's/.* at ([^ ]+:[0-9]+(:[0-9]+)?).*/\1/p')
  [ -n "$fr" ] && [ "$fr" = "$pc" ]
}

check_dbgtest() {  # check_dbgtest <label> <log> <opt>
  local L=$1 f=$2.txt O=$3 E='dbgtest\.elf`' N='\$[0-9]+'
  expect "$L line-bp" "$f" 'stop reason = breakpoint 1\.1' "frame #0: 0x[0-9a-f]+ ${E}main at dbgtest\.c:$LINE_DIST2_CALL:"
  expect "$L frame-var" "$f" '^\(point\) p = \(x = 1, y = 2\)' '^\(point\) q = \(x = 4, y = 6\)'
  expect "$L step" "$f" "stop reason = breakpoint 2\.1" "${E}dist2\(.*\) at dbgtest\.c:$LINE_SQUARES:" \
    'stop reason = step in' "frame #0: 0x[0-9a-f]+ ${E}square\(v=-3\) at dbgtest\.c" \
    "frame #1: 0x[0-9a-f]+ ${E}dist2\(" "frame #2: 0x[0-9a-f]+ ${E}main at dbgtest\.c:$LINE_DIST2_CALL:"
  expect "$L finish" "$f" "Return value: \(long\) $N = 9\$" "Return value: \(long\) $N = 25\$"
  expect "$L next" "$f" 'stop reason = step over'
  if [ "$(grep -ac 'stop reason = step over' "$f")" -lt 2 ]; then result FAIL "$L next x2" "fewer than two step-over stops"; fi
  expect "$L frameless/tail/shrink-wrap bt" "$f" \
    "${E}square\(v=5\) at dbgtest\.c" "${E}checked\(p=0x0000000000000000\) at dbgtest\.c" \
    "${E}square\(v=7\) at dbgtest\.c" "frame #1: 0x[0-9a-f]+ ${E}checked\(" \
    "Return value: \(long\) $N = -1\$" "Return value: \(long\) $N = 49\$" "Return value: \(long\) $N = 50\$"
  if [ "$O" = -O0 ]; then
    expect "$L tail call (none at -O0)" "$f" "frame #1: 0x[0-9a-f]+ ${E}hop\(v=4\) at dbgtest\.c"
  else
    expect "$L tail call" "$f" "${E}hop\(.*\[artificial\]" 'Stepped out past: frame #1: '
  fi
  expect "$L callee-saved bt" "$f" "frame #0: 0x[0-9a-f]+ ${E}leafsum\(x=21, y=54\)" \
    "frame #1: 0x[0-9a-f]+ ${E}clobber\(" "frame #2: 0x[0-9a-f]+ ${E}keeper\(" \
    "frame #3: 0x[0-9a-f]+ ${E}main at dbgtest\.c:$LINE_KEEPER:" "frame #4: 0x[0-9a-f]+ ${E}_start"
  expect "$L callee-saved vars" "$f" '^\(long\) x = 21$' '^\(long\) y = 54$' \
    '^\(long\) p = 21$' '^\(long\) q = 54$' '^\(long\) s = 17$' '^\(long\) t = 15$' \
    '^\(long\) a = 18$' '^\(long\) b = 2$' '^\(long\) d = 25$' '^\(long\) seven = 7$'
  expect "$L expr" "$f" "^\(long\) $N = 96\$" "^\(long\) $N = 19\$" "^\(long\) $N = 4294967299\$" \
    "^\(volatile long\) $N = 0x0000000000000006\$" "^\(unsigned long\) $N = 75\$" "^\(long\) $N = 182\$"
  expect "$L registers" "$f" ' r0 = 0x0000000000000015$' ' r1 = 0x0000000000000036$' \
    "pc = 0x[0-9a-f]+ +${E}leafsum"
  if [ "$O" != -O0 ]; then
    # From -O1 on the values live in r4-r7 (any order): frame 1 holds
    # clobber's p q s t, frame 2 keeper's a b, frame 3 main's d.
    local miss='' v
    for v in 15 36 11 0f; do block "$f" 'frame select 1' | grep -q "= 0x00000000000000$v\$" || miss+=" f1:0x$v"; done
    for v in 12 02; do block "$f" 'frame select 2' | grep -q "= 0x00000000000000$v\$" || miss+=" f2:0x$v"; done
    block "$f" 'frame select 3' | grep -q '= 0x0000000000000019$' || miss+=" f3:0x19"
    if [ -z "$miss" ]; then result PASS "$L callee-saved registers" "r4-r7 of frames 1-3 hold their values"
    else result FAIL "$L callee-saved registers" "missing$miss (see $f)"; fi
  fi
  expect "$L finish x3" "$f" "Return value: \(long\) $N = 1137\$" "Return value: \(long\) $N = 406842\$" \
    "Return value: \(long\) $N = 7323158\$"
  expect "$L exit" "$f" 'received signal: SIGTRAP' 'exited with status = 33 '
  if has "$f" '^error:'; then result FAIL "$L no errors" "$(grep -a -m3 '^error:' "$f" | tr '\n' ' ')"; fi
}

check_rtos() {  # check_rtos <label> <log> <isr> <threads ERE...>
  local L=$1 f=$2.txt isr=$3; shift 3
  expect "$L irq-bt" "$f" "frame #1: 0x[0-9a-f]+ [^ ]+\`$isr " 'frame #2: 0x' \
    'This UnwindPlan is for a trap handler function: yes\.'
  if same_line "$f"; then result PASS "$L irq-bt interrupted frame" "frame #2 is shown at its PC's line"
  else result FAIL "$L irq-bt interrupted frame" "frame #2's line differs from its PC's (see $f)"; fi
  expect "$L threads" "$f" "$@"
  if has "$f" '^error:'; then result FAIL "$L no errors" "$(grep -a -m3 '^error:' "$f" | tr '\n' ' ')"; fi
}

# ---- run ---------------------------------------------------------------------
DBG_SRC=$RT/baremetal/programs/dbgtest.c
LINE_DIST2_CALL=$(grep -n 'long d = dist2(&p, &q);' "$DBG_SRC" | cut -d: -f1)
LINE_SQUARES=$(grep -n 'return square(dx) + square(dy);' "$DBG_SRC" | cut -d: -f1)
LINE_KEEPER=$(grep -n 'results\[6\] = keeper(' "$DBG_SRC" | cut -d: -f1)
LINE_BUSY=$(grep -n 'k_busy_wait(' "$RT/zephyr-ws/zephyr/samples/synchronization/src/main.c" | head -1 | cut -d: -f1)
LINE_TASKB=$(grep -n 'safe_printf("\[B\]' "$RT/freertos/demo/demo.c" | head -1 | cut -d: -f1)

declare -A ELF
for p in $PROGS; do
  case $p in
  dbgtest)
    for O in $OPTS; do
      if build_dbgtest "$O"; then ELF[dbgtest$O]=$OUT/build/dbgtest$O/dbgtest.elf
      else result FAIL "build dbgtest $O" "see $OUT/build/dbgtest$O/build.log"; fi
    done ;;
  zephyr)
    if build_zephyr; then ELF[zephyr]=$OUT/build/zephyr/zephyr.elf
    else result FAIL "build zephyr dbg_synchronization" "see $OUT/build/zephyr.log"; fi ;;
  freertos)
    if build_freertos; then ELF[freertos]=$OUT/build/freertos/demo.elf
    else result FAIL "build freertos demo" "see $OUT/build/freertos/build.log"; fi ;;
  *) die "unknown program '$p' (dbgtest zephyr freertos)" ;;
  esac
done

for where in $WHERE; do
  case $where in emu|board) ;; *) die "unknown target '$where' (emu, board)" ;; esac
  D=$OUT/$where; mkdir -p "$D"
  for p in $PROGS; do
    case $p in
    dbgtest)
      for O in $OPTS; do
        [ -n "${ELF[dbgtest$O]:-}" ] || continue
        session "$where" "${ELF[dbgtest$O]}" "$HERE/lldb/e2e.lldb" "$D/dbgtest$O" \
          "s|@LINE_DIST2_CALL@|$LINE_DIST2_CALL|g" "s|@LINE_SQUARES@|$LINE_SQUARES|g"
        check_dbgtest "$where dbgtest $O" "$D/dbgtest$O" "$O"
      done ;;
    zephyr)
      [ -n "${ELF[zephyr]:-}" ] || continue
      session "$where" "${ELF[zephyr]}" "$HERE/lldb/zephyr.lldb" "$D/zephyr" "s|@LINE_BUSY@|$LINE_BUSY|g"
      check_rtos "$where zephyr" "$D/zephyr" z_klausscpu_timer_isr \
        "name = 'thread_a \[" "name = 'thread_b \[" "name = 'idle \[" \
        "frame #0: 0x[0-9a-f]+ zephyr\.elf\`hello_loop\(.* at main\.c:$LINE_BUSY:" \
        'frame #1: 0x[0-9a-f]+ zephyr\.elf`thread_a_entry_point\(' 'z_thread_entry\(' \
        'thread_b_entry_point\(' '^\(unsigned int\) \$[0-9]+ = 1$' '^\(unsigned int\) \$[0-9]+ = 2$' \
        '^\(int\) \$[0-9]+ = 7$' ;;
    freertos)
      [ -n "${ELF[freertos]:-}" ] || continue
      session "$where" "${ELF[freertos]}" "$HERE/lldb/freertos.lldb" "$D/freertos" "s|@LINE_TASKB@|$LINE_TASKB|g"
      check_rtos "$where freertos" "$D/freertos" vPortTickISR \
        "name = 'A \[" "name = 'B \[Running\]'" "name = 'C \[" "name = 'Reporter \[" "name = 'IDLE \[" \
        "TaskB\(pvParam=.*\) at demo\.c:$LINE_TASKB" '\(char\[[0-9]+\]\) \$[0-9]+ = "B"' ;;
    esac
  done
done
echo "lldb e2e: $(grep -c '^PASS' "$RES") passed, $(grep -c '^FAIL' "$RES") failed"
exit $fail
