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
