# klausscpu-runtime

The **software layer** for the KlaussCPU — a custom 32-bit soft-core CPU with its
own LLVM backend. This repo holds everything written *with* the toolchain: a
bare-metal C runtime + test programs, a **FreeRTOS** port (with lwIP, wolfSSL,
wolfSSH, FatFs), and a **Zephyr** port (`zephyr-ws/klausscpu-zephyr`, including an
SSH shell app with loadable LLEXT extensions).

It was split out of the `klausscpu-llvm` fork so the ~8 MB of software isn't
dragged around by the 3.6 GB compiler history. **It is not self-contained: it
needs a *built* `klausscpu-llvm` toolchain.**

## The contract: one env var

The only link to the compiler is the env var `KLAUSSCPU_LLVM_BIN`, pointing at the
fork's built `bin/` directory (no submodule):

```sh
export KLAUSSCPU_LLVM_BIN=<klausscpu-llvm>/build/bin
```

Every build entry point (`baremetal/Makefile`, `freertos/Makefile`, `build-*.sh`, the
wolfSSL/Zephyr cmake toolchains) reads this and **fails loudly if it is unset**.
The fork root (`$KLAUSSCPU_LLVM_BIN/../..`) is also where `compiler-rt/lib/builtins`
is found for the soft-FP / integer-divide `crt-*.o` objects.

**Pinned compiler:** built from `klausscpu-llvm` branch `main`, commit
`ebc99da5` (`Prep for repo split docs`) or newer. The backend is stable; any
recent build of the fork works.

## Prerequisites

- A built `klausscpu-llvm` fork — its `build/bin/` must contain `clang`, `llc`,
  `ld.lld`, `llvm-objcopy`, `llvm-objdump`, `llvm-readelf`, `llvm-ar`, `llvm-nm`,
  `llvm-ranlib`, `llvm-strip`, `llvm-size`. See the fork's own README to build it.
- Host tools: `make`, `meson` + `ninja` (picolibc), `cmake` ≥ 3.16 (wolfSSL),
  `west` + `pip` (Zephyr), `git`.

## Fresh-clone setup (fetched, not committed)

The vendored upstreams are **gitignored** — a fresh clone has only the ~8 MB of
glue. Re-fetch what you need, in order, with `KLAUSSCPU_LLVM_BIN` exported:

```sh
export KLAUSSCPU_LLVM_BIN=<klausscpu-llvm>/build/bin

# 1. picolibc — required by every target (clones, builds, installs to picolibc-install/)
./build-picolibc.sh

# 2. compiler-rt builtins archive — only for the Zephyr/driver-linked builds
#    (the bare-metal Makefile links individual crt-*.o and does NOT need this)
./build-builtins.sh

# 3. lwIP — only for the network demos (clones into lwip/)
./get-lwip.sh

# 4. FreeRTOS kernel — for the freertos/ targets
cd freertos && ./get-freertos.sh && cd ..

# 5. wolfSSL + wolfSSH — for the FreeRTOS TLS/SSH demos and the Zephyr ssh_shell app
cd freertos/wolfssl && ./build-wolfssl.sh && ./build-wolfssh.sh && cd ../..

# 6. Zephyr workspace — for the Zephyr port (see zephyr-ws/klausscpu-zephyr/README.md)
cd zephyr-ws
pip install --user west
west init -l klausscpu-zephyr
west update                              # fetches zephyr/ (v3.7.0) + west modules
# then re-apply the two vendored Zephyr patches (see the Zephyr README)
cd ..
```

## Building

### Bare-metal C programs
The bare-metal build lives in `baremetal/` (the `Makefile` and the `programs/`
test sources). Run `make` there so build artifacts (`.o`/`.elf`/`.d`) stay out
of the repo root. The shared runtime sources (`src/`, `klausscpu.ld`, picolibc,
lwIP, FatFs glue, headers) stay at the root, shared with the other builds.
```sh
cd baremetal
make hello          # uart hello-world
make test_fp        # soft-FP test (links compiler-rt crt-*.o from the fork)
make all            # every .elf
make all OPT=-Oz    # another level (default -O1); -Oz also runs the MachineOutliner
make all LTO=1      # link-time optimisation (make clean when switching)
```
Output is a `*.elf` with machine type `0x4b43` ("KC"). See the top of
`baremetal/Makefile` for the full target list (adventure, expr, bst, crypto,
queens, fs_demo, …).  `LTO=1` compiles the programs to bitcode and lets
`ld.lld` optimise each program whole; it pays at -Os/-Oz (`.text` -3.8% /
-4.4% over the corpus) and not at -O1/-O2 (+2%, and Dhrystone 1.2% slower on
the board at -O1), so it is off by default (`benchmarks/RESULTS.md`).
`OPT="-Os -moutline"` (or `-mno-outline`) also reaches the LTO code
generator: the Makefile passes it to `ld.lld` as `-mllvm
-enable-machine-outliner`, since the bitcode does not record it.

