# Benchmark results: CoreMark and Embench-IoT on the KlaussCPU board

How the benchmarks are built and run, and how they are scored, is in
[README.md](README.md). These numbers are an interim baseline. The compiler
work under way will change them, and they will be measured again when it
lands.

## Milestone 5: toolchain `759085ccb9c3` (2026-10-28)

The default levels after klausscpu-llvm's phase 5 (under-aligned loads,
the MUL/DIV EX-hold scheduling fix, 64-bit compares for branches on 32-bit
arithmetic, the MachineOutliner at -Oz, LTO), with picolibc and the
builtins rebuilt by that toolchain.  `tools/validate/bench.sh -O2 -Os`, run
twice with identical results.  "Was" is `65f26a14dd03` (milestone 4).

| OPT | CoreMark | CoreMark/MHz | CoreMark CPI | Embench speed | Embench size | Verification |
|---|---:|---:|---:|---:|---:|---|
| -O2 | 101.09 (was 101.13) | 1.011 | 2.95 | 0.554 (was 0.555) | 2.259 (was 2.269) | CoreMark perf + validation CRCs OK; Embench 19/19 PASS |
| -Os | 89.07 (was 88.14, +1.1%) | 0.891 | 3.02 | 0.501 (was 0.501) | 1.597 (was 1.603) | CoreMark perf + validation CRCs OK; Embench 19/19 PASS |

CoreMark -O2: 1978438597 cycles, 669990599 instructions; -Os: 2245472926
cycles, 742926155 instructions.  Embench nbody (+3.2% cycles) and cubic
(+1.6% at -O2, +1.9% at -Os) are slower because of picolibc's `sqrt`
alone: the rebuilt library saves one instruction there (SHLV + CMPLTV
becomes CMPLTVW), which moves its bit loop by 4 bytes, so two 8-byte
instructions in the loop now straddle a 16-byte fetch line.  nbody runs
0.9% fewer instructions.  Relinking with the milestone-4 `libc.a` gives the
milestone-4 cycle counts exactly.  The fetch-line bubble is proposal 3 in
klausscpu-llvm's `HARDWARE_PROPOSALS.md`.

## Code size: MachineOutliner and LTO (2026-10-28)

klausscpu-llvm's 'size-opts' step: the MachineOutliner (on by default at
-Oz only; `-moutline` / `-mno-outline` override it) and link-time
optimisation (`make LTO=1`, off by default).  Board figures are
`tools/validate/bench.sh` runs, each configuration run twice with identical
results; "corpus" is the 39 bare-metal programs on the emulator (`.text`
includes picolibc and the builtins, which were not rebuilt).

**MachineOutliner**

| | CoreMark | Embench speed | Embench size | corpus `.text` | corpus instructions |
|---|---:|---:|---:|---:|---:|
| -Os | 89.07 | 0.503 | 1.597 | 1,026,952 | 292,355,163 |
| -Os -moutline | 78.96 (-11.4%) | 0.485 (-3.6%) | 1.475 (-7.6%) | -3.52% | +2.13% |
| -Oz -mno-outline | 85.41 | 0.489 | 1.571 | 1,005,048 | 301,599,437 |
| -Oz (outliner, the default) | 71.64 (-16.1%) | 0.469 (-4.1%) | 1.460 (-7.0%) | -2.74% | +0.60% |

The speed cost is the call and return around outlined code in hot loops
(nsichneu +38% cycles, qrduino +11%, picojpeg +7..15%), so the outliner is a
-Oz feature; at -Os the size gain does not justify an 11% CoreMark loss.
nsichneu's `.text` halves (16680 -> 7180 bytes).

