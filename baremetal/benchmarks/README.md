# Benchmarks: CoreMark and Embench-IoT on KlaussCPU

This directory holds the KlaussCPU bare-metal ports of two standard embedded
benchmarks. The upstream sources are not in this repo.

| Benchmark   | Upstream                                  | Pinned                                         | Licence |
|-------------|-------------------------------------------|------------------------------------------------|---------|
| CoreMark    | github.com/eembc/coremark                 | `v1.01` = `cfa9ab377835911f23d9b0831c7be302ed1f58de` | Apache-2.0 + EEMBC CoreMark acceptable-use agreement |
| Embench-IoT | github.com/embench/embench-iot            | `embench-1.0` = `0466a18e4f6b47e19598d7c6ba72916d54b68f65` | GPL-3.0-or-later, plus a licence per benchmark |

`get-benchmarks.sh` clones both at those commits into `coremark/` and
`embench-iot/`, which are gitignored. Only these files are committed:

```
get-benchmarks.sh          fetch upstream sources (pinned)
benchmarks.mk              make rules, included by ../Makefile
coremark-port/             core_portme.c / core_portme.h
embench-port/              boardsupport.c / boardsupport.h, dummy-libklausscpu.c
run-benchmarks.sh          build + run everything on the board, collect results
embench-score.py           Embench speed/size scores from a run
RESULTS.md                 board results
```

## Build and run

All commands run in `baremetal/`, with `KLAUSSCPU_LLVM_BIN` set as for every
other program. `OPT` picks the optimisation level, and each level builds into its
own tree under `bench-build/<OPT without the dash>/`.

```sh
benchmarks/get-benchmarks.sh                 # once

make coremark OPT=-O2   # bench-build/O2/coremark.elf, bench-build/O2/coremark_validate.elf
make embench  OPT=-O2   # bench-build/O2/embench/<name>.elf        (speed, run on the board)
                        # bench-build/O2/embench-size/<name>.elf   (size, never run)
make embench-sizes OPT=-O2          # .text table of the size builds
make embench-crc32 OPT=-O2          # one benchmark (speed + size ELF)
make bench-clean                    # rm -rf bench-build (also done by `make clean`)

klausscc -i bench-build/O2/coremark.elf          -s -m </dev/null   # ~21 s at -O2
klausscc -i bench-build/O2/coremark_validate.elf -s -m </dev/null
klausscc -i bench-build/O2/embench/crc32.elf     -s -m </dev/null   # < 1 s each
```

Both benchmarks also run in the emulator:
`klausscc -i <elf> --emulate --max-instructions 3000000000`. The emulator's
cycle counter is a model, so use it for checking correctness and the board for
timing.

The full suite builds, runs on the board, and writes `bench-results/<OPT>/`
(`*.out` UART captures, `coremark.csv`, `embench.csv`, `summary.txt`):

```sh
KLAUSSCC=/path/to/klausscc BOARD_LOCK=/path/to/board.lock \
    benchmarks/run-benchmarks.sh -O2 -Os -O1
```

It exits 0 only if every program verified. `BOARD_LOCK` (optional) wraps each
board command in `flock`. `RUN_COREMARK=0` or `RUN_EMBENCH=0` skips a suite.

The shared runtime objects (`crt0.o`, `syscalls.o`, `crt-*.o` …) are the
top-level ones in `baremetal/`. After a toolchain or ABI change, run
`make clean` as you would for any other program.

### Machine-readable output

Each program prints one CSV line, and its exit status (R12 at HALT) is 0 only
on a valid, verified run:

```
CSV,coremark,<iterations>,<cycles>,<instr>,<CoreMark*1000>,<CoreMark/MHz*1000>,<PASS|SHORT|FAIL>
CSV,embench,<name>,<cycles>,<instr>,<verify>,<PASS|FAIL|NOVERIFY>
```

CoreMark exits with 1 on a CRC mismatch and 2 (`SHORT`) if the CRCs matched but
the timed run was under the 10 s the run rules require. In Embench, `<verify>`
is the return value of `verify_benchmark()`: 1 correct, 0 wrong, -1 not
checked.

## Clock

The core clock is 100 MHz: the MIG `ui_clk` in 2:1 mode
(klaussCPU `README.md`: "ddr2_control.sv MIG wrapper (2:1, ui_clk = 100 MHz
CPU clock)"; `MEM_CLOCK_PLAN.md`). Both ports time with the 64-bit core cycle
counter `REG_PERF_CYCLES` (`mmio.h`, 0xF00D0008). They also read
`REG_PERF_INSTR`, which gives CPI. crt0 never enables interrupts, so the timed
window contains only benchmark code. No timer ISR runs in it.

