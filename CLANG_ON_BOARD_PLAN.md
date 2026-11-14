# clang on the board: plan (2026-11-13)

**Goal:** run our own toolchain, clang and lld, natively on the board's Linux.
A user logs in and runs `clang hello.c -o hello && ./hello`. The program is
built on the board, links against the board's own musl `libc.so`, and runs.
The board's compiler must produce exactly what the cross compiler on the
Beelink produces from the same input and flags.

**Why it's hard:**
- LLVM is about 2 million lines of C++, and KlaussCPU Linux has no C++
  runtime yet.
- The board has 121 MB of RAM and runs about 20–30 million instructions per
  second.
- The work is a stress test of the compiler, the C library, the kernel and the
  hardware at once, which is part of the interest.

Read `LINUX_STATUS.md` first: board access, the board protocol, and the build
and install procedures.

## Where we start

- **The cross toolchain works:** `klausscpu-llvm/build/bin` (host x86-64) has
  clang, lld and compiler-rt builtins for `klausscpu-unknown-linux-musl`, both
  static and shared. Dynamic linking works (DYNLINK_PLAN), and so do threads
  and TLS.
- **musl:** two sysroots.
  - `linux-ws/sysroot`: the static `libc.a`.
  - `linux-ws/dyn/sysroot`: `libc.so` plus the loader, built with
    `MUSL_SHARED=1 LINUX_WS=linux-ws/dyn linux/musl/build.sh`.
- **No C++ runtime for the Linux triple:**
  - no libc++, libc++abi or libunwind;
  - C++ exceptions abort the backend: llvm `OPEN_ITEMS.md` #26, "a landing pad
    that reads the exception pointer or selector aborts llc". The backend
    names no exception-pointer or exception-selector registers.
  - `libffi`'s two C++ tests are "unsupported" for this reason, and Buildroot
    has C++ turned off.
- **Atomics** are `__sync_*` calls (lock-free, LR/SC in musl). C++
  `std::atomic` lowers to the same.
- **Memory:** 121 MB of RAM, 256 MB of swap on the SD card (slow). The address
  space is 2 GiB per process (`TASK_SIZE`).
- **Disk:** the system image is 256 MiB with about 110 MiB free. The SD card's
  FAT partition has tens of GB free, but writes go only inside `/linux/`.
- **The host clang** (`build/bin/clang-23`) is 189 MB. Built for KlaussCPU with
  only the KlaussCPU back end, `MinSizeRel` and stripped, it should be much
  smaller (estimate 40–80 MB; M3 measures).

## Milestones

Each milestone ends with tests that pass on the emulator. From M2 on they must
also pass on the board, and results are recorded in this file under "Results".
Commit as you go.

### M1. C++ exceptions in the backend (llvm; correctness-critical)

- **Registers:** decide which registers carry the exception pointer and the
  selector into a landing pad (R0/R1, as RISC-V uses a0/a1, is the natural
  choice). Implement `getExceptionPointerRegister` and
  `getExceptionSelectorRegister`.
- **Write it down:** record the decision in `ABI.md`. libunwind's port in M2
  must match it.
- **CFI:** check the DWARF CFI the backend emits: register numbers, CFA rules,
  frames with and without a frame pointer, and the `leaveret`/`push`/`pop`
  sequences. Exception tables are already done (ABI.md "Dynamic linking").
- **Tests:**
  - llc lit tests for landing pads, invoke/resume and the cleanup paths;
  - a `-S` comparison of small C++ programs with `throw`/`catch`.
- **Owner:** the LLVM session's area. Coordinate with it (`ListAgents`); it
  may do M1 itself. Fixes may be pushed to llvm main (Graham's standing
  permission). Get an Opus review, as for every correctness-critical change.

### M2. The C++ runtime for `klausscpu-unknown-linux-musl`

**libunwind KlaussCPU port:**
- `Registers_klausscpu` in `libunwind/src/Registers.hpp`, with the DWARF
  register map from the backend;
- `UnwindRegistersSave.S` and `UnwindRegistersRestore.S`: save and restore
  R0–R15, SP and PC. Mind the SP, which here is not a GPR (`getsp`/`setsp`);
- the DWARF and EHABI choices: DWARF `.eh_frame`, as the backend emits.

**libc++abi and libc++:**
- built with the cross clang against `linux-ws/dyn/sysroot`, both static and
  shared;
