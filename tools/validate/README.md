# tools/validate — toolchain validation harness

The checks every klausscpu-llvm change goes through before it is committed:
the bare-metal corpus on the emulator, Csmith differential testing (on the
emulator, and on the board against the emulator), the C ABI conformance test,
a byte-for-byte "oracle" of the toolchain's output, the board suite with its
cycle counts, CoreMark and Embench-IoT, the Zephyr apps, and lldb end to end
on the emulator and the board.  One driver runs
the right subset and prints a PASS/FAIL summary:

```sh
export KLAUSSCPU_LLVM_BIN=<klausscpu-llvm>/build/bin
tools/validate/validate.sh quick            # ~1 min, emulator only
tools/validate/validate.sh full             # ~4 min, emulator + lit + lldb + oracle
BOARD_LOCK=/path/board.lock tools/validate/validate.sh board    # ~4 min on the board
BOARD_LOCK=/path/board.lock tools/validate/validate.sh zephyr   # ~15 min on the board
tools/validate/validate.sh csmith 300 120000          # 300 seeds from 120000
BOARD_LOCK=/path/board.lock tools/validate/validate.sh csmith-board 100 120000
tools/validate/validate.sh oracle [<previous oracle dir>]
tools/validate/validate.sh lldb             # ~15 s, lldb on the emulator
BOARD_LOCK=/path/board.lock tools/validate/validate.sh lldb-board  # ~15 s on the board
```

Each run writes into `-o <outdir>` (default
`${VALIDATE_OUT:-${TMPDIR:-/tmp}/klausscpu-validate}/<mode>-<time>`), ends
with `validate <mode>: PASSED` or `FAILED`, and exits 0 or 1.  Every check's
log is in the output directory; `summary.txt` repeats the PASS/FAIL lines.
A full run needs about 300 MB in the output directory while it runs (the
oracle alone is 80 MB).  On a tmpfs `/tmp` with a per-user quota, a full
quota makes builds and links fail ("Disk quota exceeded", even a linker bus
error): that shows up as FAILs, never as a PASS, but it is not a compiler
problem.

## What it needs

| | |
|---|---|
| `KLAUSSCPU_LLVM_BIN` | the toolchain's `bin/` (default `<runtime>/../klausscpu-llvm/build/bin`) |
| `KLAUSSCPU_LLVM_SRC` | the klausscpu-llvm source tree, for the compiler-rt sources, lit and the oracle's test inputs (default `$KLAUSSCPU_LLVM_BIN/../..`) |
| `KLAUSSCC` | klausscc (default `<runtime>/../klausscc/target/release/klausscc`, then `klausscc` on `PATH`).  The emulator and the board loader. |
| `BOARD_LOCK` | every board command runs under `flock $BOARD_LOCK`, so several sessions can share the board (default `${XDG_RUNTIME_DIR:-/tmp}/klausscpu-board.lock`; everyone sharing the board must use the same file, and the board modes print the one they use in their first lines) |
| `JOBS` | parallel jobs (default `nproc`) |
| `KLAUSSCPU_BOARD_IP`, `KLAUSSCPU_SSH_PASS` | the board's address for the Zephyr VNC grabs and SSH session (default 192.168.68.59, and ssh_server.c's default password) |
| `ZEPHYR_WS` | the west workspace (default `<runtime>/zephyr-ws`) |
| host tools | `gcc` with ASan/UBSan, `csmith` (and `/usr/include/csmith`), `python3` (PIL for the VNC grabs), `flock`, `ssh` |

The runtime checkout must have its gitignored prerequisites: `picolibc-install`
(`./build-picolibc.sh`), and for the network programs `lwip` (`./get-lwip.sh`);
for Zephyr the west workspace, the wolfSSL/wolfSSH sources and doomgeneric.
In a fresh clone, `validate.sh deps <other checkout>` symlinks all of them
from an existing checkout instead (and lists the links in
`.git/info/exclude`, so `git status` and `git add -A` leave them alone).  The benchmark sources are fetched on
first use if `baremetal/benchmarks/{coremark,embench-iot}` are missing.

