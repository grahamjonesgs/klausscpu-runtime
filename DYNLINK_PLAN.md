# Dynamic linking for KlaussCPU Linux — plan

Started 2026-11-11 at Graham's request. Orchestrated from the runtime
session (klausscpu-runtime); the LLVM work is done by the klausscpu-llvm
session, the musl, kernel and system work by the runtime session.

## Goal

Shared libraries and a dynamic loader for user programs on the Linux
port: `libc.so` and `ld-musl-klausscpu.so.1` from musl, `-shared` and
dynamically linked executables from klausscpu-llvm, then Buildroot built
with shared libraries.

Why (LINUX_PLAN.md has the measurements):
- **Packages.** 559 of Buildroot's 2962 packages refuse static-only builds:
  Python 3, fio, rt-tests, MicroPython, Ruby, Tcl, Erlang, LuaJIT, BlueZ,
  Samba, kmod, PAM, Wayland...
- **One library, one fix.** Today's musl fix (`__init_tls`) needed every
  program relinked and a 12-minute image install; with `libc.so` it is one
  file. The same for OpenSSL, zlib, libcurl security fixes.
- **Size and memory.** Every static binary carries its own musl, OpenSSL,
  curl, ncurses: the system image needs ~160 MiB. Shared library text is in
  memory once. (With 64 KiB pages each library's writable data costs at
  least 64 KiB per process, so small libraries can cost more than they
  save: measure.)
- **dlopen** (Python C extensions, Lua C modules, OpenSSL providers, PAM),
  **LD_PRELOAD** (malloc debuggers, tracing shims, fakeroot), and **ASLR**
  with PIE.

Costs: slower program start (relocation at load, at 100 MHz), a few
cycles per call through the PLT, and the work below.

## Where we start

| Part | State |
|---|---|
| Kernel | ready in principle: `binfmt_elf` loads `PT_INTERP`, `ET_DYN` (`ELF_ET_DYN_BASE` is set), passes `AT_BASE`/`AT_PHDR`/... (64-bit auxv entries). Unchecked: a PIE and an interpreter loaded together, 64 KiB alignment |
| klausscpu-llvm codegen | a PIC mode exists (bare metal and FreeRTOS load PIC programs): own symbols are reached PC-relative with `LEAPC`; no GOT, no PLT |
| klausscpu-llvm MC / ELF | relocations 0-7 only (`ELFRelocs/KlaussCPU.def`): ABS32/64, PCREL32, PC32/64, CALL18, TPREL32; all static |
| lld | links static executables only: no `-shared`, no PIE, no GOT/PLT, no dynamic relocations |
| musl port (`linux/musl`) | static only: `reloc.h` says so, `SHARED_LIBS` empty, no `libc.so`; TLS local-exec only |
| musl's dynamic linker | **assumes the ELF class matches the pointer size**: `ldso/dynlink.c` reads `_DYNAMIC` and the relocations as `size_t` arrays (`decode_vec`, `do_relocs(..., size_t *rel, ..., stride)`) and picks `Elf32_*`/`Elf64_*` from `UINTPTR_MAX`. KlaussCPU is LP64 (8-byte `long` and pointers) in **ELF32** files (4-byte `d_val`, `r_offset`, `r_info`, `st_value`): that has to be adapted (the `__init_tls` bug fixed today was the same mismatch) |

## The ABI to define (M1)

To be written as a "Dynamic linking" section of
`llvm/lib/Target/KlaussCPU/ABI.md` by the LLVM session and reviewed
(runtime session plus an independent review) before code. Proposed
starting points, to be checked against the ISA (`LEAPC`, `CALLREL`,
`JMPR`, immediate widths) and changed where the LLVM session finds better:

- **Pointer-sized dynamic fields are 64 bits.** Pointers are 8 bytes, so
  GOT entries and data pointers relocated at load time are 64-bit fields
  in an ELF32 file. RELA (`Elf32_Rela`) as today.
- **New relocation types** (numbers after TPREL32 = 7):
  - static: `GOTPCREL32` (word 1 of `LEAPC`: the GOT entry's address, PC
    relative), `PLT32` (word 1 of `CALLREL`/jumps: the PLT entry, or the
    function itself when it is not preemptible), `TLS_GOTTPREL32` (IE: the
    GOT entry holding the TP offset), `TLS_GD32` (GD: the GOT pair for
    `__tls_get_addr`), and whatever local-dynamic needs, if anything;
  - dynamic: `RELATIVE` (B + A, 64-bit), `GLOB_DAT` (S, 64-bit GOT entry),
    `JUMP_SLOT` (S, 64-bit), `COPY`, `ABS64` (exists: symbolic data
    pointer), `TPOFF64`, `DTPMOD64`, `DTPOFF64`.
- **Code sequences.** Own symbols: `LEAPC` (as now). Preemptible data:
  `leapc rT, %gotpcrel(sym)` then `ldidx64 rT, rT, 0`. Calls: `CALLREL`
  to `%plt(sym)`; lld points it straight at the function when it isn't
  preemptible. TLS: local-exec (have it) in executables, initial-exec
  (`%gottprel`: load the offset from the GOT, add TP) for libc and
  executables, general-dynamic (`%tlsgd`, call `__tls_get_addr`) for
  `dlopen`ed libraries. No TLSDESC at first.
