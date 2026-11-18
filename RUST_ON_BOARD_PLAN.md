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

**Done first (2026-11-18): direct I/O for the image loop devices.** busybox
`mount -o loop` made `rust.img` and `toolchain.img` buffered loops, so every
block was cached twice: by ext4 above and by the FAT file below. That halves
the RAM left for rustc's pages. sdinit already used direct I/O for the root
and /data. Now rcS and install-{toolchain,rust} call `loopdio`
(linux/userland/loopdio.c, LOOP_SET_DIRECT_IO), and board-check checks it.
Cold runs (drop_caches first), loop mode given as toolchain/rust:

| Run | Buffered (0/0) | Direct I/O | Card reads |
|---|---|---|---|
| rustc hello, cold | 79.5 s | 42.7 s (live switch, 3 runs 42.5-42.8); 40.3-40.5 s after a reboot (1/1) | 549 → 278-295 MB |
| clang zlib-deflate -O2, cold | 37.7 s | 36.5 s | 38 → 35 MB |
| clang cxx-hello -O2, cold | 91.2 s | 88.4 s | 54 → 37 MB |
| perf.sh -O2 (1 warm-up), user+sys: deflate / cxx-hello | 30.5 / 79.0 s (0/1) | 30.2 / 77.8 s (1/1, after the reboot) | |

clang barely changes, because its ordered PGO build already fits in RAM. For
hardware A/B runs from now on, the clang baselines are the 1/1 row.

## Decisions for Graham

Decided 2026-11-18:
- **R5:** a separate `/linux/rust.img`, mounted read-only at `/opt/rust` at
  boot (like toolchain.img), with its own install script.
- **Scope:** go through R7, cargo included (offline), unattended.
- **/data:** grow `/linux/data.img` to 2 GiB in place (backup first).

Still open: how much speed work (R8).  The numbers (R4, R6, R7): a one-line
program takes about a minute, a 163-line one 15-20 minutes, a small crate
with a proc macro 12 minutes; most of it is paging the 152 MB driver from
the SD card through 121 MB of RAM (hello world: 10 s of CPU in 77 s).  The
candidates, roughly in order of effort: rustc and LLVM optimised for size
(`opt-level = "s"`, MinSizeRel LLVM is already used), a call-graph order of
the driver as for clang (CLANG_ON_BOARD M6: the text a compile touches,
packed), and fewer relocations at start-up.

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

### R1 (2026-11-18): done, klausscpu-llvm 8a5fc816 (rustc-patches/0005)

- `Arch::KlaussCpu` and `targets/klausscpu_unknown_linux_musl.rs`: PIC by
  default (rustc's rule for a target with dynamic linking), thread-locals on.
  `klauss-linux-cargo` takes it when the rustc has it.
- **Tests:** `library-tests.sh` 30/30 on the board (coretests 2768,
  alloctests 333 + 1331, sync 337, thread_local 12), stdcheck static and
  dynamic PASS; bare metal: `klauss-build all`, ffi_check 90/90, the ABI
  test 960 runs.  Opus review: no blockers, minors fixed.

### R2 (2026-11-18): done

`linux/rustc/build-llvm.sh` (klausscpu-llvm/build-native-rustc-llvm:
MinSizeRel, PIC, threads, 110 libraries, 485 MB, about 10 minutes) and
`linux/rustc/llvm-config`, a host script answering for it.

### R3 (2026-11-18): done

`x.py build --config bootstrap-cross.toml --stage 2 compiler/rustc library`
(klausscpu-llvm `rust/rustc-patches/bootstrap-cross.toml`).  Fixed on the way:
- bootstrap's `cc` did not know the architecture (fork, `crate-patches/cc`);
- bootstrap wanted musl self-contained objects and LLVM tools (excluded);
- a dylib's metadata object and the linked-symbols object were not written
  for KlaussCPU (rustc_codegen_ssa, patch 0005): the metadata went to the
  linker raw, and `librustc_driver.so` came out at 4 MB without the
  compiler's code;
- the compiler's crates needed the `libc`, `linux-raw-sys` and `rustix`
  forks (`setup-crate-forks.sh`).

No backend bug showed.  `librustc_driver.so`: 152 MB stripped (111 MB of
text), 268 000 relative relocations, needs libc++, libc++abi, libunwind and
libc.so; STATIC_TLS (bootstrap builds rustc with initial-exec TLS).

### R4 (2026-11-18): hello world compiles and runs on the board

`linux/rustc/build-rust-image.sh` → `linux-ws/rust.img` (329 MiB: rustc,
cargo, std's rlibs and their `.rmeta` files, which hold the full metadata),
installed by hand as `/linux/rust.img` at `/opt/rust`.

| | wall | user + sys |
|---|---|---|
| `rustc -vV` (first run) | 6.8 s | 1.4 s |
| `rustc hello.rs` (static) | 76 s | 10.4 s |
| the same again | 78 s | 10.6 s |
| `rustc -O hello.rs` | 81 s | 11.5 s |
| `rustc -C target-feature=-crt-static hello.rs` (PIE) | 77 s | 10.5 s |

All three programs print their line.  Nearly all the time is waiting:
the 152 MB driver does not fit in the 121 MB of RAM, so a second run reads
it from the SD card again (R8).  `cargo -V` works (3 s).

### R5 (2026-11-18): done

The system image of runtime 9b24293: rcS mounts `/linux/rust.img` read-only
at `/opt/rust` (staged `rust.img.next` swapped in first, as toolchain.img),
`/opt/rust/bin` on PATH, `install-rust`.  Installed, rebooted:
board-check 16 PASS, 0 FAIL; `/opt/rust` mounted, `rustc` on PATH.

### R6 (2026-11-18): byte-identical

`linux/rustc/identity.sh`: three sources (hello, stdcheck, the threads demo)
at -O0 and -O2, `--emit=obj -C codegen-units=1`, by the board's rustc and by
the x86 stage1 rustc of the same tree (`build-cross`, the same std): all six
objects identical.  Times on the board:

| source | -O0 | -O2 |
|---|---|---|
| hello (1 line) | 64 s | 60 s |
| stdcheck (26 checks, 163 lines) | 900 s | 1177 s |
| threads demo (15 lines) | 240 s | 271 s |

And stdcheck built on the board (`rustc --edition 2024 -O stdcheck.rs`,
1931 s with the link) passes on the board: `stdcheck: PASS`, 26 checks.

### R7 (2026-11-18): proc macros and cargo work, online too

cargo is cross-built with rustc (`x.py build ... cargo`, cargo/0001 and the
crate forks for its newer libc, rustix and linux-raw-sys; it links the
board's OpenSSL from Buildroot's staging directory) and is in rust.img.
`linux/rustc/board-cargo-test.sh` on the board:

| test | build | result |
|---|---|---|
| `cargo new hello && cargo build --release` | 106 s | runs |
| a workspace with a local proc-macro crate, `#[derive(Hello)]` | 722 s | runs: rustc built the macro as a shared library on the board and loaded it |
| a crates.io dependency (`itoa`), fetched by cargo over HTTPS | 613 s | runs |

### /data (2026-11-18)

`/linux/data.img` grown from 256 MiB to 2 GiB while mounted (Python
`os.truncate`, `LOOP_SET_CAPACITY`, `resize2fs`): /data has 1.8 GB free.
The old image is `/linux/data.img.bak-2026-11-18`.