- CMake runtimes build (`llvm/runtimes`) with a toolchain file;
- threads via musl pthreads;
- no locale data beyond C/POSIX at first.

**Install** into the sysroot used for Linux programs, so the driver finds them
(`-stdlib=libc++` the default for the Linux triple, and `-unwindlib`). The
cross clang must link C++ programs with nothing but `clang++ x.cpp`.

**Tests, on the emulator, then the board:**
- **Exceptions:**
  - throw/catch inside a function, across frames, and across a shared
    library;
  - rethrow, `catch (...)`, and destructors during unwinding;
  - `std::exception_ptr`;
  - exceptions in threads;
  - `noexcept` termination.
- **The library:** `std::string`, `std::vector`, `std::map`, `std::thread`,
  `std::mutex`, iostreams and `std::filesystem` on simple paths.
- **Upstream suites:** a subset of libc++abi's and libunwind's own test suites
  (they're lit-based; run the programs under the emulator the way
  `linux/userland/tests/libffi/run.sh` runs libffi's suite).

**By-products:**
- Buildroot's external toolchain can switch C++ on (`BR2_TOOLCHAIN_EXTERNAL_CXX`
  in `toolchain.sh`), which opens the C++ packages;
- libffi's two C++ tests can run.

### M3. clang and lld built for KlaussCPU (cross build)

**The cross build:**
- a CMake toolchain file for host `klausscpu-unknown-linux-musl`, using
  `build/bin/clang` and the M2 libc++;
- `LLVM_TABLEGEN` and `CLANG_TABLEGEN` taken from the host build (`build/bin`);
- the settings:
  - `LLVM_TARGETS_TO_BUILD=KlaussCPU` and `LLVM_ENABLE_PROJECTS="clang;lld"`;
  - `CMAKE_BUILD_TYPE=MinSizeRel`;
  - zlib, zstd, libxml2, terminfo and `LLVM_ENABLE_THREADS` off at first;
  - assertions off;
- tools: clang, lld (as `ld.lld`), `llvm-ar`, `llvm-nm`, `llvm-objdump` and
  `llvm-readelf`;
- output in its own build directory (`klausscpu-llvm/build-native-klausscpu`),
  not `build/`.

**Static linking:** link the tools statically (libc++ and musl static). A
shared `libLLVM.so` would cost every process start hundreds of thousands of
relocations. The measurement behind that: `libcrypto.so`'s 25 000 relocations
cost OpenSSH about 0.2 s per process. Measure both if in doubt.

**A large stress test:** this is by far the largest program ever compiled for
KlaussCPU, so expect backend bugs and miscompiles. When clang crashes or
misbehaves on the target, reduce the failing source file (`creduce`,
`llvm-reduce` on the IR) and fix the backend, going through the LLVM session
and an Opus review. `-O1` for a suspect file is a temporary workaround, not a
fix.

**First signs of life (emulator):** `clang --version`,
`clang -cc1 -triple klausscpu-unknown-linux-musl -fsyntax-only hello.c`, and
`ld.lld --version`.

### M4. A toolchain image and the sysroot on the board

**A separate `/linux/toolchain.img`:** a read-only ext4 image on the card,
loop-mounted by rcS at `/opt/llvm` when present.
- **Why separate:** the toolchain (tens of MB) changes rarely, unlike the
  system image. A separate image keeps `install-rootfs` small and lets each be
  updated alone.
- **Install:** an `install-toolchain` script, like `install-rootfs`, including
  the `.next` staging when it's in use.
- **Alternative:** grow the system image. Graham decides (see "Decisions").

**Contents:**
- `bin/clang`, `clang++`, `ld.lld`, `llvm-ar` and the other tools;
- clang's resource directory (`lib/clang/23/include`, and the Linux-triple
  compiler-rt builtins);
- a sysroot: musl headers, `crt1.o`/`Scrt1.o`/`crti.o`/`crtn.o`, `libc.so`
  plus `libc.a`, kernel UAPI headers, libc++ headers and libraries.

The system image today leaves out headers and static libraries (build-rootfs
filters them), so the toolchain image carries its own.

**Configuration** (no wrapper scripts): `/opt/llvm/bin/clang.cfg` with
`--sysroot=/opt/llvm/sysroot`, the default triple, `-fuse-ld=lld`, and the
dynamic linker `/lib/ld-musl-klausscpu.so.1`.
- Programs built on the board link against `/lib/libc.so`, the same
  `libc.so` as in the sysroot. Check they match: the sysroot copy must come
  from the same musl build as the system image's.