- **PLT.** musl binds everything at load (no lazy binding), so no PLT0
  resolver: each entry is load-from-its-`.got.plt`-slot and jump, through
  a scratch register the call ABI leaves free (to choose: a caller-saved
  register that is not an argument). JUMP_SLOT fills the slot.
- **Executables.** First dynamically linked non-PIE executables (simplest
  codegen; COPY relocations for data such as `environ`, `stdout`), then
  PIE as the default once it works (ASLR).
- **Page size.** lld must use 64 KiB maximum page size and segment
  alignment for KlaussCPU (`-z max-page-size`), as the kernel maps 64 KiB
  pages.
- **Interpreter.** `/lib/ld-musl-klausscpu.so.1` (musl's `LDSO_ARCH`
  is already `klausscpu`).
- **Unchanged:** static linking, bare metal, Zephyr, FreeRTOS, the
  kernel, xv6 and Rust: every new form is opt-in (`-fPIC`/`-shared`/
  `-pie`/dynamic linking), and the existing PIC loaders must keep working.

## Milestones

| # | What | Who | Done when |
|---|---|---|---|
| M1 | ABI section (above), reviewed | LLVM session; review by runtime session + independent reviewer | agreed text in ABI.md on the `dynlink` branch |
| M2 | codegen + MC: GOT/PLT code sequences under `-fPIC`/`-fPIE`, the new relocations and `%gotpcrel`/`%plt`/`%gottprel`/`%tlsgd` in the assembler, parser and printer | LLVM session | lit tests (CodeGen, MC round trip, errors); all existing KlaussCPU tests pass; static output byte-identical for non-PIC code |
| M3 | lld: `-shared`, dynamically linked executables, PIE, GOT/PLT, dynamic relocations, `.dynamic`, `PT_INTERP`, TLS IE/GD, 64 KiB pages | LLVM session | lld lit tests; a shared library and an executable using it link, and `llvm-readelf -d -r` shows what the ABI says |
| M4 | musl: ELF32-on-LP64 dynamic linker, `reloc.h`, `_dlstart`, `dlsym`, `__tls_get_addr`, `libc.so` + `ld-musl-klausscpu.so.1`, `Scrt1.o` | runtime session (with an agent) | builds; `ldd` runs |
| M5 | bring-up on the emulator: hello world, a library with data + functions + TLS, `dlopen`/`dlsym`, threads with TLS in shared libraries, interposition, `LD_PRELOAD`, constructors; then musl's libc-test if it ports | runtime session | all pass on klausscc |
| M6 | the board: the same, and start-up time, RSS and image size against static | runtime session | measured |
| M7 | Buildroot with shared libraries (toolchain wrapper without `-static`, `BR2_SHARED_STATIC_LIBS`), the 40 packages, then Python 3 | runtime session | buildroot-test passes; `python3` runs |

M2 and M3 can overlap; M4 can start from the ABI text before M3 is
finished (it needs only the relocation semantics), then meet at M5.

## M1 review (2026-11-11): what lands on the musl / kernel side

The ABI text (klausscpu-llvm `dynlink`, ABI.md "Dynamic linking") passed an
independent review with no blockers. Items for M4 and the kernel:
- `<link.h>`: ElfW Phdr/Dyn/Sym/Half are Elf32, every pointer/Addr field in
  `dl_phdr_info`, `link_map`, `r_debug` is 8 bytes. The static
  `dl_iterate_phdr` already misreads PT_TLS / PT_GNU_EH_FRAME today (the
  `__init_tls` bug class): fix with M4.
- `r_addend` (Elf32_Sword) is sign-extended for every relocation type.
- `libc.so` link: musl's configure flags (`--dynamic-list`,
  `--exclude-libs=ALL`, `--no-undefined`, symbolic binding of libc's own
  calls) and Linux-triple, `-fPIC`, hidden-visibility compiler builtins
  (the LLVM session builds them).