### FreeRTOS
```sh
cd freertos
make demo           # task demo, no network
make net_demo       # FreeRTOS + lwIP: DHCP + NTP + HTTP
```

### Zephyr (SSH shell)
Command-first build guide (env var, one-time `west` setup, the exact verified
`west build` invocation, `.kbt` loading): **[`zephyr-ws/klausscpu-zephyr/CLAUDE.md`](zephyr-ws/klausscpu-zephyr/CLAUDE.md)**.
Background, Kconfig requirements, memory map and LLEXT internals are in the
human-facing [`zephyr-ws/klausscpu-zephyr/README.md`](zephyr-ws/klausscpu-zephyr/README.md).
The `apps/ssh_shell` app produces `zephyr.elf`, loaded to the FPGA with `klausscc`.

## Layout

| Path | Contents |
|---|---|
| `baremetal/` | bare-metal build: `Makefile` + `programs/` test programs (run `make` here) |
| `src/`, `klausscpu.ld` | shared runtime sources + linker script (used by all three builds) |
| `freertos/` | FreeRTOS port, lwIP glue, wolfSSL/wolfSSH, demos |
| `fatfs/` | FatFs glue for the **FreeRTOS** build (tracked) |
| `lwip_port/` | lwIP `sys_arch`/`ethernetif`/`cc.h` for KlaussCPU |
| `zephyr-ws/klausscpu-zephyr/` | Zephyr arch/SOC/board/drivers + `ssh_shell` app |
| `build-*.sh`, `get-*.sh` | upstream fetch/build scripts |
| `klausscpu_elf.h` | `EM_KLAUSSCPU` and the relocation types, for the run-time loaders |
| `check-toolchain-constants.py` | checks the constants copied from klausscpu-llvm (relocation types, register and DWARF numbers, NOP/HALT/TRAP words, the FreeRTOS/Zephyr stack alignment, the registers `setjmp.S` saves) in this repo and in klausscc against klausscpu-llvm's sources; run it after changing either side |
| `tools/validate/` | toolchain validation harness: `validate.sh quick\|full\|board\|zephyr\|csmith N\|csmith-board N\|oracle` runs the emulator corpus against committed UART baselines, Csmith (on the emulator, and on the board against the emulator), the ABI test, lit, the board suite with CoreMark/Embench against `baselines/perf.tsv`, and the Zephyr apps; see [`tools/validate/README.md`](tools/validate/README.md) |
| `abi-test/` | C ABI conformance test: generated caller/callee pairs in separate translation units, every mix of -O0/-O1/-O2/-Os/-Oz, run on the emulator against host gcc (`./abi-test/run_abi_test.sh [first-seed] [count] [functions]`) |

## Validating a toolchain change

`tools/validate/validate.sh` is the check list for a klausscpu-llvm change
([`tools/validate/README.md`](tools/validate/README.md)).  With
`KLAUSSCPU_LLVM_BIN` exported: `validate.sh quick` (about a minute, emulator
only), `validate.sh full` (adds 450 Csmith seeds, the ABI test, lit and an
output-byte oracle), `BOARD_LOCK=<lock> validate.sh board` (board suite,
CoreMark and Embench against the committed perf baseline) and `validate.sh
zephyr`.  It builds in a staged copy under its output directory, never in
this checkout.

## Gotchas

- `picolibc` uses 64-bit `long`, so printf is `%lu`, not `%llu` (no `ll` modifier).
- lwIP checksum: `LWIP_CHKSUM_ALGORITHM=1` in `lwipopts.h` (LDIDX16 reads BE).
- The top-level `fatfs/` is the **FreeRTOS** FatFs glue (tracked) — *not* the
  west-managed `zephyr-ws/modules/fs/fatfs` (untracked/ignored). Don't confuse them.
