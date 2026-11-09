// klausscpu: saved registers for kernel context switches (swtch.S): SP
// and the callee-saved set of the KlaussCPU ABI, R4-R7 and R15 (P).  The
// return address is on the stack (CALL pushes it), not here.
struct context {
  uint64 sp;
  uint64 r4;
  uint64 r5;
  uint64 r6;
  uint64 r7;
  uint64 r15;
};

// Per-CPU state.
struct cpu {
  struct proc *proc;      // The process running on this cpu, or null.
  struct context context; // swtch() here to enter scheduler().
  int noff;               // Depth of push_off() nesting.
  int intena;             // Were interrupts enabled before push_off()?
};

extern struct cpu cpus[NCPU];

// klausscpu: what trapvec.S leaves on the kernel stack on every trap.  The
// hardware pushes the 64-bit frame (PC, flags, INT_MASK, DI, PM) at SP-8
// (on the process's kernel stack if it came from user mode), then the stub
// pushes R15..R0 and the user SP (SR USP).  A trap from user mode always
// arrives with SP = the top of the kernel stack (SP_ALT), so the user
// trapframe is at a fixed place: p->trapframe = kstack + KSTACKSIZE - 144.
// trapret pops it all and IRETs.
//
// System calls: number in r[12], arguments in r[0]..r[3], result in r[12].
struct trapframe {
  /*   0 */ uint64 usp;    // the user SP
  /*   8 */ uint64 r[16];  // R0..R15
  /* 136 */ uint64 frame;  // hardware frame: PC [31:0], ... PM [44]
};

enum procstate { UNUSED, USED, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  struct spinlock lock;

  // p->lock must be held when using these:
  enum procstate state; // Process state
  void *chan;           // If non-zero, sleeping on chan
  int killed;           // If non-zero, have been killed
  int xstate;           // Exit status to be returned to parent's wait
  int pid;              // Process ID

  // wait_lock must be held when using this:
  struct proc *parent; // Parent process

  // these are private to the process, so p->lock need not be held.
  uint64 kstack;               // klausscpu: kseg address of the kernel stack page
  uint64 sz;                   // Size of process memory (bytes)
  pagetable_t pagetable;       // User page table
  struct trapframe *trapframe; // klausscpu: at the top of kstack
  struct context context;      // swtch() here to run process
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)
};