## CoreMark port

- `core_portme.h`: KlaussCPU is LP64, so `ee_u32` is `unsigned int` and
  `ee_ptr_int` is `uintptr_t`. `CORE_TICKS` is the raw 64-bit cycle count.
  Settings: `HAS_FLOAT=1` (compiler-rt soft-float doubles, used only for the
  report), `HAS_PRINTF=1` (picolibc `printf` to the UART), `SEED_VOLATILE`,
  `MEM_STACK` and `MAIN_HAS_NOARGC`.
- `start_time()` and `stop_time()` read the cycle and instruction counters, and
  `time_in_secs()` divides by 100 MHz.
- `ITERATIONS` comes from `COREMARK_ITERATIONS` (default 2000). That is about 21 s
  at -O2 and 24 s at -Os on the current toolchain, so a compiler up to 2x faster
  still stays over 10 s. `COREMARK_ITERATIONS=0` makes `core_main.c` auto-size
  the run to at least 10 s. That costs an extra calibration pass of 1 to 10 s.
- `coremark.elf` is the performance run (`PERFORMANCE_RUN`, seeds 0,0,0x66), and
  `coremark_validate.elf` is the validation run (`VALIDATION_RUN`, seeds
  0x3415,0x3415,0x66). The run rules ask for both. `core_main.c` checks the
  list, matrix and state CRCs against its reference values for these seeds:
  - 2K performance: `crclist 0xe714`, `crcmatrix 0x1fd7`, `crcstate 0x8e3a`
  - 2K validation: `crclist 0xe3c1`, `crcmatrix 0x0747`, `crcstate 0x8d84`

  `crcfinal` depends on the iteration count. A host (x86-64 gcc) build of the
  same port gives 0x4983 (performance) and 0x0cac (validation) at 2000
  iterations, and the board matches both.
- `portable_fini()` prints the cycles, the instructions, CPI and CoreMark/MHz,
  then the CSV line, and sets the exit status.
- `core_main.c` prints `ee_u32` values with `%lu`, which raises a `-Wformat`
  warning on LP64. It still prints correctly, because KlaussCPU passes varargs
  in 64-bit slots. The upstream file is left unmodified, as the run rules
  require.

## Embench-IoT port

- `boardsupport.c`: `start_trigger()` and `stop_trigger()` snapshot the cycle
  and instruction counters. Embench's own `support/main.c` is compiled
  unmodified apart from `-Dmain=embench_main`. The port's `main()` calls it and
  then prints the result. The link uses `--wrap=verify_benchmark`, so the port
  sees `verify_benchmark()`'s return value.
- `CPU_MHZ` (`EMBENCH_CPU_MHZ`, default 1) is Embench's iteration multiplier
  (`LOCAL_SCALE_FACTOR * CPU_MHZ`). It is not the real clock. At 1, each
  benchmark does the reference work, which the reference Cortex-M4 does in about
  4 M cycles. The cycle counter gives exact counts, so the short run (0.03 to
  0.15 s) loses nothing. `WARMUP_HEAT=1` is Embench's default.
- Flags follow Embench: `-ffunction-sections` plus `--gc-sections`, and the
  repo's usual `$(CFLAGS)`. The reference platform uses -O2 for speed and -Os for
  size.
- **Size**, by Embench's method ("compile with dummy versions of all standard
  libraries; record the size of all `.text` sections"): each benchmark is linked
  a second time into `embench-size/<name>.elf`. That link uses Embench's
  `dummy-crt0.c`, `dummy-libc.c` and `dummy-libm.c`, the port's
  `dummy-libklausscpu.c`, `--gc-sections`, and lld's default layout, which keeps
  `.rodata` out of `.text`. The metric is that ELF's `.text`.
  `dummy-libklausscpu.c` stands in for Embench's `dummy-libgcc.c`, which only
  covers Arm's `__aeabi_*`. It holds stubs for the compiler-rt soft-float and
  integer helpers, plus `fabs`/`fabsf` (clang calls them on KlaussCPU, while Arm
  GCC inlines them) and picolibc's `_ctype_b` table.
- **Score**: `embench-score.py bench-results/<OPT>/embench.csv` follows Embench
  1.0. Relative speed = reference ms × 1000 / (cycles / CPU_MHZ), so with
  CPU_MHZ=1 it is the speed per MHz relative to the reference. Relative size =
  `.text` / reference `.text`. Each gets a geometric mean and a geometric SD. The
  reference data is in `embench-iot/baseline-data/`.
- LP64 note: some benchmarks use `long` for 32-bit data, for example crc32's
  table. That doubles some `.rodata` and `.bss`, but `.text` and correctness are
  not affected. Every benchmark verifies.