**Build:** a script, `linux/toolchain/build-toolchain-image.sh`, from the M3
output plus the sysroot. Test it on the emulator first, with the image on an
emulated SD card (`klausscc --sd-image`, as `linux.sh sd-spi` does).

### M5. Correctness: the board's compiler matches the Beelink's

**The acceptance test:** for a corpus of C (and C++) files, the objects
`clang -c` produces on the board (or the emulator) must be byte-identical to
the cross clang's, with the same version, flags and `-ffile-prefix-map` for
paths. Any difference is a miscompile of clang itself. The corpus:
- the bare-metal test programs (`baremetal/programs`, with the bare-metal
  triple);
- `linux/userland` sources;
- the dynlink and libffi test programs;
- a few large sources: `sqlite3.c` (if memory allows, else a slice), lua and
  busybox applets.

**Then link and run on the board:**
- the programs pass their checks;
- `ld.lld` output also matches the cross `ld.lld` byte for byte.

**Automation:** a script, `linux/userland/tests/native-clang/board.sh`:
compile on the board, fetch the objects, compare, run. Add it to
`board-check.sh` as an optional section.

### M6. Measure, tune, document

**Measure** on the board, with perfstat and wall time:
- `clang hello.c` at `-O0` and `-O2`;
- a 1000-line file;
- a C++ file using the standard library;
- lua built from source;
- peak memory (`/proc/<pid>/status` VmHWM) and swap use.

Profile with `KLAUSSCC_PROFILE` on the emulator: TLB, I-cache, where the
start-up goes. clang's code is huge, so expect its I-cache and TLB behaviour
to look like Python's or worse (`LINUX_PLAN.md`).

**Cheap wins to try:**
- `-O1` or `-Os` as the board's default;
- precompiled headers for big headers;
- `-fno-integrated-cc1`, if it saves anything, to avoid the exec;
- `-ffunction-sections` effects on lld time.

**Write it up:** what to expect (seconds per file) in `LINUX_STATUS.md`, plus
a short user guide: how to compile on the board, where the headers are, and
how to add libraries.

## Decisions for Graham (ask before M4)

**Answered 2026-11-13:** a separate `toolchain.img`; static tools; clang,
lld and the binutils (`llvm-ar`, `llvm-nm`, `llvm-objdump`, `llvm-readelf`);
Buildroot's C++ on (`BR2_TOOLCHAIN_EXTERNAL_CXX`, the shared C++ libraries
in the system image).

- **Where the toolchain lives:** a separate `toolchain.img` (recommended) or a
  bigger system image.
- **Static or shared tools:** static (recommended for start-up time) or
  shared `libLLVM.so` (smaller in total if many tools are installed).
- **Which tools:** clang and lld only, or also `llvm-objdump`, `llvm-nm`,
  `llvm-ar`, `clang-format`… Each static tool adds several MB.
- **Whether C++ goes into Buildroot** once M2 works, which opens many more
  packages.

## Risks

- **Backend bugs on a large C++ code base** are the main risk. The M5
  identity test and emulator-first work catch them; reduce each one to a small
  case.
- **Memory:** clang at `-O2` on a big file can exceed 121 MB. Swap works but
  is slow, so set expectations (small files) and measure in M6.
- **Speed:** a compile may take tens of seconds to minutes (estimates only).
  M6 measures; `-O1` by default may be the practical choice.
- **The libunwind port's details:** the SP isn't a GPR, and frames are set up
  with `enter`/`leave`/`leaveret`. CFI and unwinding must agree exactly.
  Write tests that unwind through every frame shape the backend makes.
- **`ELF32` with LP64:** some LLVM code may assume ELF64 for 64-bit hosts.
  Watch for it in M3; the host-side tools already handle the target format,
  so trouble is more likely in host-detection code (`Host.cpp`, memory
  mapping).

## Results

(Fill in per milestone: commits, test counts, sizes, timings.)

### M1 (2026-11-13): done, llvm 537f135c

- Landing pads get the exception pointer in **R0** and the selector in **R1**
  (`getExceptionPointerRegister` / `getExceptionSelectorRegister`; clang's
  `__builtin_eh_return_data_regno` 0 / 1). ABI.md has a new "Exceptions"
  section, which the libunwind port follows. It covers DWARF columns 0–15
  (GPRs), 16 (SP) and 17 (the return-address column), the CFI, and which
  code has unwind tables. OPEN_ITEMS #26 is closed.