- **C atomics (`_Atomic`, `<stdatomic.h>`, `__atomic_*`, `__sync_*`) are
  atomic within one core only.** The ISA has no atomic instructions; the
  compiler makes aligned atomic loads/stores of up to 8 bytes plain accesses,
  fences compiler barriers, and every read-modify-write / compare-and-swap a
  call into `src/atomic.c`, which does the plain operation with `INT_MASK`
  saved, zeroed and restored. That is atomic against this core's interrupt
  handlers and RTOS preemption, not against core 2 (AMP): use the mailbox
  and a D-cache flush for that. Atomics wider than 8 bytes or misaligned
  become `__atomic_*` calls, which nothing provides (link error). The
  bare-metal, FreeRTOS and Zephyr builds all link it (Zephyr's own `atomic_t`
  API is `ATOMIC_OPERATIONS_C` and does not use it), and the Zephyr kernel
  exports the `__sync_*` functions to LLEXT extensions
  (`ssh/llext_exports.c`). `baremetal/programs/test_atomic.c` is the test,
  also as an extension (`test_atomic.llext`). This needs a klausscpu-llvm with "forced atomics" (an older
  toolchain makes every atomic an `__atomic_*` call, so `test_atomic` does
  not link).
- `baremetal/programs/test_flags.c` checks which flags (Z S C V) every
  instruction writes against the compiler's model of them (a `Defs = [FLAGS]`
  that is missing is a miscompile).  After changing the backend's instruction
  definitions, regenerate its cases with
  `python3 programs/gen_test_flags.py <klausscpu-llvm>` (it fails when the
  `.td` and its table of the RTL's flag masks disagree) and run it on the
  emulator and the board.
- **Unaligned data:** only `MEMGET32` (a 32-bit load from `[r]`) reads an
  unaligned address; every other 16/32-bit load or store and the A=1
  64-bit forms use the aligned-down address, silently, and the A=0 ("raw")
  64-bit forms pass the address on unmodified and must be given an aligned
  one (CPU_ARCHITECTURE.md 8.3). klausscpu-llvm compiles packed-struct
  fields, `memcpy(&w, p, 4/8)` and bytes assembled into words to
  `MEMGET32` (two for 64 bits) and splits unaligned stores into bytes, but a
  plain `uint32_t *` (or `int *`) pointing at an unaligned address, e.g. the
  address of a packed member, reads and writes the wrong word.
  `baremetal/programs/test_unaligned.c` checks the compiler's unaligned
  loads at every offset, across doublewords and cache lines, on the board.
- **The hardware must be klaussCPU 9ad6614 or later.** klausscpu-llvm
  assumes its squash fixes: a taken fused branch (FBR) may be followed
  directly by a divide or an LCD write (`divr`/`divur`/`modr`/`modur`/
  `divv`/`modv`, `lcdcmdv`/`lcddatav`/`lcddatar`), compiled or hand-written.
  On an older bitstream the squashed divide kept the divider running and
  the next divide returned its result, and the squashed LCD write still
  reached the LCD; the compiler used to avoid the shape and the assembler
  warned about it, and neither does now. Older bitstreams are not
  supported: rebuild everything (picolibc too, `./build-picolibc.sh`) with
  the current toolchain and run it on the current core.
  `baremetal/programs/test_divshadow.c` is the board test (the emulator has
  no pipeline and always passes it).
- **The C ABI changed (klausscpu-llvm `llvm/lib/Target/KlaussCPU/ABI.md`,
  version 2):** `int`/`unsigned` arguments and results are sign-extended to
  64 bits, structs of up to 16 bytes are passed in registers, and only
  variadic calls reserve the argument home area.  Code built by an older
  toolchain does not mix with it: rebuild picolibc (`./build-picolibc.sh`),
  the builtins (`./build-builtins.sh`), wolfSSL/wolfSSH
  (`freertos/wolfssl/build-*.sh`), Zephyr pristine (`west build -p always`)
  and `make clean` the bare-metal and FreeRTOS builds (make does not notice a
  new compiler).  lld refuses to link objects of different ABI versions (the
  low byte of `e_flags`), so a stale object fails the link instead of the
  program, and the PIC loaders (`baremetal/programs/loader.c`,
  `freertos/telnet/pic_loader.c`) and Zephyr's LLEXT loader
  (`arch_elf_check_header()` in `arch/klausscpu/core/elf.c`) refuse a program
  or extension built for another ABI version.  The hand-written assembly here (`setjmp.S`, the context
  switches, the ISR wrappers) did not need changes: the register roles are
  the same.
- A program ends with `HALT` (exit status in R12) when `main` returns or
  `_exit` is called; `__builtin_trap()` is the `TRAP` instruction, an
  abnormal stop (crash dump on the board, `stop = Trap` in the emulator).

## Relationship to `klausscpu-llvm`

```
klausscpu-llvm  (the fork)   →  builds the toolchain  →  build/bin/{clang,ld.lld,...}
        │                                                        │
        └──────────────── KLAUSSCPU_LLVM_BIN ───────────────────┘
                                  ↓
klausscpu-runtime (this repo) →  builds the software against it
```

A change spanning both repos (e.g. a new backend intrinsic + its use here) is two
commits in two repos — rare, since the backend is stable, but coordinate it.
