# rustc on the board: plan (2026-11-18)

**Goal:** run the Rust compiler natively on the board's Linux. A user logs in
and runs `rustc hello.rs && ./hello`. The program is compiled on the board,
linked by the board's own clang and lld (`/opt/llvm`), and runs. Later, if it
is usable: proc macros, and cargo for offline projects.

**Why:** for fun (Graham, 2026-11-18). Like clang on the board
(`CLANG_ON_BOARD_PLAN.md`), it is a self-hosting milestone and a stress test
of the compiler, the C and C++ runtimes, the kernel and the hardware. rustc
is a larger and stranger program than clang: about 600 000 lines of Rust on
top of LLVM, heavily threaded, thread-local-heavy, loaded as a shared library.

Read `LINUX_STATUS.md` (board access and protocol, images, installs) and
`CLANG_ON_BOARD_PLAN.md` (how LLVM was cross-built and shipped) first.

## Where we start

- **Rust with std on the board's Linux works** (klausscpu-llvm `rust/README.md`
  part 7, commit 81cdadb): the JSON target `klausscpu-unknown-linux-musl.json`,
  patched rustc 1.98.0-dev (rust 09a37136 + `rustc-patches/0001`–`0004`, the
  backtrace patch, the `libc` fork), `klauss-linux-cargo`. Rust's own core,
  alloc and std test suites pass on the board (30/30 binaries), and the `libc`
  fork matches musl's headers (9944 ctest checks).
- **The stage1 rustc** is at
  `~/Documents/src/klausscpu-p5-rust-build/klausscpu-rust/build/x86_64-unknown-linux-gnu/stage1/bin/rustc`,
  linked against the fork's `build/` LLVM.
- **LLVM for the board:** `linux/toolchain/build-native-llvm.sh` cross-builds
  clang and lld for `klausscpu-unknown-linux-musl` against the board's libc++
  (CLANG_ON_BOARD M2/M3). The installed clang is a PGO + ThinLTO multicall
  binary in `/linux/toolchain.img`, mounted at `/opt/llvm`.
- **The board:** 121 MB of RAM, 256 MB of swap on the SD card, 20–30 million
  instructions per second, 2 GiB of address space per process. clang's C++
  hello takes 78 s at -O2 with a 77 MB peak RSS.
- **Size on the host:** the stage1 `librustc_driver.so` is 571 MB (x86,
  unoptimised parts, debug info), plus 150 MB of host std rlibs.

## Milestones

Each milestone ends with tests that pass, recorded under "Results". Commit as
you go: rustc patches are exported to klausscpu-llvm `rust/rustc-patches/`.
Backend fixes follow the usual path (reduce, fix, lit, Opus review, validators).

### R1. A built-in target `klausscpu-unknown-linux-musl` in rustc

