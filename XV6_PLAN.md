# xv6 on KlaussCPU — plan

**Status (2026-11-09, branch `mmu`): milestones 0–3 done.** xv6 boots, init
starts sh, the user programs run, and `forktest` and `usertests -q` pass on
the emulator and on the board (klaussCPU `mmu` 98fe470, the stage 3
release): `tools/validate/xv6.sh <out> full [emu|board]`. Milestone 4
(polish) is next. See *As built* below for where the port differs from the
design.

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

## As built (milestones 0–2)

- **TLB refill.** `trapvec.S` `tlbrefill` saves the caller-saved registers
  on the stack and calls `tlbrefill_c()` (trap.c). It doesn't use
  SCRATCH0/1 with no stack: a miss from user mode has already switched to
  the process's kernel stack, which is in kseg, and the kernel itself
  never touches useg. `cmprv` sets the flags before the POPs (POP leaves
  them alone); 0 means not mapped, and it jumps to `excvec`. ECAUSE is still
  the TLB_MISS. The refill only loads PTEs with V and PTE_U (bit 4);
  `uvmclear` clears PTE_U for the stack guard page.
- **Trapframe.** `struct trapframe { usp; r[16]; frame; }` (144 bytes) sits
  at the top of the kernel stack (`p->trapframe = kstack + 64K - 144`)
  because every trap from user mode arrives with SP = SP_ALT = the top.
  `trapret(tf)` does SETSP/POP/IRET. A new process's stack holds
  `forkret`'s address just below the trapframe, for `swtch`'s RET.
- **Interrupts.** A taken interrupt doesn't set DI (it clears its
  INT_MASK bit), so `trap()` calls `intr_off()` first. `clockintr()` sets
  INT_MASK again, or the scheduler's `wait` could never wake; IRET
  restores the frame's mask anyway. User frames built by the kernel
  (`trapframe_setpc`) have INT_MASK = timer and PM = 1. Tick: 10 ms
  (1,000,000 cycles). The timer tick polls UART RX.
- **TLB flushing.** `tlbactivate()` (scheduler before `swtch`, `exec` after
  the switch) sets the refill's page table and runs TLBINV. `uvmunmap` and
  `uvmclear` flush too. ASID stays 0.
- **exec.** ELF32 headers (`elf.h`). `r0` = argc and `r1` = argv for
  `ulib.c` `start()`. `CACHE_CTRL.ICACHE_INV` runs after the new image is
  loaded through kseg, as `test_tlb` T5 does. User programs are linked by
  `user.ld` with PHDRS: text R-X at 0 and data RW at the next 64 KiB page.
  `--nmagic` keeps the files small.
- **Boot.** `jmp kentry` was relaxed to a PC-relative `jmprel`, which
  would have stayed at the PA alias. `boot.S` uses `setr` + `jmpr`. BSS,
  including the 16 KiB boot stack, is zeroed in assembly before the stack
  is used.
- **Small things.** `memset`/`memmove` move 8 bytes at a time (64 KiB
  pages). `katomic.c` provides `__sync_lock_test_and_set_{4,8}` (DI/EI),
  which clang calls for `__atomic_exchange_n`. `push_off` uses DI's
  result for the old enable bit.
- **usertests at 64 KiB pages.** `bigargtest` sizes its arguments to
  overflow a 64 KiB stack. `lazy_sbrk` climbs to `MAXVA` (512 MiB) in
  64 MiB steps, and `lazy_alloc`/`lazy_unmap` use a 128 MiB region with
  16 MiB steps (`PGSIZE * PGSIZE` overflows `int`). The `usertrap(): unexpected ecause …`
  lines in `kernmem`, `MAXVAplus`, `stacktest`, `nowrite`, `sbrkbugs`
  and `lazy_unmap` are the faults those tests provoke on purpose. Each
  prints OK.
- **klausscc (milestone 0).**
  - `--uart-in FILE` types a file into the UART RX FIFO, one byte per
    2,000 instructions while the FIFO has room.
  - `--uart-prompt TEXT` types each line only after TEXT (`$ `) is
    printed, and stops the run (`stop = InputDone`) when the prompt comes
    back after the last line.
  - Interactive stdin isn't fed under `--emulate`, because its UART
    output is only printed at the end.
  - On the board, `-s -m` already forwards keystrokes in raw mode (not
    Ctrl+C, which stops the monitor). It reads terminal key events, so
    piped input isn't forwarded yet.
- **Validation.** `tools/validate/xv6.sh <out> quick|full` needs a
  klausscc with `--uart-prompt` (mmu d0035b2+).
  - quick: a scripted session with ordered expected lines, about 160M
    instructions, 2 s.
  - full: adds `usertests -q`, about 12.9G instructions, 2 min.

## Board (milestone 3)

- Loaded with `klausscc -i kernel -s -m` (a 4 MiB `.kbt`, about 15 s) and
  typed with `--uart-in`/`--uart-prompt` (klausscc b94000b).
- **Idle loop.** On 98fe470 the hardware does not take a pending interrupt
  in the one-instruction window of `intr_on(); intr_off()`. EI's enable
  commits at WB, about 3 cycles late; the emulator takes it at once.
  Upstream's idle (`wfi` with interrupts off, relying on that window) never
  serviced the tick that polls the UART, so the shell got no input. The
  scheduler now WAITs with interrupts on (`intr_on(); wait; intr_off()`).
  The CPU session is making EI precise in a later release, as RISC-V does.
- Results: the quick session and `usertests -q` (ALL TESTS PASSED, about
  5 minutes) on the board.

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
  - Milestones 0–2 found no compiler bug.
  - Gotcha: the assembler relaxes `jmp label` to a PC-relative `jmprel`.
    Code that runs at one alias and must jump to another needs `jmpr`.