**LTO** (`make LTO=1`: the programs are bitcode, ld.lld optimises each
whole; the compiler-rt builtins, `atomic.o`, assembly, picolibc and
Embench's support code stay native)

| corpus | `.text` | instructions |
|---|---:|---:|
| -O1 | +1.96% | -0.14% |
| -O2 | +2.04% | +1.41% (dhrystone +4.7%) |
| -Os | -3.76% | -0.27% |
| -Oz | -4.37% (against -Oz, both with the outliner) | +0.0% |

Board: CoreMark -O2 101.95, -Os 88.25, -Oz 72.80 (+0.8%, -0.9%, +1.6%);
Dhrystone `-fno-builtin` at -O1 1488 cycles/run against 1470 (+1.2%),
perf_baseline unchanged.  Embench is not built for whole-program
optimisation: with its data internalised, LTO folds cubic at -O2 (167
cycles) and most of statemate (-85% cycles, `.text` 7732 -> 772 bytes at
-Os), so its LTO geomeans (speed 1.098 / 0.585 / 0.536, size 2.417 / 1.250 /
1.155 at -O2 / -Os / -Oz, 19/19 PASS) are not comparable.  Per benchmark at
-Os the rest moves by -24%..+3% in cycles and -73%..+4% in size; wikisort
gets its -O2 code back (see below).  So LTO pays for size at -Os/-Oz and not
at -O1/-O2, and stays off by default.

**Zephyr** (`text` bytes; `tools/validate/zephyr/suite.sh` with
`ZEPHYR_CMAKE_ARGS`; every app passes on the emulator in each
configuration, and -Oz with LTO also on the board).  The port builds
`CONFIG_SIZE_OPTIMIZATIONS` at -O2 (`cmake/compiler/clang/compiler_flags.cmake`);
`CONFIG_SIZE_OPTIMIZATIONS_AGGRESSIVE` gives -Oz, `CONFIG_LTO` works with
either since klausscpu-llvm passes the LTO options to lld.

| app | -O2 | -O2 LTO | -Oz | -Oz LTO |
|---|---:|---:|---:|---:|
| hello | 10544 | 10444 | 9848 | 7900 |
| mandel | 138844 | 157732 | 105540 | 93380 |
| gui_lvgl | 435160 | 562496 | 337684 | 306588 |
| doom | 458872 | 551088 | 325828 | 292416 |
| ssh_shell | 854292 | 1180836 (+38%) | 580840 (-32%) | 511040 (-40%) |

-Oz with LTO on the board, against the -O2 baseline: mandel -2.4%, doom
draw fps 18 (was 20), LVGL frames 5-58% slower, irq_soak +2.9%.

**wikisort at -Os** (the anomaly noted below) is not a KlaussCPU cost:
at -O2 IPSCCP's function specialiser clones `WikiSort` for the constant
`compare = TestCompare`, and the clone, internal with one call site, is
inlined into `benchmark_body` and folded (10 KB of generic code becomes
1 KB).  The specialiser skips `optsize` functions
(`FunctionSpecializer::isCandidateFunction`), and the -Os inline threshold
(50) is far below `WikiSort`'s cost (1210), so -Os keeps the generic
out-of-line copy.  x86-64 behaves the same.  With LTO, `WikiSort` is
internal and inlined at -Os too (-24% cycles, `.text` 10008 -> 2708).

## Latest: toolchain `65f26a14dd03` (2026-10-28)

The same board, flags and command (`run-benchmarks.sh -O2 -Os`), after the
second round of backend work: interlock-aware scheduling, jump tables,
1-word short calls, GlobalMerge, absolute MMIO loads/stores, inline small
memcpy/memset, C ABI version 2 and frameless leaf functions. Runtime at
`722003c` with picolibc and compiler-rt rebuilt by that toolchain. Only the
summary is repeated here; the sections after this one are the 2026-10-27
baseline.

| OPT | CoreMark | CoreMark/MHz | CoreMark CPI | Embench speed | Embench size | Verification |
|---|---:|---:|---:|---:|---:|---|
| -O2 | 101.11 (was 98.33, +2.8%) | 1.011 | 2.95 | 0.555 (was 0.509, +9.0%) | 2.269 (was 2.563, -11.5%) | CoreMark perf + validation CRCs OK; Embench 19/19 PASS |
| -Os | 88.14 (was 84.89, +3.8%) | 0.881 | 3.05 | 0.501 (was 0.471, +6.4%) | 1.603 (was 1.826, -12.2%) | CoreMark perf + validation CRCs OK; Embench 19/19 PASS |

CoreMark -O2: 1977567715 cycles, 670639103 instructions for 2000 iterations;
-Os: 2269098380 cycles, 743574155 instructions. Against the committed
`ca184a1c92dc` tree (97.29 at -O2, see the notes) the -O2 gain is +3.9%.

**Re-measured 2026-10-28** with `tools/validate/bench.sh` (part of `validate.sh board`; it runs
`run-benchmarks.sh -O2 -Os` in a staged copy of `baremetal/`): the same
toolchain build, `65f26a14dd03` (klausscpu-llvm `e436cb084003`; the commits
after `65f26a14dd03` change only `CLAUDE.md`), runtime `98ae6f9`.  CoreMark
-O2 101.13 (1977567966 cycles, 251 more than above), -Os 88.14 (2269098380
cycles, identical).  Every Embench instruction count and `.text` size is
identical, and so is every cycle count except edn (+42 cycles at both levels)
and huffbench (+13 at -O2, +15 at -Os): the few cycles of DDR-refresh jitter
that memory-bound code shows from run to run.  The speed and size geomeans
are unchanged (-O2 0.555 / 2.269, -Os 0.501 / 1.603).
CoreMark performance and validation CRCs OK, Embench 19/19 PASS at both
levels.  These figures are the `bench.*` entries of
`tools/validate/baselines/perf.tsv`.

## Setup

| | |
|---|---|
| Board | Nexys A7-100T (Artix-7), KlaussCPU core 1, **100 MHz** (MIG `ui_clk`) |
| Toolchain | klausscpu-llvm **`ca184a1c92dc`**, *plus* the uncommitted work-in-progress pre-RA machine scheduler (`KlaussCPUMachineScheduler.{cpp,h}`, wired into `KlaussCPUTargetMachine::createMachineScheduler`). `build/bin/clang-23` was built 2026-10-27 15:58:26 and frozen for the whole run (clang-23 sha256 `4dec783eb2225346…`). See the notes for the committed-tree figure. |
| Runtime | klausscpu-runtime `8d594c9` (branch `benchmarks`), picolibc-install built 2026-10-16 |
| Benchmarks | CoreMark v1.01 `cfa9ab3`; Embench-IoT 1.0 `0466a18` |
| Flags | `-target klausscpu-unknown-elf $(OPT) -ffunction-sections -fdata-sections -fPIC`, `ld.lld --gc-sections`; soft-float via compiler-rt (built at -O1) |
| Date | 2026-10-27 |
| Command | `benchmarks/run-benchmarks.sh -O2 -Os -O1` |

The board's cycle counts are deterministic. On the same binary, a repeat run
gives the same Embench cycle counts to the cycle, and the same CoreMark count
(-Os 2355954726 cycles in both runs).

## Summary

| OPT | CoreMark | CoreMark/MHz | CoreMark CPI | Embench speed (geomean, per MHz, vs Cortex-M4) | Embench size (geomean, vs Cortex-M4 -Os) | Verification |
|---|---:|---:|---:|---:|---:|---|
| -O2 | 98.33 | 0.983 | 2.96 | 0.509 | 2.563 | CoreMark perf + validation CRCs OK; Embench 19/19 PASS |
| -Os | 84.89 | 0.849 | 3.12 | 0.471 | 1.826 | CoreMark perf + validation CRCs OK; Embench 19/19 PASS |
| -O1 | 95.24 | 0.952 | 3.08 | 0.496 | 1.829 | CoreMark perf + validation CRCs OK; Embench 19/19 PASS |

## CoreMark

2K data set (`TOTAL_DATA_SIZE=2000`), 2000 iterations, `MEM_STACK`, one
context. Every run printed "Correct operation validated".

| OPT | run | seeds | time (s) | cycles | instructions | CoreMark | crclist | crcmatrix | crcstate | crcfinal |
|---|---|---|---:|---:|---:|---:|---|---|---|---|
| -O2 | performance | 0,0,0x66 | 20.34 | 2033947061 | 687649570 | 98.33 | 0xe714 | 0x1fd7 | 0x8e3a | 0x4983 |
| -O2 | validation | 0x3415,0x3415,0x66 | 20.39 | 2038517003 | 686508171 | 98.11 | 0xe3c1 | 0x0747 | 0x8d84 | 0x0cac |
| -Os | performance | 0,0,0x66 | 23.56 | 2355954726 | 756156479 | 84.89 | 0xe714 | 0x1fd7 | 0x8e3a | 0x4983 |
| -Os | validation | 0x3415,0x3415,0x66 | 23.62 | 2362485031 | 758011083 | 84.66 | 0xe3c1 | 0x0747 | 0x8d84 | 0x0cac |
| -O1 | performance | 0,0,0x66 | 21.00 | 2099985418 | 682062314 | 95.24 | 0xe714 | 0x1fd7 | 0x8e3a | 0x4983 |
| -O1 | validation | 0x3415,0x3415,0x66 | 21.06 | 2106327087 | 683577121 | 94.95 | 0xe3c1 | 0x0747 | 0x8d84 | 0x0cac |

These match CoreMark's reference CRCs: 2K performance list/matrix/state
0xe714/0x1fd7/0x8e3a, and 2K validation 0xe3c1/0x0747/0x8d84. They also match a
host (x86-64 gcc -O2) build of the same port. The host build gives crcfinal
0x4983 and 0x0cac at 2000 iterations, the same as the board.

The run-rules report line, -O2:
`CoreMark 1.0 : 98.330976 / Clang 23.0.0git (… ca184a1c92dc…) -O2 / STACK`

## Embench-IoT 1.0

`CPU_MHZ=1` and `WARMUP_HEAT=1`. Cycles and instructions span
`start_trigger()` to `stop_trigger()`. In these tables:

- **verify** is the result of `verify_benchmark()`.
- **.text** is the `.text` of the dummy-library size build (Embench's size
  method).
- **rel. speed** is reference ms × 1000 / cycles: the speed per MHz relative to
  the reference Cortex-M4 at -O2. Higher is better.
- **rel. size** is `.text` / reference `.text`, where the reference is the
  Cortex-M4 at -Os. Lower is better.

The reference is an Arm Cortex-M4 compiled with GCC 9.2. The speed reference
was built at -O2 and the size reference at -Os, so -O2 speed and -Os size are
the like-for-like comparisons.

### -O2

| benchmark | cycles | instr | CPI | verify | .text (B) | rel. speed | rel. size |
|---|---:|---:|---:|---|---:|---:|---:|
| aha-mont64 | 3531452 | 2322333 | 1.52 | PASS | 1696 | 1.134 | 1.582 |
| crc32 | 10670727 | 2918259 | 3.66 | PASS | 560 | 0.376 | 1.972 |
| cubic | 7576661 | 3244366 | 2.34 | PASS | 3424 | 0.519 | 2.162 |
| edn | 9816046 | 3011469 | 3.26 | PASS | 2616 | 0.409 | 1.976 |
| huffbench | 7806787 | 2649191 | 2.95 | PASS | 3420 | 0.528 | 2.754 |
| matmult-int | 11997291 | 2808799 | 4.27 | PASS | 1932 | 0.332 | 3.927 |
| minver | 10309894 | 3639723 | 2.83 | PASS | 5716 | 0.388 | 4.894 |
| nbody | 4401436 | 2193099 | 2.01 | PASS | 6048 | 0.638 | 6.366 |
| nettle-aes | 9356184 | 3740033 | 2.50 | PASS | 5492 | 0.430 | 2.557 |
| nettle-sha256 | 9974860 | 4338584 | 2.30 | PASS | 8564 | 0.401 | 2.522 |
| nsichneu | 11140782 | 2007800 | 5.55 | PASS | 17744 | 0.359 | 1.483 |
| picojpeg | 9611374 | 3179755 | 3.02 | PASS | 30036 | 0.419 | 4.313 |
| qrduino | 7547884 | 2872452 | 2.63 | PASS | 17340 | 0.563 | 2.982 |
| sglib-combined | 8715988 | 2479018 | 3.52 | PASS | 4880 | 0.457 | 2.148 |
| slre | 9700388 | 2944580 | 3.29 | PASS | 6452 | 0.413 | 2.933 |
| st | 4924631 | 2066055 | 2.38 | PASS | 3476 | 0.828 | 3.476 |
| statemate | 6436467 | 1835409 | 3.51 | PASS | 8464 | 0.622 | 1.888 |
| ud | 8767925 | 2920576 | 3.00 | PASS | 2972 | 0.456 | 4.128 |
| wikisort | 2677124 | 898096 | 2.98 | PASS | 2672 | 1.038 | 0.622 |
| **geomean** | | | | | | **0.509** | **2.563** |
| geo. SD | | | | | | 1.405 | 1.661 |

### -Os

| benchmark | cycles | instr | CPI | verify | .text (B) | rel. speed | rel. size |
|---|---:|---:|---:|---|---:|---:|---:|
| aha-mont64 | 4445537 | 2464882 | 1.80 | PASS | 1252 | 0.901 | 1.168 |
| crc32 | 11853201 | 3311460 | 3.58 | PASS | 464 | 0.338 | 1.634 |
| cubic | 7432780 | 3251749 | 2.29 | PASS | 2768 | 0.529 | 1.747 |
| edn | 12126141 | 3457333 | 3.51 | PASS | 1992 | 0.331 | 1.505 |
| huffbench | 8103639 | 2617652 | 3.10 | PASS | 2560 | 0.508 | 2.061 |
| matmult-int | 14608601 | 4388437 | 3.33 | PASS | 808 | 0.273 | 1.642 |
| minver | 9905240 | 3583670 | 2.76 | PASS | 2536 | 0.404 | 2.171 |
| nbody | 4430682 | 2201310 | 2.01 | PASS | 1692 | 0.634 | 1.781 |
| nettle-aes | 9386609 | 3747160 | 2.50 | PASS | 4108 | 0.429 | 1.912 |
| nettle-sha256 | 10196035 | 4406137 | 2.31 | PASS | 7408 | 0.392 | 2.181 |
| nsichneu | 11388240 | 2001644 | 5.69 | PASS | 16776 | 0.351 | 1.402 |
| picojpeg | 11112297 | 3463693 | 3.21 | PASS | 13176 | 0.363 | 1.892 |
| qrduino | 8121715 | 2847137 | 2.85 | PASS | 11920 | 0.524 | 2.050 |
| sglib-combined | 9292188 | 2572920 | 3.61 | PASS | 4076 | 0.428 | 1.794 |
| slre | 10268654 | 3059358 | 3.36 | PASS | 5080 | 0.391 | 2.309 |
| st | 5046230 | 2102211 | 2.40 | PASS | 1712 | 0.809 | 1.712 |
| statemate | 6968441 | 2007245 | 3.47 | PASS | 8028 | 0.574 | 1.790 |
| ud | 6918670 | 2487512 | 2.78 | PASS | 1260 | 0.578 | 1.750 |
| wikisort | 4324365 | 1227013 | 3.52 | PASS | 12056 | 0.643 | 2.806 |
| **geomean** | | | | | | **0.471** | **1.826** |
| geo. SD | | | | | | 1.361 | 1.207 |

### -O1

| benchmark | cycles | instr | CPI | verify | .text (B) | rel. speed | rel. size |
|---|---:|---:|---:|---|---:|---:|---:|
| aha-mont64 | 3901997 | 2557943 | 1.53 | PASS | 1360 | 1.026 | 1.269 |
| crc32 | 11845767 | 3309939 | 3.58 | PASS | 464 | 0.339 | 1.634 |
| cubic | 7431738 | 3256280 | 2.28 | PASS | 2816 | 0.529 | 1.778 |
| edn | 11752659 | 3545121 | 3.32 | PASS | 2060 | 0.341 | 1.556 |
| huffbench | 8027961 | 2668363 | 3.01 | PASS | 2796 | 0.513 | 2.251 |
| matmult-int | 12899439 | 4078169 | 3.16 | PASS | 864 | 0.309 | 1.756 |
| minver | 9998483 | 3640837 | 2.75 | PASS | 2788 | 0.400 | 2.387 |
| nbody | 4356392 | 2203614 | 1.98 | PASS | 1688 | 0.645 | 1.777 |
| nettle-aes | 9397453 | 3753090 | 2.50 | PASS | 4260 | 0.428 | 1.983 |
| nettle-sha256 | 10247333 | 4419440 | 2.32 | PASS | 7572 | 0.390 | 2.230 |
| nsichneu | 11140791 | 2007800 | 5.55 | PASS | 17884 | 0.359 | 1.494 |
| picojpeg | 10447032 | 3548193 | 2.94 | PASS | 25824 | 0.386 | 3.708 |
| qrduino | 7698866 | 2893198 | 2.66 | PASS | 12368 | 0.552 | 2.127 |
| sglib-combined | 9121000 | 2583449 | 3.53 | PASS | 4320 | 0.436 | 1.901 |
| slre | 9747377 | 3036370 | 3.21 | PASS | 5648 | 0.411 | 2.567 |
| st | 5005914 | 2085523 | 2.40 | PASS | 1856 | 0.815 | 1.856 |
| statemate | 6946848 | 2011175 | 3.45 | PASS | 8064 | 0.576 | 1.798 |
| ud | 7101956 | 2682610 | 2.65 | PASS | 1420 | 0.563 | 1.972 |
| wikisort | 2686170 | 907966 | 2.96 | PASS | 2572 | 1.035 | 0.599 |
| **geomean** | | | | | | **0.496** | **1.829** |
| geo. SD | | | | | | 1.415 | 1.413 |

## Notes

- **Committed toolchain.** Earlier the same day, the `clang-23` built at 15:27
  was used. It predates the WIP scheduler sources and is most likely the
  committed `ca184a1c92dc`. With it, CoreMark -O2 was 97.29 (2055740724 cycles,
  692603570 instructions), and Embench -O2 had the same speed geomean of 0.509.
  The WIP scheduler build is about 1% faster on CoreMark -O2.
- **wikisort.** Its -Os `.text` is 4.5x the -O2 size, and it takes 1.6x the
  cycles. At -O1/-O2, clang specialises `WikiSort()` for its constant
  `compare` and inlines the clone into `benchmark_body()`, so the 12 KB
  out-of-line copy is garbage-collected. At -Os it stays out of line, and
  generic: the function specialiser skips optsize code (target-independent;
  see "Code size" above).
- **Code size.** -O2 code is about 2.6x the reference Cortex-M4 (-Os) on the
  geomean, and -Os is about 1.8x. The largest ratios are nbody (6.4x at -O2,
  1.8x at -Os) and minver: soft-float and FP-heavy code with inlining.
- **fabs/fabsf.** With the toolchain of the first runs (`ca184a1c92dc`, whose
  driver still added `-ffreestanding`), clang emitted these as libm calls in
  cubic, minver, nbody and st, where Arm GCC clears the sign bit inline.
  Since klausscpu-llvm `1ebec965e063` programs are compiled hosted, and
  `fabs`/`fabsf`/`copysign`/`-x` are bit operations (`BCLR`, `ANDV`, `BTGL`,
  `XORI`) at every level; no Embench benchmark calls them any more, so the
  phase-4 results above already include that.
- **CPI.** Embench CPI ranges from 1.5 (aha-mont64, native 64-bit) to 5.5
  (nsichneu, a large, branchy state machine). CoreMark's CPI is about 3.0.
- **No verification failures.** Every CoreMark run validated, and every
  Embench benchmark returned `verify_benchmark() == 1`, at every level.