- **One fix beyond the plan:** a function with dynamic allocas has no
  reserved call frame, and such a function with landing pads now gets
  `DW_CFA_GNU_args_size` before every call. Without it, a catch inside a loop
  leaked the call's stack arguments on every catch.
- **CFI checked with llvm-dwarfdump:**
  - the CIE: SP+8, RA at CFA-8;
  - ENTER frames;
  - PUSH/GETSP frames;
  - R7 realignment;
  - MachineOutliner FDEs.
- **Tests:** `landingpad.ll` (O0/O2), `klausscpu-eh-regno.c`; the KlaussCPU
  lit suites (179) pass.
- **Opus review:** no blocker. Its jumpto finding was in an earlier draft
  and was already fixed; its doc points are in.
- **Unwind tables:** the Linux triple still defaults to none for C (x86-64,
  AArch64 and RISC-V default to asynchronous tables). This doesn't matter
  for C++ code. Left as is.

### M2 (2026-11-13): runtime built; emulator and board pass

- **llvm 54b72456:**
  - the libunwind KlaussCPU port (`Registers_klausscpu`,
    `__unw_getcontext`, `jumpto`, x86-64 style: SP is set last);
  - driver changes for the Linux triple:
    - libc++ is the default C++ library;
    - `<sysroot>/include/c++/v1` is on the include path;
    - `clang++` links `-lc++ -lc++abi -lunwind`;
    - every link gets `--eh-frame-hdr`, static ones too;
  - libc++ `<bitset>` `count()` avoids `_BitInt` on KlaussCPU, because the
    target has no `_BitInt`.
- **`linux/cxx/build.sh`:** builds libunwind, libc++abi and libc++ with
  CMake (`linux/cxx/klausscpu-linux.cmake`) and installs them into
  `linux-ws/dyn/sysroot`, static (-fPIC) and shared. That needs a fresh
  configure plus about 2 min on 24 cores. Sizes:
  - `libc++.so.1` 1.26 MB;
  - `libc++abi.so.1` 357 KB;
  - `libunwind.so.1` 54 KB.

  A static hello-with-exceptions is 488 KB, a PIE 5.7 KB.
- **`linux/userland/tests/cxx/run.sh` (emulator, one boot, 23 s) and
  `board.sh` (board, 21 s):** 13 programs, all pass on both (0 failing
  checks).
  - **eh.cpp:** 23 checks, built at -O0/-O2/-Oz, both static and as a PIE
    linked to a shared library (`eh_lib.cpp`), plus two -O2 variants:
    - without ENTER/LEAVE (PUSH/GETSP frames);
    - without shrink-wrapping, with leaf frame pointers.

    It covers:
    - throw/catch locally and across frames, with destructors;
    - rethrow, `catch (...)`, a throw from inside a handler;
    - catch by base and by pointer;
    - `exception_ptr`, `current_exception`;
    - callee-saved registers restored by the unwinder;
    - `new[]` with a throwing constructor;
    - `uncaught_exceptions`;
    - GNU_args_size: 2000 catches in a VLA frame with SP unchanged;
    - a realigned frame;
    - exceptions in threads and across threads;
    - `_Unwind_Backtrace`;
    - throwing into, out of and through the library.
  - **stdlib.cpp:** 24 checks (static -O0/-O2, PIE -O2): strings,
    containers, `<format>`, `<charconv>`, `<regex>`, iostreams, threads,
    mutex, condition variables, atomics, `std::filesystem` on /tmp.
  - **terminate.cpp:** a throw out of `noexcept` reaches the terminate
    handler; an uncaught exception prints libc++abi's message and ends in
    SIGABRT.
- **Still open in M2:**
  - the upstream libc++abi/libunwind test subsets;
  - Buildroot's `BR2_TOOLCHAIN_EXTERNAL_CXX`;
  - libffi's two C++ tests;
  - installing the shared C++ libraries in the system image.

  The board run uses copies from /tmp with LD_LIBRARY_PATH.

### M2 follow-ups (2026-11-13)

- **libffi's two C++ tests** (`unwindtest`, `unwindtest_ffi_call`) now run
  in `libffi/run.sh`. Both pass at -O0 and -O2: exceptions unwind through
  the port's assembly.
