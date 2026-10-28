# tools/validate — toolchain validation harness

The checks every klausscpu-llvm change goes through before it is committed:
the bare-metal corpus on the emulator, Csmith differential testing (on the
emulator, and on the board against the emulator), the C ABI conformance test,
a byte-for-byte "oracle" of the toolchain's output, the board suite with its
cycle counts, CoreMark and Embench-IoT, and the Zephyr apps.  One driver runs
the right subset and prints a PASS/FAIL summary:

```sh
export KLAUSSCPU_LLVM_BIN=<klausscpu-llvm>/build/bin
tools/validate/validate.sh quick            # ~1 min, emulator only
tools/validate/validate.sh full             # ~4 min, emulator + lit + oracle
BOARD_LOCK=/path/board.lock tools/validate/validate.sh board    # ~4 min on the board
BOARD_LOCK=/path/board.lock tools/validate/validate.sh zephyr   # ~15 min on the board
tools/validate/validate.sh csmith 300 120000          # 300 seeds from 120000
BOARD_LOCK=/path/board.lock tools/validate/validate.sh csmith-board 100 120000
tools/validate/validate.sh oracle [<previous oracle dir>]
```

Each run writes into `-o <outdir>` (default
`${VALIDATE_OUT:-${TMPDIR:-/tmp}/klausscpu-validate}/<mode>-<time>`), ends
with `validate <mode>: PASSED` or `FAILED`, and exits 0 or 1.  Every check's
log is in the output directory; `summary.txt` repeats the PASS/FAIL lines.

## What it needs

| | |
|---|---|
| `KLAUSSCPU_LLVM_BIN` | the toolchain's `bin/` (default `<runtime>/../klausscpu-llvm/build/bin`) |
| `KLAUSSCPU_LLVM_SRC` | the klausscpu-llvm source tree, for the compiler-rt sources, lit and the oracle's test inputs (default `$KLAUSSCPU_LLVM_BIN/../..`) |
| `KLAUSSCC` | klausscc (default `<runtime>/../klausscc/target/release/klausscc`, then `klausscc` on `PATH`).  The emulator and the board loader. |
| `BOARD_LOCK` | every board command runs under `flock $BOARD_LOCK`, so several sessions can share the board (default `${XDG_RUNTIME_DIR:-/tmp}/klausscpu-board.lock`; everyone sharing the board must use the same file) |
| `JOBS` | parallel jobs (default `nproc`) |
| `KLAUSSCPU_BOARD_IP`, `KLAUSSCPU_SSH_PASS` | the board's address for the Zephyr VNC grabs and SSH session (default 192.168.68.59, and ssh_server.c's default password) |
| `ZEPHYR_WS` | the west workspace (default `<runtime>/zephyr-ws`) |
| host tools | `gcc` with ASan/UBSan, `csmith` (and `/usr/include/csmith`), `python3` (PIL for the VNC grabs), `flock`, `ssh` |

The runtime checkout must have its gitignored prerequisites: `picolibc-install`
(`./build-picolibc.sh`), and for the network programs `lwip` (`./get-lwip.sh`);
for Zephyr the west workspace, the wolfSSL/wolfSSH sources and doomgeneric.
In a fresh clone, `validate.sh deps <other checkout>` symlinks all of them
from an existing checkout instead.  The benchmark sources are fetched on
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
`common.sh`).  A program that does not halt (the network demos, adventure)
is compared on its first 12 lines.  It also prints the emulator instruction
count and `.text` size of every program that changed, the totals, and a WARN
for any program that grew by more than `STATS_TOL` (2%).  Then 4 ABI
conformance seeds and 30 Csmith seeds.

