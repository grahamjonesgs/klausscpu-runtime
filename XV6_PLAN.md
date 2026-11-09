# xv6 on KlaussCPU — plan

**Status: started 2026-11-09, on branch `mmu`.**

Goal: run xv6, MIT's teaching Unix, on the board as the first protected OS
on KlaussCPU. User programs run in user mode, each in its own address space
through the stage 3 TLB, and system calls, interrupts and page faults go
through the stage 1 exception path. xv6 is small enough to debug, and it
exercises everything stages 1–3 added before a Linux port does.

Hardware: the stage 3 release bitstream (klaussCPU `mmu` 52bece8 LEAN,
`CPU_FEATURES` 0x0010_100D). It has exceptions, user mode, LR/SC and a
16-entry TLB with 64 KiB pages, and no MPU. Spec: klaussCPU `MMU_PLAN.md`
§3–§6 and §11. Runtime interface: `exc.h`.

## Source

xv6-riscv at upstream commit `fa2ddcb` (MIT licence; the licence is kept in
`xv6/LICENSE`). It's 64-bit (`uint64`, LP64), which matches our LP64 ABI.
It lives in `xv6/` in this repo. Files that don't depend on the architecture
are kept as close to upstream as possible, so later upstream fixes still
apply. Each change is marked `// klausscpu:`.

## Design

### Address space

| Range | What | How |
|---|---|---|
| useg `0x0000_0000`–`MAXVA` | a process's memory | TLB-mapped through the process's page table |
| kseg `0x8000_0000`–`0x87FF_FFFF` | the kernel: code, data, every physical page | direct, PA = VA − `0x8000_0000`, no TLB |
| `0xF000_0000`+ | MMIO | identity, supervisor only |

- The kernel runs entirely in kseg. Every physical page (`kalloc`) is a
  kseg pointer, so the kernel never needs the TLB for itself. Page tables,
  kernel stacks and the refill handler are all in kseg, as §11.3 requires.
- There's no trampoline and no per-process kernel page table, since kseg is
  visible in every address space. `kvmmake`/`kvminithart` go away.
- `copyin`/`copyout` translate user addresses in software (`walkaddr`, then
  the kseg alias), as xv6 already does. The kernel never touches useg directly.

### Pages and page tables

- `PGSIZE` = 64 KiB, the TLB page size. A minimal process is about 4 pages
  (code, data, guard, stack), or 256 KiB. 128 MiB of DDR holds plenty.
- The page table has **one level**: an array of 8192 64-bit PTEs, exactly
  one 64 KiB page, covering `MAXVA` = 512 MiB of user space. Index = VA >> 16.
- PTE format = `TLB_LO` (PFN [26:16], X [3], W [2], R [1], V [0]) plus
  software bits above [31] (for example U, COW later). The refill handler
  copies it into `TLB_LO` unchanged.
- `walk`, `mappages`, `uvm*` and `freewalk` are rewritten for one level.
  `vmfault` (lazy `sbrk`) keeps its logic.

### TLB refill

- `TLBVEC` points to a dedicated refill handler in kseg (assembly, no stack):
  1. Save two registers in SCRATCH0/1.
  2. Read the VPN from `TLB_HI`, which was auto-loaded on the fault.
  3. Load the PTE from the current process's page table, whose kseg address
     is kept in a kernel global.
  4. If the PTE is valid and VA < `MAXVA`: write `TLB_LO`, pick an index by
     round-robin, `TLBWI`, restore the registers and `IRET`.
  5. Otherwise jump to the general exception path, which handles the page
     fault (`vmfault`) or kills the process.
- Supervisor faults on useg addresses don't happen, because the kernel only
  uses kseg.
- Context switch: `TLBINV` on every switch to a different process (the
  simple first version). ASIDs come in milestone 4.

### Traps

- **One entry for everything.** `EVEC`, and the interrupt vectors for
  `INT_SRC_TIMER`, go to one kseg stub, `trapvec.S`. It saves r0–r15 and
  the user SP (`MFSR usp`) on the kernel stack below the hardware's 64-bit
  frame. That makes a `struct trapframe` on the kernel stack, and
  `p->trapframe` points at it.
- **Leaving.** `usertrapret` restores `USP`, the registers and the frame,
  then `IRET`s. The hardware leaves the kernel SP in `SP_ALT` for the next
  user→kernel entry, so each process's kernel stack is used automatically.
- **Causes.**
  - SYSCALL → `syscall()`. The frame PC is already past the instruction
    (§4), so no `epc += 4` is needed.
  - TLB_MISS / ACCESS from user mode → `vmfault`, or kill.
  - ILLEGAL / TRAP / misaligned from user mode → kill.
  - Any fault from supervisor mode → `panic` with ECAUSE/EADDR/PC.