- **Buildroot C++ is on** (shared build): `BR2_TOOLCHAIN_EXTERNAL_CXX`, the
  wrapper's `g++`/`c++` run clang++, and `BR2_TOOLCHAIN_EXTRA_LIBS="libc++
  libc++abi libunwind"`. That option takes bare names: Buildroot adds `.so*`
  itself, and the old name `BR2_TOOLCHAIN_EXTRA_EXTERNAL_LIBS` trips
  Buildroot's legacy check. The libraries are copied into the target's
  `/lib` and also `/usr/lib` (about 1.4 MB duplicated).

- Upstream suites (`linux/userland/tests/cxx/upstream.sh`,
  `upstream_plan.py` evaluates each test's lit lines): libunwind's and
  libc++abi's own tests, static, -O0 and -O2 on the emulator: all 70
  runnable tests pass at both levels (forceunwind, signal_frame,
  unw_getcontext/unw_resume, unwind_01..06, forced_unwind1..3, catch_*,
  dynamic_cast*, the demangler, guard_threaded_test with a 600 s timeout);
  31 unsupported (other architectures, AIX, no-exceptions builds, the
  `.sh.*` lit-shell tests).

### M3 (2026-11-13): built; runs on the board

- **`linux/toolchain/build-native-llvm.sh`** (llvm 2f0e77af makes
  `klausscpu` a CMake host architecture). The first build compiled all
  3002 steps with no compiler error and took about 10 minutes on the
  Beelink.
- **Sizes, stripped, MinSizeRel:**
  - separate tools: clang 56 MB, lld 34 MB, llvm-ar/nm/objdump/readobj
    4–6 MB each (111 MB in all);
  - the multicall build (`NATIVE_MULTICALL=1`, one static `llvm` binary
    with symlinks): 64 MB for all of them. This one goes into the image.
- **On the board:** `clang --version`, `ld.lld --version` and
  `clang -cc1 -fsyntax-only` work, 2–4 s each from /data the first time.
- **M5 early (`native-clang/board.sh`):** 38 sources at -O2 (zlib, bzip2,
  dynlink tests, linux/userland), objects byte-identical to the cross
  clang's except `.comment` (the version string, which names each build's
  commit).
- **Compile times on the board, -O2:**
  - zlib deflate.c: 105 s;
  - bzip2 compress.c: 176 s;
  - libc++ test eh.cpp: 164 s;
  - small files: 2–20 s.

  This is far slower than hoped. M6 has to find out why.

### M4 (2026-11-13): image built

- **`linux/toolchain/build-toolchain-image.sh`:** `toolchain.img`,
  127 MiB with 104 MiB used. It holds:
  - the multicall binary;
  - clang's resource headers and the Linux builtins;
  - the sysroot at `/opt/llvm/klausscpu-linux-musl`. That is the driver's
    built-in default (`<bindir>/../klausscpu-linux-musl`), so there is no
    config file and no wrapper.
- **Boot and install:** rcS mounts `/linux/toolchain.img` read-only at
  `/opt/llvm`, swapping in a staged `.next` first. `install-toolchain`
  works like `install-rootfs`, and `/opt/llvm/bin` is on PATH.
- **libc.so check:** the board's `/lib/libc.so` is the sysroot's
  `libc.so` after `llvm-strip --strip-all`, because Buildroot strips it.
  The script prints that hash.

### M4 on the board (2026-11-14)

- **Installed:** the new system image (busybox with loop mounts, the new
  rcS, `install-toolchain`, libc++ in `/lib`) and `/linux/toolchain.img`.
- **board-check after the reboot:** 16 PASS, 0 FAIL. rcS mounts
  `/opt/llvm`, and `clang` is on PATH in login shells.
- **On the board, from /opt/llvm:**
  - `clang -O2 h.c -o h && ./h` works, static and `-pie`, in 2–5 s;
  - `clang++ hello.cpp`, with exceptions, compiles, links and runs.

### M5 (2026-11-14): passes

`native-clang/board.sh`, with `NC_SKIP=cxx-stdlib` (its compile alone takes
over an hour, because the board swaps):
- **Objects:** 102 compiles, -O2 and -O0, are byte-identical to the cross
  clang's (ignoring `.comment`). The corpus: zlib including `example.c`,
  bzip2, the dynlink tests, linux/userland, six bare-metal programs (their
  own triple) and the libc++ tests `eh.cpp` and `eh_lib.cpp`.
- **Links:** 5 programs linked on the board by its lld are byte-identical
  to the cross lld's links: eh, musltest, strtest, zlib's example, bzip2.
