# Linux on KlaussCPU — plan

**Status (2026-11-09): milestones 0–3 are done on the klausscc emulator,
and milestone 4 is done except for busybox.**
- v7.3.3 builds with klausscpu-llvm (`linux/build.sh`) and boots through
  `start_kernel` to user space. The console is `hvc0` on the polled UART.
- musl 1.2.5 is ported (`linux/musl/`). klausscpu-llvm `fc94ed9f` adds the
  `klausscpu-unknown-linux-musl` triple and its static link.
- `minish` (`linux/userland/minish.c`), a small musl shell, runs as
  `/init` with typed input (`klausscc --uart-in`). It forks and execs
  programs; `/proc` and tmpfs work.
- `tools/validate/linux.sh` checks this (`shell` mode) and milestone 3
  (`m3` mode).

Busybox (`linux/userland/busybox.sh`, `busybox.config`, initramfs profile
`busybox`) is written but has not been run: building it compiles and runs
busybox's host tools, which this session's permission classifier refused.
Next after busybox: milestone 5 (the board; a milestone-3 `vmlinux` is
waiting for its first try).

Goal: mainline Linux (`ARCH=klausscpu`) on the stage 3 release bitstream,
klaussCPU `mmu` 52bece8 LEAN:
- `CPU_FEATURES` = 0x0010_100D: exceptions and user mode, a 16-entry TLB
  with 64 KiB pages, LR/SC and the TRNG;
- no MPU, and no crypto blocks other than the TRNG.

It boots on the klausscc emulator first, then on the board, from an
initramfs, to a busybox shell on the UART.

## Facts the port is built on