- **Timer.** The interval timer (`REG_TIMER_PER`, `INT_SRC_TIMER`) drives
  `clockintr` and `yield`. The UART has no RX interrupt, so the timer tick
  also polls the RX FIFO into `consoleintr`.
- **Interrupt enable.** `intr_on`/`intr_off` are EI/DI, and `intr_get`
  reads STATUS.DI. Entry sets DI, as on RISC-V.

### System call ABI

xv6 calls take at most 3 arguments. A user stub is `setr r12, SYS_x;
syscall; ret`: arguments in r0–r3 (where the C ABI already puts them), the
number in r12, the result in r12. `argraw(n)` reads `trapframe->r[n]`.
`usys.pl` emits that stub.

### Context switch

`swtch` saves and restores the callee-saved set: R4–R7, R15 (P) and SP
(klausscpu-llvm `CSR_KlaussCPU`), plus the return address (`ra`). `struct
context` follows.

### CPU, locks, atomics

- `NCPU` = 1. Core 2 has no TLB, and xv6 on one core is simpler.
- Spinlocks stay. `__sync_lock_test_and_set` / `__sync_synchronize` come
  from the kernel's own small implementation (DI/EI is enough on one core).
  User mode doesn't use atomics.

### Console, disk, boot

- **Console:** `uart.c` is rewritten for our polled UART (`REG_UART_*`):
  output is synchronous, input is polled by the timer tick. `consoleintr`
  is unchanged.
- **Disk:** `ramdisk.c` replaces `virtio_disk.c`. `fs.img` (built on the
  host by `mkfs`, `FSSIZE` about 2000 × 1 KiB blocks) is linked into the
  kernel ELF as a section, and the "disk" is that memory. Writes are lost
  on reset, which is fine to start with. SD card later, if wanted.
- **Boot:** the kernel is linked at kseg VMAs with LMA = PA, so the loader
  puts it in DDR. A short boot stub runs physically with TE = 0:
  1. Wire a global identity TLB entry for its own page (§11.2: the next
     fetch after turning TE on is translated).
  2. Set `TLBVEC`/`EVEC` to kseg, turn TE on.
  3. Jump to the kseg alias of `main`.
  4. `main` clears the TLB and doesn't wire anything.
- **Probing:** `main` checks `CPU_FEATURES` and refuses to boot without
  bits 0, 2 and 3.

### Toolchain

klausscpu-llvm `mmu` (`KLAUSSCPU_LLVM_BIN`), `-ffreestanding -nostdlib`, the
MMU mnemonics (`syscall`, `mfsr`, `mtsr`, `tlbwi`, …). The kernel links
compiler-rt builtins if any are needed. `mkfs` is built with the host cc.
User programs use `user.ld` linked at VA 0x0 (page 0 is valid in useg; xv6
keeps its text there).

## Milestones

0. **klausscc input.**
   - `--emulate` gets `--uart-in <file>`, which feeds those bytes to the
     UART RX FIFO, a few per tick, for scripted sessions.
   - With no file, stdin is fed when it's a terminal, for interactive use.
   - Check that `-s -m` on the board forwards keyboard input; add it if not.
1. **Boot on the emulator.** Kernel prints its banner, initialises kalloc,
   traps, timer and ramdisk, runs `initcode` → `init` → `sh`, and the
   shell prints `$ `. Driven by an `--uart-in` script: `echo hi`, `ls`.
2. **Userland on the emulator.**
   - All of `user/` builds and runs, through the shell from a script: `cat
     README`, `wc`, `grep`, `mkdir`, `ln`, `rm`, pipes, `&`, `kill`.
   - `forktest` and `usertests -q` pass, and any failures are explained.
   - Then `tools/validate/xv6.sh` runs the scripted session and checks it.
3. **Board.** The same kernel on the release bitstream: the scripted
   session over the UART, then an interactive shell. Timing (UART baud,
   tick rate) is tuned here.
4. **Polish.**
   - ASIDs instead of `TLBINV` on every switch; a refill fast-path count.
   - `usertests` in full.
   - A `perf` note (fork/exec/syscall costs).
   - Optionally an SD-card-backed disk, and copy-on-write fork.

## Risks

- **Emulator ≠ board.** The emulator has no cycle model or cache timing, so
  races in the kernel may only show on the board. Milestone 3 is where
  those show up.
- **64 KiB pages and the 8 KiB direct-mapped I-cache.** Code written through
  kseg and executed from useg (`exec`) needs `ICACHE_INV`, as `test_tlb` T5
  does. `exec` invalidates after loading.
- **Compiler gaps.** xv6 uses little inline assembly. Anything that needs a
  klausscpu-llvm fix is noted here and raised with the session that owns
  klausscpu-llvm before anyone edits it.