- **Runs:** all five run correctly on the board: eh 23/23, musltest PASS,
  strtest PASS, zlib's example, and a bzip2 round trip.
- **Earlier run:** `cxx-stdlib` was also identical at -O2 and -O0, in 94/94
  objects.
- **Time:** the board took 3024 s for the 102 compiles.

### M6 so far (2026-11-14)

**Board, perfstat (`native-clang/perf.sh`; counters in
`linux-ws/native-perf/counters.txt`):**

| compile | MinSizeRel clang | Release clang | instructions | CPI | peak RSS |
|---|---|---|---|---|---|
| hello.c -O2 | 2 s | 1 s | 0.03 G | 5.3 | 46 MB |
| zlib deflate.c -O0 | 34 s | 29 s | 0.59 G | 5.7 | 50 MB |
| zlib deflate.c -O2 | 109 s | 90 s | 2.14 G | 5.1 | 55 MB |
| C++ hello (`<string>`, `<vector>`), any -O | 255 s | 213 s | 3.7 G | 6.8 | 65 MB |

- **The Release build** executes 24% fewer instructions and is about 17%
  faster, at 90 MB against 64 MB. The image now takes it.
- **C++ time is header parsing.** A precompiled header of
  `<cstdio> <stdexcept> <string> <vector>`, built once on the board
  (4.5 min, 11 MB), cuts the C++ hello from 213 s to 44 s.
- **Where the cycles go:**
  - instruction-fetch misses: ~40%;
  - D-cache stalls: 36–45%;
  - `stall_muldiv`: 6–9%. It is mostly the TLB's +1 EX cycle per load and
    store (the CPU session), not division.
- **The TLB refill handler** (32 instructions) runs 10 times per 1000
  instructions: 31–36% of all executed instructions (klausscc model).
- **The text-page spread** (klausscc profile): 90% of instructions run in
  98–199 of the ~840 64 KiB text pages, against 32 TLB entries.
  `clang-order.txt.gz` (lld `--symbol-ordering-file`, `NATIVE_ORDER=1`)
  packs the hot functions together.
- **Hardware candidates** sent to the CPU session, and from there to
  Graham: its ranking is
  1. a hardware page-table walker, or a refill assist;
  2. translation in parallel with the D-cache;
  3. a 128 KiB L2;
  4. a 32 KiB 2-way I-cache;
  5. an 8-entry return stack.

  The klausscc models behind it (L2, I-cache shapes, return stack,
  DIV/MOD, refill cost) are on klausscc master (1f99640).
- **Hot-function ordering made things worse.** `--symbol-ordering-file`,
  hottest first, from the klausscc profile of the MinSizeRel binary:

  | variant | deflate -O2 | C++ hello -O2 | refills per 1000 instructions |
  |---|---|---|---|
  | unordered Release | 91 s | 218 s | 8.5 / 10.1 |
  | every executed function ordered | 110 s | 240 s | 13.5 / 14.0 |
  | only the 300 hottest ordered | 98 s | 228 s | 9.5 / 11.7 |

  The extra instructions are the refill handler's. A pure hotness order
  mixes every phase of clang (lexer, parser, Sema, codegen) into one block,
  so each phase's code spreads over all of it. The link order, by library
  and source file, already keeps a phase's code together, and the 32-entry
  round-robin TLB rewards that. A call-graph-aware order (lld
  `--call-graph-profile-sort`) would need call edges, which the profile
  lacks. The option was removed.
- **The board's `/linux/toolchain.img` now holds the unordered Release
  build:** 156 MiB, 129 MiB used. It went on with `umount /opt/llvm` and
  `install-toolchain -z -`, 5.3 min, no reboot needed.
- **M5 again, on the installed Release clang** (2026-11-14): 102 objects
  and 5 links identical, all 5 programs run correctly. The 102 compiles
  took 2415 s, against 3024 s with the MinSizeRel build: 20% faster.
- **The hardware TLB walker** (klaussCPU MMU_PLAN §11.8, spec v1 frozen
  2026-11-14, Graham's choice of the CPU session's first item):
  - the RTL session is building it on klaussCPU branch `tlbwalk`;
  - the SR names `tlb_ptb`, `tlb_walk` and `tlb_wcnt` are in klausscpu-llvm
    bddd5e3;
  - kernel support (`mm/tlbwalk.c`, gated on CPU_FEATURES[25]) and a
    klausscc model are in progress.