rustc's build system builds a compiler only for a built-in target (a host
needs a std built by the bootstrap, and JSON targets cannot be hosts).
Built-in targets may not use `Arch::Other`, so:
- **`Arch::KlaussCpu`** in `rustc_target::spec` and the matches over `Arch`
  (callconv dispatch, target features, inline asm "unsupported", va_arg,
  the feature gate for c_variadic, raw-dylib, rustc_codegen_gcc's list).
  Patch 0001's `Arch::Other("klausscpu")` guard becomes the variant; the
  bare-metal JSON target keeps working because the parser maps the
  `"klausscpu"` string to the variant.
- **`targets/klausscpu_unknown_linux_musl.rs`**, the JSON spec's contents in
  Rust: the data layout, `max_atomic_width` 64, PIE executables, static CRT
  by default, `-lunwind` (std's unwind already does that), the clang linker
  flavour. Tier 3, `host_tools: Some(true)`.
- **Thread-locals:** the JSON spec has `has-thread-local: false`, so std uses
  pthread keys. Linux TLS works for C (CLANG_ON_BOARD "threads and TLS").
  Turn `has_thread_local` on if rustc's `#[thread_local]` codegen works with
  the backend's TLS model (test with std's `thread_local` tests); rustc
  itself uses thread-locals everywhere, so this matters for speed.
- **Check:** the host scripts take the built-in triple (no `-Zunstable-options`
  or JSON path), and `stdcheck` and `library-tests.sh` still pass. The JSON
  file stays for older rustcs and as the lit test's reference.
- **Output:** `rustc-patches/0005`.

### R2. LLVM libraries for rustc, built for the board

rustc links LLVM statically into `librustc_driver.so`.
- A separate cross build of the LLVM libraries (no clang), Release or
  MinSizeRel, KlaussCPU target only, **no ThinLTO** (the toolchain image's
  libraries are LTO bitcode), PIC (they go into a shared library), against
  the board's libc++. Output in its own build directory.
- An `llvm-config` for it. rustc's `rustc_llvm/build.rs` runs `llvm-config`
  for the target; the host's `llvm-config` with the target build's library
  directory, or a small wrapper script.

### R3. rustc cross-built for the board

`x.py build --host klausscpu-unknown-linux-musl --target klausscpu-unknown-linux-musl`
(the stage that gives a compiler for that host), with `llvm.use-libcxx` and
the board's clang++ for `rustc_llvm`'s C++ wrapper.
- **Crates with per-architecture code:** expect a few. Known so far: `psm`
  (stack switching for `stacker`) has no KlaussCPU assembly and builds
  without it, so rustc cannot grow its stack on deep recursion; a port is
  about 30 lines of assembly (later). jemalloc is off (musl's malloc).
- **Expect backend bugs:** rustc is the largest Rust program ever compiled for
  KlaussCPU. A crash or miscompile is reduced and fixed in the backend, as in
  CLANG_ON_BOARD M3.
- **First signs of life:** `rustc -vV` and `rustc --print sysroot` on the
  board, from /data.

### R4. Hello world on the board

- A sysroot on the board: `bin/rustc`, `lib/librustc_driver-*.so`, and
  `lib/rustlib/klausscpu-unknown-linux-musl/lib` (std rlibs, built on the
  host). The linker is `/opt/llvm/bin/clang`.
- `rustc hello.rs && ./hello`, static and `-C target-feature=-crt-static`.
- **Measure:** wall time, user+sys, instructions, peak RSS, major faults,
  and how much is start-up (relocating `librustc_driver.so`; libcrypto's 25 000
  relocations cost 0.2 s per process) against compiling.

### R5. An image for it

`/linux/rust.img` at `/opt/rust`, like `toolchain.img` (`install-toolchain`,
`.next` staging). Needs an rcS change and a system image rebuild (Graham
decides, below). Until then, R3/R4 run from `/data`.

### R6. Correctness: the board's rustc matches the Beelink's

As CLANG_ON_BOARD M5: the same sources and flags give byte-identical objects
(`--emit=obj`) from the board's rustc and the cross rustc. Sources: stdcheck,
the hello examples, a few crates' files. Then stdcheck compiled on the board
passes on the board.

### R7. Proc macros and cargo (optional)

- **Proc macros:** rustc `dlopen`s a proc-macro crate built as a dylib for
  the host. A `#[derive]` test crate, compiled and used on the board.
- **cargo:** cargo depends on curl, OpenSSL and libgit2. An offline cargo
  (vendored or path dependencies only) is the target, if R4 shows that
  compiling on the board is bearable.

### R8. Measure and tune

Size (opt-level, strip, `share-generics`), start-up relocations, the
`psm` port, TLS. Decide how far to go from R4's numbers.

## Decisions for Graham

- **R5:** a separate `rust.img` at `/opt/rust` (recommended: like
  toolchain.img, installs alone), or rustc inside `toolchain.img`.
- **R7:** whether cargo is worth porting after R4's numbers.
- **R8:** how much speed work, after R4.

## Risks

- **Memory:** rustc's code alone is likely larger than the board's RAM
  (estimate 100–150 MB stripped, against clang's 94 MB), and it needs tens
  of MB of heap to load std's metadata, even for hello world. Expect heavy
  paging to SD; hello world may take minutes.
- **Start-up:** `librustc_driver.so` may have hundreds of thousands of
  relocations.
- **Backend:** miscompiles in a program this size.
- **Disk on the Beelink:** about 39 GB free; a cross rustc build adds an
  estimated 10–20 GB. Clean up host-only build products if needed.
- **Address space:** 2 GiB per process should be enough; check `rustc`'s
  mappings in R4.

## Results

(none yet)