- `map_library` checks `e_machine` and the `e_flags` ABI version.
- `crt_arch.h` `_dlstart`: `.hidden _DYNAMIC`, `leapc`.
- musl's `jmp setjmp` / `jmp __cancel` / `jmp __sigsetjmp_tail` become
  tail calls (they'd be PCREL32 against preemptible symbols).
- Kernel: no ASLR yet (no `ARCH_HAS_ELF_RANDOMIZE`): PIEs load at
  0x55550000 until it is added.

## Progress

| # | State (2026-11-11) |
|---|---|
| M1 | done: ABI.md "Dynamic linking" on klausscpu-llvm `dynlink` (505579e, `<link.h>` ElfW(Addr) wording 9e5fefc) |
| M2 | done, reviewed: codegen + MC (505579e..a25b4bb); non-PIC and non-Linux output byte-identical; Linux C++ exception tables change (indirect pcrel personality) |
| M3 | done, reviewed (1 major, 4 minor fixed): lld `-shared`/PIE, GOT/PLT, dynamic relocations, TLS IE/GD, 64 KiB pages and `-z now` for dynamic outputs; driver `-shared`/`-pie`. M2+M3 fast-forwarded to llvm `main` (432b009); `build/` rebuilt, the Linux builtins in it (`BUILTINS_TRIPLE=klausscpu-unknown-linux-musl ./build-builtins.sh`); bare-metal objects byte-identical to the pre-M2 toolchain |
| M4 | done, reviewed, merged to runtime main (52999f1): `linux/musl/patches/` (ELF32 dynamic structures with LP64 pointers, gated on `DL_ELFCLASS`; arch ELF header check; `<link.h>` `__ELFW_ADDR`), `MUSL_SHARED=1` build in its own workspace; the static `libc.a` is now non-PIC (it was built from the -fPIC objects) and `dl_iterate_phdr` reads Elf32_Phdr |
| M5 | done on the emulator: `linux/userland/tests/dynlink/run.sh`, 185 checks pass (library data/functions/function pointers/RELRO, protected/weak/interposed symbols, constructors and destructors in order, TLS GD/IE/LE with 4 threads, `dl_iterate_phdr`/`dladdr`/`r_debug`, `dlopen`/`dlsym`/`RTLD_NEXT`/`RTLD_GLOBAL` with TLS in threads made before and after, `LD_PRELOAD`, `LD_LIBRARY_PATH`, `$ORIGIN`, setjmp/signals across modules, bad ELF files refused). Start-up per program (fork+exec+wait, emulator): static 4.9 ms, dynamic 9.0 ms, dynamic + one library 12.9 ms. It found a kernel race (an interrupt in the page fault return path zeroed r12; fixed in 03b354c) |
| M6 | the board (perf-hw c1874cf): `linux/userland/tests/dynlink/board.sh` runs run.sh's programs in a chroot on /tmp (the system image has no /lib yet); all 141 guest checks pass. Start-up (fork+exec+wait, 500 runs): static 18.4 ms, dynamic 32.8 ms, + one library 48.5 ms; see below |

M6, per start-up (perfstat over 200 runs):

| | static | dynamic | + libdt1.so |
|---|---|---|---|
| instructions | 231k | 451k | 670k |
| cycles | 1.87M | 3.36M | 4.91M |
| D-cache lines zeroed (DCBZ: page clearing, memset) | 10.6k | 19.7k | 33.1k |
| D-cache read / write misses | 19.6k / 12.3k | 33.6k / 23.2k | 50.0k / 34.2k |

Each module costs ~1.5M cycles (15 ms): about 220k instructions of
relocation and symbol lookup, and ~4-7 freshly zeroed 64 KiB pages (its
writable data, bss, GOT and RELRO each start a page; ~9-13k zeroed lines,
2048 per page). Ideas, not done: no separate RELRO page for small
libraries (one writable page instead of two), prelinking-style symbol
caches, and fewer, bigger libraries (busybox-style) in M7.

The dynlink branch work merged to llvm `main` (M2+M3, 432b009); the
`build-dynlink` directory is no longer needed.

Notes from M5:
- `-fPIC` through the driver means `-fhalf-no-semantic-interposition`:
  a library's calls to its own default-visibility functions can be
  inlined or constant-folded, so interposing them from the program needs
  the library built with `-fsemantic-interposition` (as on other ELF
  targets with clang).
- Run as a command (`ld-musl-klausscpu.so.1 prog`), musl leaves
  `getauxval` describing the loader (upstream behaviour).
- `$ORIGIN` for the main program needs `/proc` mounted.
- On every load `map_library` zeroes the rest of each writable page; lld
  pads the RELRO segment to the 64 KiB page, so that's up to ~60 KiB of
  memset per library: a start-up cost to look at in M6.

## Working rules

- **klausscpu-llvm** (until the merge on 2026-11-11): branch `dynlink`, built in a **separate build
  directory** (`build-dynlink`): everything else (the kernel, Buildroot,
  the runtime) keeps using `build/` from `main` until the branch is
  reviewed and merged. Commits stay local until review; the runtime
  session merges to `main` (Graham's standing permission for non-breaking
  LLVM work) after an independent review.
- Every milestone is checked by tests, and each correctness-critical piece
  (codegen, lld relocation handling, the musl loader) by an independent
  review before it merges.
- Board use follows the protocol with the klaussCPU session (both locks,
  announcements).
- Keep big temporary files out of /tmp (small quota).

## Risks

- **musl ELF32 + LP64.** `dynlink.c` (~2400 lines) mixes ELF words and
  `size_t`. The adaptation must be complete (the `dynv` decode, the
  relocation walk, symbol tables, hash tables, `dl_iterate_phdr` and
  `<link.h>`'s `ElfW`, the vDSO if one is added). A missed spot reads
  garbage, as `__init_tls` did.
- **64 KiB pages.** Segment alignment, `RELRO`, and the per-library cost
  of a 64 KiB writable data page per process.
- **Start-up cost** at 100 MHz with relocation processing for big
  libraries (OpenSSL): measure, and use `-z now` (musl binds at load
  anyway).
- **Two toolchains in use** during the work (`build/` and
  `build-dynlink/`): don't mix their objects.
