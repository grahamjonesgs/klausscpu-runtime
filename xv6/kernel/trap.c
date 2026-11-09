#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "klauss.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

// klausscpu: traps for KlaussCPU (XV6_PLAN.md).  Every exception, the
// timer interrupt and TLB misses that the refill can't satisfy arrive in
// trap() through trapvec.S, with the registers saved in a struct trapframe
// on the current kernel stack.

struct spinlock tickslock;
uint ticks;

// in trapvec.S
void excvec(void);
void timervec(void);
void tlbrefill(void);
void trapret(struct trapframe *tf) __attribute__((noreturn));

// The page table the TLB refill reads (tlbactivate()), and the next TLB
// entry it replaces (round-robin over all of them).
pagetable_t cur_pagetable;
static uint tlb_next, tlb_mask;

// timer period in clock cycles: 10 ms at 100 MHz
#define TICK_CYCLES 1000000

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up the vectors, the TLB and the timer.
void
trapinithart(void)
{
  uint n = CPU_FEAT_TLB_ENTRIES(CPU_FEATURES); // a power of 2 (16 on the A7)

  tlb_mask = n - 1;
  tlb_next = 0;
  tlbinv(); // drops boot.S's identity entry: the kernel only uses kseg
  w_sr(asid, 0);
  w_sr(tlbvec, (uint64)tlbrefill);
  w_sr(evec, (uint64)excvec);
  INT_VEC(INT_SRC_TIMER) = (uint32)(uint64)timervec;
  TIMER_PER = TICK_CYCLES;
  INT_MASK = 1 << INT_SRC_TIMER;
}

// Make pagetable the one TLB refills use, and forget the old one's entries.
void
tlbactivate(pagetable_t pagetable)
{
  cur_pagetable = pagetable;
  tlbinv();
}

// Forget every TLB entry (after a mapping of the current process changes).
void
tlbflush(void)
{
  tlbinv();
}

// Called by tlbrefill (trapvec.S) on a TLB miss, with TLB_HI auto-loaded
// ({VPN, G = 0, ASID}).  Loads the entry and returns 1, or returns 0 when
// the page isn't a valid user page: the miss then goes to trap().
int
tlbrefill_c(void)
{
  uint64 va = r_sr(tlb_hi) & 0xFFFF0000L;
  pagetable_t pt = cur_pagetable;

  if (pt == 0 || va >= MAXVA)
    return 0;
  pte_t pte = pt[va >> PGSHIFT];
  if ((pte & (PTE_V | PTE_U)) != (PTE_V | PTE_U))
    return 0;
  w_sr(tlb_lo, PTE_TLB_LO(pte));
  w_sr(tlb_index, tlb_next);
  tlb_next = (tlb_next + 1) & tlb_mask;
  tlbwi();
  return 1;
}

// Set a user trapframe's frame so that trapret enters user mode at pc,
// with the timer interrupt enabled.
void
trapframe_setpc(struct trapframe *tf, uint64 pc)
{
  tf->frame = (pc & 0xFFFFFFFFL) | ((uint64)(1 << INT_SRC_TIMER) << FRAME_INT_MASK_SHIFT) |
              FRAME_PM;
}

static int devintr(uint64 kind);
static void usertrap(struct trapframe *tf, uint64 kind);
static void kerneltrap(struct trapframe *tf, uint64 kind);

// every trap, from trapvec.S.
void
trap(struct trapframe *tf, uint64 kind)
{
  // an interrupt doesn't disable interrupts (it clears its INT_MASK bit);
  // run the handlers with them off, as RISC-V xv6 does.  IRET restores.
  intr_off();
  if (tf->frame & FRAME_PM)
    usertrap(tf, kind);
  else
    kerneltrap(tf, kind);
}

//
// handle an interrupt, exception, or system call from user space.
//
static void
usertrap(struct trapframe *tf, uint64 kind)
{
  int which_dev = 0;
  struct proc *p = myproc();

  if (tf != p->trapframe)
    panic("usertrap: trapframe");

  if (kind != 0) {
    which_dev = devintr(kind);
  } else {
    uint64 cause = r_sr(ecause) & 0xFF;
    uint64 eaddr = r_sr(eaddr);

    if (cause == EXC_SYSCALL) {
      // system call: the frame PC is already past the SYSCALL.
      if (killed(p))
        kexit(-1);

      // the cause and address have been read; interrupts may come now.
      intr_on();

      syscall();
    } else if ((cause == EXC_TLB_MISS_LOAD || cause == EXC_TLB_MISS_STORE) &&
               vmfault(p->pagetable, p->sz, eaddr,
                       (cause == EXC_TLB_MISS_LOAD) ? 1 : 0) != 0) {
      // page fault on lazily-allocated page
    } else {
      printk("usertrap(): unexpected ecause %ld pid=%d\n", cause, p->pid);
      printk("            pc=0x%lx eaddr=0x%lx\n", FRAME_PC(tf->frame), eaddr);
      setkilled(p);
    }
  }

  if (killed(p))
    kexit(-1);

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space with the current process's trapframe.
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // trapret pops the trapframe at the top of the kernel stack; the IRET
  // leaves SP_ALT there for the next trap from user mode.
  intr_off();
  trapret(p->trapframe);
}

// interrupts and exceptions from kernel code go here, on whatever the
// current kernel stack is.
static void
kerneltrap(struct trapframe *tf, uint64 kind)
{
  int which_dev = devintr(kind);

  if (which_dev == 0) {
    // an exception in the kernel
    printk("ecause=%ld pc=0x%lx eaddr=0x%lx\n", r_sr(ecause) & 0xFF,
           FRAME_PC(tf->frame), r_sr(eaddr));
    panic("kerneltrap");
  }

  // give up the CPU if this is a timer interrupt.
  if (which_dev == 2 && myproc() != 0)
    yield();
}

void
clockintr()
{
  if (cpuid() == 0) {
    acquire(&tickslock);
    ticks++;
    wakeup(&ticks);
    release(&tickslock);
  }

  // taking the interrupt cleared its INT_MASK bit; set it again so that the
  // scheduler's WAIT can wake (IRET restores the frame's mask anyway).
  INT_MASK = 1 << INT_SRC_TIMER;

  // the UART has no receive interrupt: poll it every tick.
  uartintr();
}

// returns 2 if timer interrupt, 0 if not an interrupt.
static int
devintr(uint64 kind)
{
  if (kind == 1) {
    clockintr();
    return 2;
  }
  return 0;
}