| | |
|---|---|
| Core | 64-bit registers r0–r15, 32-bit addresses (EA[31:0]), little-endian, in order, 100 MHz. LP64: 64-bit pointers and `long`. |
| ABI (klausscpu-llvm) | Arguments in R0–R3, then the stack. Result in R12 (R11:R12 for 128-bit). Callee-saved R4–R7 and R15 (P, the frame pointer). SP is a separate register (`getsp`/`setsp`). |
| Modes | UM (user) and supervisor. `SP_ALT` is the inactive SP; it's SR `USP` in supervisor mode (MMU_PLAN §3.1). |
| Entry | Interrupts and exceptions alike (§3.3): from user mode, swap to the kernel SP, then push one 64-bit frame. The frame is PC [31:0], flags [38:32], INT_MASK [42:39], DI [43] and PM [44] (§3.2). An exception writes ECAUSE/EADDR, **sets DI** and jumps to `EVEC`. An interrupt clears its own `INT_MASK[n]`, leaves DI alone and jumps to `INT_VECn`. IRET restores everything, including `UM = PM` and the SP swap (§3.4). |
| SRs | STATUS, EVEC, ECAUSE, EADDR, USP, SCRATCH0/1 (the kernel's), TP (user TLS, context-switched), CYCLE (64-bit, user-readable), TLB_HI/LO/INDEX, ASID, TLBVEC (§6.1, §11.3). |
| TLB | 16 entries, fully associative, software refill. 64 KiB pages. 8-bit ASID plus a G bit. `TLBWI`/`TLBR`/`TLBP`/`TLBINV`. TLB_HI is auto-loaded on a miss, and misses go to `TLBVEC` if it's set (§11.3). |
| Address map (TE = 1, §11.2) | useg `0x0000_0000–0x7FFF_FFFF` (TLB). kseg `0x8000_0000–0x87FF_FFFF` (direct, PA = VA − 0x8000_0000, supervisor). `0x8800_0000–0xBFFF_FFFF` faults. kmap `0xC000_0000–0xEFFF_FFFF` (TLB, supervisor). MMIO `0xF000_0000+` (identity, supervisor). |
| Rules (§11.3) | Exception vectors, the refill handler, page tables and kernel stacks must be in kseg, because frame pushes aren't translated. A push outside kseg with TE = 1 is a double fault. TLB R/W/X are checked in supervisor mode too. |
| Memory | 128 MiB DDR at PA 0, all of it in kseg. D-cache 64 KiB 2-way, I-cache 8 KiB direct-mapped. The I-cache snoops CPU stores by line, but code that arrives by DMA, or is written through one mapping and run through another (kseg vs useg, as `test_tlb` T5 does), needs `ICACHE_INV`. |
| Devices | UART (polled, no RX interrupt), interval timer and 64-bit ms clock, interrupt controller (4 sources: timer, blitter, Ethernet, VGA; per-source vectors), LiteEth, SD card, QSPI flash, TRNG. |

## Workspace

- `linux-ws/` (gitignored) holds the upstream checkout, `linux-ws/linux`, a
  shallow clone of stable **v7.3.3**, and the build tree `linux-ws/build`
  (`O=`). Both are recreated by `linux/fetch.sh`.
- `linux/` (tracked) holds the port:
  - `linux/arch/klausscpu/` is the architecture. `fetch.sh` symlinks it into
    the checkout.
  - `linux/patches/NNNN-*.patch` are changes to generic code, applied in order.
  - `linux/tools/prebuilt-parser.py` stands in for bison/flex, which aren't
    installed. It emits kconfig's parser and lexer from the generated files
    in `/usr/src/linux-headers-*/scripts/kconfig`, with 7.3.3's action-code
    differences applied (the grammar is the same).
  - `linux/build.sh` is the `make` wrapper (`ARCH=klausscpu LLVM=<bin>/`,
    `HOSTCC=gcc`, `O=`).
  - `linux/toolchain-tests/` is the compiler, assembler and linker survey.

## Design

Model: **OpenRISC** for overall shape. It's a small arch with a software-
refilled TLB and leans on asm-generic. **MIPS64** is the model for a kernel
in a 32-bit direct-mapped segment (CKSEG0), the refill handler and the
`pgd_current` approach. **LoongArch** shows a 64-bit software-TLB port with
direct-mapped windows. RISC-V shows the generic syscall table and
`GENERIC_ENTRY`.

### Memory layout

- `CONFIG_64BIT`, with 32-bit hardware addresses. Kernel pointers are
  zero-extended kseg/kmap addresses.
  - `PAGE_OFFSET` = `0x8000_0000` and `PHYS_OFFSET` = 0, so `__pa(x)` = x − 0x8000_0000.
  - All 128 MiB is lowmem: no HIGHMEM, `FLATMEM`, no `SPARSEMEM_VMEMMAP`.
- `TASK_SIZE` = `0x8000_0000` (useg). `access_ok()` rejects anything at or
  above it, including pointers with bits [63:32] set, which the hardware
  would otherwise truncate.
- kmap `0xC000_0000–0xEFFF_FFFF` holds vmalloc, modules and fixmap/kmap
  (TLB-mapped, global entries).
- MMIO is identity-mapped at `0xF000_0000+` with no TLB entry needed, so
  `ioremap()` of an MMIO range can return the address itself.
- `THREAD_SIZE` = 16 KiB from the slab, in kseg. No `VMAP_STACK`, since
  kernel stacks must be in kseg.

### Pages and page tables

- `PAGE_SIZE` = 64 KiB (`HAVE_PAGE_SIZE_64KB`), the only TLB page size.
- PTEs are 8 bytes, 8192 per page, so one PTE page maps 512 MiB.
- **Two levels**: pgd → pte (`asm-generic/pgtable-nopmd.h`).
  `PGDIR_SHIFT` = 29, so the pgd has 8 entries covering 4 GiB.
  - Four entries are user space and two are kmap.
  - The first version allocates one pgd page; a small slab comes later.
  - `pgd_alloc` copies the kernel (kmap) entries from `swapper_pg_dir`.
- PTE low 32 bits = `TLB_LO` format: PFN [26:16], X [3], W [2], R [1], V [0].
  `MTSR tlb_lo` keeps only those bits, so the refill handler writes the PTE
  unmasked. Software bits are in [63:32]: PRESENT, DIRTY, YOUNG, SPECIAL,
  PROTNONE, and the swap encoding.
- Hardware W is set only when the PTE is both writable and DIRTY (the MIPS
  scheme). The first store faults, `handle_mm_fault` marks the page dirty,
  and `update_mmu_cache` rewrites the TLB entry.
- YOUNG: at first every present PTE counts as young; MIPS-style V-bit
  ageing can come later.

### TLB refill and faults

- **`TLBVEC` refill handler** (kseg, assembly):
  - A miss is always taken on a kseg kernel stack: from user mode the entry
    has already switched to it, and in supervisor mode SP is a kernel stack.
    So the handler may push.
  - It picks the pgd by VA: `swapper_pg_dir` for kmap (≥ 0xC000_0000),
    otherwise `pgd_current`, which `switch_mm` keeps in a variable.
  - pte = pgd[VA >> 29] → [(VA >> 16) & 8191]. If PRESENT: `MTSR tlb_lo`,
    then `TLBWI` at a round-robin index over the non-wired entries, then
    `IRET`.
  - Otherwise it branches to the general exception path, which becomes
    `do_page_fault`.
  - TLB_HI already holds {VPN, ASID}; G is set for kmap.
- **ASIDs** (8-bit) use a MIPS/arm64-style generation allocator, with a
  `TLBINV` on rollover. The first version just flushes on every
  `switch_mm`. `flush_tlb_page` uses `TLBP` plus an invalidating `TLBWI`.
- **Faults.** TLB_MISS_* with no PTE, and ACCESS_* (a permission or
  W-not-dirty fault), go to `do_page_fault` → `handle_mm_fault`. A kernel
  fault on a user address goes through the exception table (uaccess fixups).

### Exceptions, interrupts and system calls

- **One kseg entry stub** for EVEC and every INT_VEC. It saves r0–r15 and
  USP, the hardware frame, ECAUSE/EADDR, and `orig_r0`/`syscallno` in a
  `struct pt_regs` on the kernel stack. `GENERIC_ENTRY` handles the rest:
  `irqentry_enter/exit`, `syscall_enter_from_user_mode`, signals on return.
- **Interrupt entry.** The stub does `DI` first, because hardware entry
  doesn't. It records which source was taken, so each `INT_VECn` has its
  own short prologue. The handler unmasks the source when done, since
  entry auto-masked it.
- **System calls** use the asm-generic syscall table (`unistd.h`, as
  RISC-V/LoongArch/OpenRISC do).
  - **ABI:** `SYSCALL` with the number in **r12**, arguments in **r0–r5**,
    result in **r12**.
  - The frame PC already points past `SYSCALL`, so restarting rewinds by 4.
  - `orig_r0` and `syscallno` are kept for restart and ptrace.
- **`current`** lives in `SCRATCH0` (`mfsr`, one instruction), so no
  `-ffixed` register is needed (klausscpu-llvm doesn't support that).
  `SCRATCH1` is free. With no MPU, the entry stub doesn't need scratch
  registers for a guard-safe path.
- **Context switch.** `__switch_to` saves and restores R4–R7, R15, SP and
  the return address in `thread_struct`, plus `TP` (user TLS) and
  `SCRATCH0` (`current`). USP lives in each thread's `pt_regs`.
- **Signals.** `rt_sigframe` on the user stack, with `rt_sigreturn` through
  a trampoline in a vDSO page; at first in a fixed "sigpage" mapped into
  every process (no executable stack).
- **uaccess.** The kernel touches user memory directly through useg (TLB
  R/W/X apply to supervisor too), with an `__ex_table` fixup on each
  accessing instruction. Faults inside them are restartable and precise
  (§4.1).

### Time, interrupts, console

- **clocksource and `sched_clock`:** `CYCLE` (100 MHz, 64-bit,
  user-readable, so a vDSO `clock_gettime` is possible later).
- **clockevent:** the interval timer (`TIMER_PER`, periodic, source 0). Its
  frequency comes from Kconfig or a built-in constant.
- **irqchip:** a linear domain with 4 hwirqs over `INT_MASK`/`INT_PEND`
  (`GENERIC_IRQ_MULTI_HANDLER`).
- **Console:** earlycon/bootconsole on the polled UART, then a tty driver
  that polls RX from an hrtimer, since there's no RX interrupt. This works
  with `--uart-in` scripted input on the emulator.
- **No device tree at first:** memory is `memblock_add(0, 128 MiB)` and
  devices are platform devices registered in arch code. DT (built-in DTB)
  can come later if drivers need it; LiteEth in mainline uses DT.

### Atomics, SMP, caches

- `!SMP`. At first, atomics are the asm-generic UP versions, which disable
  interrupts. Then `atomic*`/`cmpxchg` move to LR/SC
  (`lr64`/`sc64`, `lr32`/`sc32`).
- Futexes use LR/SC on user addresses, with exception-table fixups.
- **Caches:**
  - DMA is coherent.
  - `flush_icache_range` (module load, `ptrace` pokes, signal trampolines,
    `exec` through the page cache) does an `ICACHE_INV`.
  - The D-cache is PIPT, so 64 KiB pages leave no alias problems (§11.1).

### Boot

- **Loading.** klausscc loads the `vmlinux` ELF by LMA. The kernel is
  linked at kseg VMAs (`0x8010_0000`), with `AT()` LMA = VMA − 0x8000_0000.
  The initramfs is built in (`INITRAMFS_SOURCE`). The board load path will
  need checking for a multi-MiB image: UART upload speed, or the netboot
  loader.
- **`head.S`** starts physical, with TE = 0:
  1. Wire one global identity TLB entry for its own 64 KiB page (§11.2:
     the next fetch after TE goes on is translated).
  2. Point `EVEC`/`TLBVEC` at kseg.
  3. Set TE, then jump to the kseg alias.
  4. Drop the wired entry.
  5. Clear BSS, set the init stack and `SCRATCH0` = `&init_task`, then
     call `start_kernel`.
- Probe `CPU_FEATURES` and refuse to boot without bits 0, 2 and 3.
- The command line is built in (`CMDLINE`).

### User space

- A **musl** port (`arch/klausscpu`):
  - `syscall_arch.h` uses register variables r0–r5 and r12, plus
    `syscall`.
  - `atomic_arch.h` uses LR/SC; `__set_thread_area` writes TP.
  - Also `crt_arch.h`, `setjmp`, `clone`, `syscall_cp`, `sigsetjmp`, the
    signal context, `reloc.h`, and the `bits/` headers.
  - Static linking first.
- **Compiler.** The `klausscpu-*-linux-*` triple compiles but doesn't
  define `__linux__`/`__unix__`, and the driver has no Linux toolchain
  (sysroot, crt files, `-static` link). That's a klausscpu-llvm change
  needed before musl and busybox.
- **busybox**, static, with a minimal config: sh, coreutils, mount, init.

## Toolchain status

See `linux/toolchain-tests/RESULTS.md` (`run.sh` regenerates it). As of
2026-11-09 the kernel-relevant surface mostly passes:
- passes: asm goto with and without outputs; `%c`/`%n`; `"i"(&sym)`;
  `X`/`m`; naked; the attributes the kernel uses; the builtins; jump
  tables; computed goto; macros and `.altmacro`; push/popsection; kernel
  linker-script features; `ld -r`; thin archives; and the binutils
  equivalents.
- Gaps:
  1. `.word`/`.hword` are unknown directives. Generic and ported `.S`
     code use them; `.long`/`.short` work.
  2. `-fpatchable-function-entry` is unsupported, so no ftrace for now.
  3. `-ffixed-<reg>` is unsupported; `current` lives in SCRATCH0 instead.
  4. `__thread` (TLS) is unsupported. The kernel doesn't need it; user
     space (musl) does, with TP-relative access.
  5. The Linux triple doesn't define `__linux__` and has no Linux driver
     toolchain (user space only).

## Findings while porting

- **ECAUSE numbering:** ILLEGAL 1, PRIVILEGED 2, TRAP 3, SYSCALL 4,
  MISALIGNED_LOAD/STORE 5/6, ACCESS_LOAD/STORE/FETCH 7/8/9,
  MISALIGNED_FETCH 10, TLB_MISS 11–13. `asm/klausscpu.h` uses these.
- **`bextr` only reaches bits below 32.** The compiler never uses it for
  higher bits, so the entry code tests frame[44] PM with `shrv`/`andi`.
- **`setr` sign-extends a 32-bit immediate; `setru` zero-extends.**
  Kernel addresses (0x8000_0000+) must use `setru`, as the compiler does
  for symbols.
- **klausscc loads by VMA, relative to the first segment**, at PA 0x20,
  and ignores `p_paddr`. So the kernel links at kseg `0x8000_0020`.
- **User binaries are ELF32**; `ELF_CLASS` = ELFCLASS32 with an LP64 ABI.
  `EM_KLAUSSCPU` = 0x4B43.
- **The fused branch conditions** (`fbr`/`fbri`) are EQ 0, LT 1, LE 2,
  ULT 3, ULE 4, and +8 inverts them (NE 8, GE 9, GT 10, UGE 11, UGT 12).
- **ELF32 files with an LP64 ABI:** the user stack's argc/argv/envp/auxv
  must be longs. Patch 0002 lets the arch set `elf_addr_t` (ARCH_ELF_ADDR_T
  = `unsigned long`); with 32-bit words, musl read argv[0] as NULL.
- **EI takes effect at WB, about 3 cycles later, on the stage 3 bitstream**
  (98fe470). An `ei; di` window never takes a pending interrupt, so the
  idle loop is `ei; wait`.
- **`fbr`/`fbri` can't branch to a global symbol** (no relocation for
  them), so asm uses a local label.
- **`INT_MASK` stays 0xF**, and a source is enabled by setting its
  `INT_VECn` (as in the Zephyr port). That way IRET never restores a
  stale mask from an old frame.

## Milestones

0. **Groundwork.** *(done)*
   - Workspace, patches and kconfig without bison/flex.
   - `make ARCH=klausscpu defconfig` succeeds.
   - Arch skeleton; record how far the build gets.
1. **Banner.** *(done)*
   - `head.S`, `setup_arch` (memblock, `paging_init`) and earlycon on the
     emulator.
   - Checkpoint: `Linux version 7.3.3 ... klausscpu` printed by the
     klausscc emulator.
2. **`start_kernel` completes.** *(done)*
   - Traps, IRQs, timer, scheduler, idle, slab, VFS and the initramfs
     unpacked.
   - Checkpoint: `Kernel panic - not syncing: No working init found` (or
     `Run /init`), with timer ticks advancing jiffies.
3. **First user process.** *(done on the emulator)*
   - User mode, SYSCALL, TLB refill, page faults, ELF exec.
   - A static libc-less `/init` in the initramfs (`write`, `getpid`,
     `fork`, `wait4`, `exit`) prints and loops.
   - Checkpoint: its output, and `fork` working.
4. **busybox.** *(musl and a shell done; busybox not yet built)*
   - musl and the clang Linux toolchain.
   - Checkpoint: a busybox shell on the emulator, scripted with `--uart-in`.
5. **Board.**
   - Same image on 52bece8; tune load speed and timer frequency.
   - Checkpoint: an interactive shell over the UART.
6. **Later.**
   - ASIDs and LR/SC atomics if milestones 2–4 didn't need them.
   - vDSO, LiteEth and networking, SD card, ftrace (once the compiler has
     patchable entries), modules.

## Risks

- **Emulator ≠ board** (no cache or cycle model). xv6 milestone 3 finds
  the first of these.
- **Kernel size versus load time.** `vmlinux` plus initramfs will be
  several MiB; UART upload may need the netboot path.
- **16 TLB entries with 64 KiB pages:** 1 MiB reach. The kernel uses kseg
  (no TLB) for nearly everything, so only user space and vmalloc compete.
  Watch the miss rate.
- **Compiler maturity.** The kernel is far larger than anything compiled so
  far. Expect codegen bugs; each gets a reduced test and a klausscpu-llvm
  fix, coordinated with the session that owns it.