**full** — quick, plus 300 Csmith seeds at -O0/-O1/-O2/-Os (from
`CSMITH_SEED`, default 1), 150 `--float` seeds, 30 ABI seeds, the KlaussCPU
lit tests (`llvm/test/{CodeGen,MC,MC/Disassembler,DebugInfo,Transforms/LoopUnroll}/KlaussCPU`,
`lld/test/ELF/klausscpu-*`, `clang/test/{CodeGen,Driver,Preprocessor}/klausscpu-*`)
and `KlaussCPUTests`, and an oracle snapshot, diffed against `ORACLE_BASE`
when that is set.

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
`ZEPHYR_APPS` picks a subset.

**csmith N [FIRST]** — `csmith_run.sh`: each program's checksum from host gcc
-O0 against KlaussCPU clang at -O0/-O1/-O2/-Os on the emulator.  Programs
with undefined behaviour that the host can see are discarded first: the host
build under `-fsanitize=undefined,address,float-cast-overflow` must run clean
and print the same checksum (status `host-ub`).  Packed structs are off.
Results in `csmith/results.tsv` (`seed opt status host target`), failing
seeds' sources in `csmith/fail/`.  `CSMITH_OPTS=--float` for the FP runs.
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
(`skip-big`); Csmith's default programs need well under 1M instructions and
load in under a second.

**abi [N] [FIRST]** — `abi-test/run_abi_test.sh` (generated caller/callee
pairs in separate translation units, every mix of -O0/-O1/-O2/-Os, against
host gcc).

**oracle [BASE]** — `oracle.sh` snapshots the toolchain's output bytes: the
corpus objects and ELFs at -O1/-O2/-Os with their disassembly, llvm-mc on the
ISA coverage inputs (`oracle-inputs/`, generated by the ISA coverage tool)
and on the MC tests with short forms on and off, disassembly of the
Disassembler tests, and llc -O0/-O1/-O2 of every CodeGen test.
`oracle_diff.sh A B` lists every file that differs.  Use it to show that a
refactor changes no output.

**baseline DIR...** — after an intended change of output or performance,
rewrite the baselines from the output directories of quick, board and/or
zephyr runs: `baselines/emu-O1`, `emu-O2`, `board-O1` (filtered UART text and
`stats.tsv`) and the values in `baselines/perf.tsv` (direction and tolerance
of each existing metric are kept).  Commit the result with the change.

## Baselines

`baselines/emu-O1/`, `baselines/emu-O2/`: the filtered UART output of every
bare-metal program, and `stats.tsv` (emulator instructions, `.text` bytes).
`baselines/board-O1/`: the same for the board suite.  `baselines/perf.tsv`:
one board metric per line — key, value, the better direction (`lower`,
`higher`, or `info`: reported, never judged) and the tolerance in percent.
`validate.sh board` and `zephyr` flag a metric that got worse by more than its
tolerance as a REGRESSION (and fail).  The board's cycle counts repeat to
within a few cycles (kernels that miss the cache vary a little with DDR
refresh), so the bare-metal tolerances are tight (0.2-0.5%); the Zephyr
figures are wall-clock rates and are looser (1-10%).  The file's comments
name the toolchain each figure came from.

## Scripts

| Script | |
|---|---|
| `validate.sh` | the driver |
| `common.sh` | tool lookup, `board`/`emu` helpers, `uart_filter`, the staged build tree |
| `emu_suite.sh <out> [OPT]` | build and run the corpus on the emulator |
| `compare.sh <base> <new>` | UART, instruction-count and size comparison of two runs, or of a run and a baseline dir |
| `board_suite.sh <out> [OPT]` | build and run the board programs (`BOARD_RUNS`, `BOARD_PROGS`) |
| `bench.sh <out> [OPT...]` | CoreMark + Embench-IoT on the board (`baremetal/benchmarks/run-benchmarks.sh`) |
| `perf.py` | extract board figures; compare with or merge into a baseline |
| `csmith_run.sh`, `csmith_board.sh`, `csmith_lib.sh` | Csmith on the emulator / on the board and the emulator |
| `oracle.sh`, `oracle_diff.sh` | output-byte snapshot and diff |
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
