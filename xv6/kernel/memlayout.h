// klausscpu: physical memory layout and address map (XV6_PLAN.md).
//
// The loader puts the kernel image in DDR at PA 0x20 (the first 32 bytes
// are the hardware's heap header); the kernel is linked at its kseg alias,
// KERNBASE + PA, and runs there with translation on.
//
//   0x0000_0000 - MAXVA       useg: a process's memory, through the TLB
//   0x8000_0000 - 0x87FF_FFFF kseg: every physical byte, PA = VA - KERNBASE
//   0xF000_0000 -             MMIO (supervisor only)
//
// The kernel never touches useg: copyin/copyout translate in software.
// There is no trampoline and no trapframe page: the trapframe sits at the
// top of each process's kernel stack (proc.h).

#define KERNBASE 0x80000000L
#define PHYSTOP  (KERNBASE + 128 * 1024 * 1024)

// the per-process kernel stack: one page, from kalloc()
#define KSTACKSIZE PGSIZE