The harness never builds in the checkout: each bare-metal build runs in a
staged copy of `baremetal/` under the output directory (the other top-level
directories are symlinked), so it can run next to development work and
several runs can go at once.  `make clean` in `baremetal/` is not needed.

## Modes

**quick** — `emu_suite.sh` builds every bare-metal program (`make all`) at
-O1 and at -O2 and runs each on the emulator (800M-instruction cap).
`compare.sh` compares the UART output with `baselines/emu-O1` and
`baselines/emu-O2`, after filtering the lines that change legitimately with
code generation (cycle and time figures, addresses, the loader's lines, the
interrupt race count in test_atomic, Zephyr log timestamps: `uart_filter` in
`common.sh`; a line that starts with `PASS` or `FAIL` is always kept).
How each run ended must match the baseline too (`baselines/*/stop.tsv`:
`Halt`, `InstructionCap`, `Trap`, `InvalidOpcode`, ...): a program that used
to halt and now hangs, traps or is killed fails with `STOP <prog>: Halt ->
<new>` even when every line it printed is the same.  A program whose
baseline stop is `InstructionCap` (it idles or waits for a peripheral the
emulator does not model, so the cap stops it; today test_rtos, test_sync,
adventure, blit_capture, crypto_selftest, crypto_test, eth_test and the six
network demos, see `stop.tsv`) prints more or less before the cap, and
interleaves its tasks differently, as the code gets faster or slower.  It
is compared on its first 12 lines (in any order: a reordering is listed as
`ORDER` but does not fail) and on its self-check verdicts anywhere in the
output: the distinct lines with a `PASS` or `FAIL` word or `Result:`,
digits ignored (`verdicts` in `common.sh`).  Every verdict of the baseline
must still be printed (`VERDICT <prog> lost: Result: PASS` fails, e.g.
when test_rtos's line 39 turns FAIL or never comes), and a new verdict
that says FAIL fails (`VERDICT <prog> new:`); a new PASS verdict, from a
run that now gets further, is only listed.  Every other program is
compared on its whole output.  The summary line reads `uart: N programs, D
differ, S stop differently, M missing`.  It also prints the emulator instruction count and `.text` size of every program
that changed, the totals, and a WARN for any program that grew by more than
`STATS_TOL` (2%).  Then 4 ABI conformance seeds and 30 Csmith seeds.

**full** — quick, plus 300 Csmith seeds at -O0/-O1/-O2/-Os/-Oz (from
`CSMITH_SEED`, default 1), 150 `--float` seeds, 30 ABI seeds, the KlaussCPU
lit tests (`llvm/test/{CodeGen,MC,MC/Disassembler,DebugInfo,Transforms/LoopUnroll}/KlaussCPU`,
`lld/test/ELF/klausscpu-*`, `clang/test/{CodeGen,Driver,Preprocessor}/klausscpu-*`,
and when the build has lldb and its tests (`LLDB_INCLUDE_TESTS=ON`) every
`KlaussCPU` directory or `klausscpu-*` file under `lldb/test`) and
`KlaussCPUTests`, lldb end to end on the emulator (the `lldb` mode), and an
oracle snapshot, diffed against `ORACLE_BASE` when that is set (a failed
`oracle.sh` is a FAIL).

**board** — `board_suite.sh` builds the self-checking programs at -O1, plus
Dhrystone with `-fno-builtin` (`dhrystone_nobuiltin`, the like-for-like
figure: since strcpy/strcmp became built-ins the default build folds
Dhrystone's string work), and runs each twice on the board.  It checks that
every program halts (test_rtos and test_sync idle instead, and are stopped
after 12 s), that the UART output matches `baselines/board-O1`, and that the
two runs agree.  Then `bench.sh` runs CoreMark and Embench-IoT at -O2 and -Os
(`BENCH=0` skips them); every CoreMark CRC and Embench verification must
pass.  `perf.py` turns the captures into numbers (`perf.tsv` in the output)
and compares them with `baselines/perf.tsv`.

**zephyr** — `zephyr/suite.sh` builds the Zephyr apps pristine (hello,
synchronization with debug info, irq_soak, llext_demo, mandel, gui_demo,
gui_lvgl, doom, ssh_shell), runs each on the emulator for 200M instructions
and on the board: the GUI apps with a VNC grab (`<app>.png`), ssh_shell with
an SSH session (`kernel version`, `kernel uptime`).  The expected lines are
in `zephyr/expect-emu.txt` and `zephyr/expect-board.txt`; the mandel, doom,
irq_soak and gui_lvgl figures go through `perf.py` like the board's.
`ZEPHYR_APPS` picks a subset, and `ZEPHYR_CMAKE_ARGS` adds `-D` options to
every build (`-DCONFIG_SIZE_OPTIMIZATIONS_AGGRESSIVE=y` builds at -Oz,
`-DCONFIG_LTO=y` with LTO; the expectations are the same).

**csmith N [FIRST]** — `csmith_run.sh`: each program's checksum from host gcc
-O0 against KlaussCPU clang at -O0/-O1/-O2/-Os/-Oz (-Oz runs the
MachineOutliner) on the emulator; `csmith_run.sh <first> <count> <dir>
[level...]` takes other levels, and a level can carry more flags joined by
`+` (`-Os+-moutline`).  Programs
with undefined behaviour that the host can see are discarded first: the host
build under `-fsanitize=undefined,address,float-cast-overflow` must run clean
and print the same checksum (status `host-ub`).  Packed structs are off.
Results in `csmith/results.tsv` (`seed opt status host target
instructions`), failing seeds' sources, outputs and ELFs in `csmith/fail/`.
A run must end in HALT: a trap or other abnormal stop is `stop-<reason>`
(a failure) even after a correct checksum.  When one level runs past the
2G-instruction cap while another level of the same seed halts with the
right checksum, that level is run again with `EMU_RETRY_MAX` (2e10, about
two minutes): it must then halt with the right checksum (`ok-long`) or the
seed fails as `HANG`.  A slow -O0 build of a loop the optimiser removed is
the usual reason (seed 120376 with `--float`: 103K instructions at -O1,
10.4G at -O0); a miscompiled infinite loop is the one to catch.  When every
level hits the cap, the program is just long (`instr-cap`, not a failure).
`CSMITH_OPTS=--float` for the FP runs.  Csmith's `--float` programs often
pass a `float *` where an `int32_t *` is expected (128 of 150 seeds from
121300; no integer seed tried does): reading a float through an int lvalue
is aliasing UB the sanitizers cannot see, and TBAA then changes the result
(seed 121439: a different checksum at -O1 and above, the same one on host
clang -O1 for x86, the right one with `-fno-strict-aliasing`).  A seed
whose source has incompatible pointer types is therefore built with
`-fno-strict-aliasing` (listed in `csmith/no-strict-aliasing.txt`, counted
in the summary); every other seed keeps TBAA.
The C library follows the toolchain's ABI (the low byte of `e_flags`): an
ABI-0 toolchain (before klausscpu-llvm's ABI v2) links
`picolibc-install-oldabi`, since lld refuses to mix ABI versions.

**csmith-board N [FIRST]** — `csmith_board.sh`: one ELF per seed and level
(-O1 -O2), run on the emulator and on the board, so the RTL is compared with
the emulator on random code.  Each result is `all-equal`; `BOARD-EMU`
(board != emulator: an RTL or emulator bug; the ELF, source and both
outputs are kept in `csmith-board/board-emu/`); `EMU-HOST` (board = host !=
emulator: an emulator bug, kept there too); or `COMPILER` (emulator = board
!= host: a compiler bug, in `csmith-board/compiler/`).  Seeds whose emulator
run exceeds `EMU_MAX_INSTRUCTIONS` (300M) are not run on the board
(`skip-long`), nor are programs with more than `MAX_TEXT` bytes of .text
(`skip-big`, only when the emulator's checksum is right; a big program with
a wrong one is `COMPILER`).  A `skip-long` level of a seed whose other level was
`all-equal` gets the same `EMU_RETRY_MAX` re-run on the emulator as above
(`skip-long-ok`, or `HANG`).  The board run must print `CPU halted` too.
Csmith's default programs take about 150K instructions on average and a few
million at most (2.5M in 858 runs), and load in under a second.

**abi [N] [FIRST]** — `abi-test/run_abi_test.sh` (generated caller/callee
pairs in separate translation units, every mix of -O0/-O1/-O2/-Os/-Oz
(`OPTS`, with the same `+` syntax), against host gcc).

**lldb / lldb-board** — `lldb_e2e.sh <outdir> emu|board`: lldb (from
`KLAUSSCPU_LLVM_BIN`) against `klausscc --gdbserver`, each session scripted
with `lldb -b -s` (`lldb/*.lldb`, with the source lines substituted) and its
output checked line by line.  `lldb/e2e.lldb` runs `dbgtest`
(`baremetal/programs/dbgtest.c`) built with -g at -O0, -O1 and -O2: line and
function breakpoints, step-in / finish (with return values) / next,
backtraces through a frameless leaf, a shrink-wrapped early exit and a tail
call (an artificial frame from -O1 on), frame variables and r4-r7 of outer
frames whose values live in callee-saved registers (restored from the
CFI), expressions, the exit status.  `lldb/zephyr.lldb` (the synchronization
sample with debug info and thread names, `zephyr/build.sh
dbg_synchronization`) and `lldb/freertos.lldb` (`freertos/demo` with -g,
built in a staged copy of `freertos/`) load the RTOS OS plugin
(`debug/klausscpu_rtos.py`): a backtrace from the timer interrupt's C
handler through the assembly handler (a trap handler frame) into the
interrupted code, which must be shown at its PC's own line; the thread list,
every thread named with its state and with its own stack; expressions over
kernel objects.  `lldb-board` needs the board's `DEBUG_EN=1` bitstream and a
klausscc built with `--features board` (`KLAUSSCC`); each session holds
`BOARD_LOCK`.  `E2E_OPTS`, `E2E_PROGS` (dbgtest zephyr freertos) and
`E2E_PORT` narrow or move it.  `full` runs the emulator half.

**oracle [BASE]** — `oracle.sh` snapshots the toolchain's output bytes: the
corpus objects and ELFs at -O1/-O2/-Os with their disassembly, llvm-mc on the
ISA coverage inputs (`oracle-inputs/`, generated by the ISA coverage tool)
and on the MC tests with short forms on and off, disassembly of the
Disassembler tests, and llc -O0/-O1/-O2 of every CodeGen test.
`oracle_diff.sh A B` lists every file that differs.  Use it to show that a
refactor changes no output.

**selftest [EMU_RUN [BOARD_RUN]]** — `selftest.sh`, fault injection for
the harness itself: it tampers with copies of a -O1 emulator run (and of a
board run when one is given) and checks that `compare.sh` catches a program
that hangs part-way, traps after its last line, is killed, starts halting,
changes an early line, or goes missing, a `PASS` line that turns into
`FAIL`, a capped program (test_rtos, test_sync) whose late verdict turns
`FAIL`, that stops before its verdicts or that prints a new `FAIL` (and
that a capped program that gets less far, interleaves differently or
reaches a new `PASS` still passes), and a board program that stops halting;
then that Csmith flags a trap after a good checksum and a level that only
halts with the larger cap (`ok-long`) or not at all (`HANG`); that
csmith-board flags a wrong emulator checksum in a program too big for the
board (`COMPILER`, not `skip-big`); and that `perf.py` fails a metric that
is no longer measured.  Run it after changing any script here.

**baseline DIR...** — after an intended change of output or performance,
rewrite the baselines from the output directories of quick, board and/or
zephyr runs: `baselines/emu-O1`, `emu-O2`, `board-O1` (filtered UART text and
`stats.tsv`) and the values in `baselines/perf.tsv` (direction and tolerance
of each existing metric are kept).  Commit the result with the change.

## Baselines

`baselines/emu-O1/`, `baselines/emu-O2/`: the whole filtered UART output of
every bare-metal program (also of those the cap stops, for their verdicts),
`stats.tsv` (emulator instructions, `nohalt` for a
program that ran into the cap, `.text` bytes) and `stop.tsv` (how each run
ended).  `baselines/board-O1/`: the UART output and `stop.tsv` of the board
suite (`Halt`, or `none` for test_rtos and test_sync, which idle until the
timeout).  `baselines/perf.tsv`:
one board metric per line — key, value, the better direction (`lower`,
`higher`, or `info`: reported, never judged) and the tolerance in percent.
`validate.sh board` and `zephyr` flag a metric that got worse by more than its
tolerance as a REGRESSION (and fail).  A judged metric that the run did not
produce while it measured others of its group (the first two parts of the
key: `board.O1`, `bench.O2`, `zephyr.mandel`) is MISSING and fails too, so
a Dhrystone or perf_baseline line that stops being printed, or changes
format, does not pass silently; a group that was not run at all (`BENCH=0`,
a `ZEPHYR_APPS` subset) is only listed as not measured.  The board's cycle counts repeat to
within a few cycles (kernels that miss the cache vary a little with DDR
refresh), so the bare-metal tolerances are tight (0.2-0.5%); the Zephyr
figures are wall-clock rates and are looser (1-10%).  The file's comments
name the toolchain each figure came from.

### Expected FAIL lines

The baselines are what the programs print today, not a list of what works,
and some of it is a known failure that no compiler change should alter; a
change in any of it is a regression like any other.

- Emulator (no model of these peripherals): `test_sd`, `fs_demo`, `loader`
  and the FatFs part of `test_big` (no SD card: `f_mount` fails with 3);
  `blit_selftest` and `blit_capture` (no blitter); `crypto_test` (no
  AES engine: the result reads 0); `test_eth` (`[FAIL] scratch`: the
  Ethernet controller's scratch register reads 0, and the test stops
  there); `test_cache` (`Geometry check: FAIL`: the cache-info register
  reads 0).
- Board: `test_cache` `Geometry check: FAIL` (the test expects 2048 sets,
  the cache reports 2 ways x 1024 sets, 64 KB: a stale expectation in the
  test); `test_big` `FAIL read hello.txt` (the SD card in the board has no
  `hello.txt`; the other FatFs checks pass).

## Scripts

| Script | |
|---|---|
| `validate.sh` | the driver |
| `common.sh` | tool lookup, `board`/`emu` helpers, `uart_filter`, the staged build tree |
| `emu_suite.sh <out> [OPT]` | build and run the corpus on the emulator |
| `compare.sh <base> <new>` | UART, instruction-count and size comparison of two runs, or of a run and a baseline dir (needs no toolchain) |
| `board_suite.sh <out> [OPT]` | build and run the board programs (`BOARD_RUNS`, `BOARD_PROGS`) |
| `bench.sh <out> [OPT...]` | CoreMark + Embench-IoT on the board (`baremetal/benchmarks/run-benchmarks.sh`) |
| `perf.py` | extract board figures; compare with or merge into a baseline |
| `csmith_run.sh`, `csmith_board.sh`, `csmith_lib.sh` | Csmith on the emulator / on the board and the emulator |
| `oracle.sh`, `oracle_diff.sh` | output-byte snapshot and diff |
| `selftest.sh <out> <emu-O1 run> [<board-O1 run>]` | fault injection for compare.sh, perf.py and the Csmith scripts |
| `zephyr/suite.sh` | Zephyr build, emulator and board runs; `zephyr/build.sh <app> <dir>` builds one app; `zephyr/sshrun.sh` drives an SSH session; `zephyr/vncgrab.py` grabs a VNC frame |

The perf_haz program (the per-kernel hazard breakdown: data, load-use,
flags, SP, mul/div, branch flush, I-fetch miss and memory-wait stall cycles)
runs in both the emulator corpus and the board suite; its `HAZ` lines land in
`perf.tsv` as `info` metrics.

## Reducing a failure

A Csmith failure keeps `t.c` and the outputs.  `cvise` is installed; an
interestingness test builds the program at the failing level, runs it with
`klausscc --emulate` (or on the board, under the lock) and checks that the
checksum still differs while host gcc's UBSan/ASan build still runs clean.
