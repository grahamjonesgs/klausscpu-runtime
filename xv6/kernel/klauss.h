// klausscpu: KlaussCPU replacement for riscv.h (XV6_PLAN.md).
//
// The kernel runs in kseg (0x8000_0000 + PA, direct, no TLB); user space
// (useg, VA < MAXVA) goes through the software-refilled TLB, one 64 KiB
// page per entry.  A page table is one 64 KiB page of 8192 PTEs indexed by
// VA >> 16; a PTE has TLB_LO's layout (PFN [26:16], X [3], W [2], R [1],
// V [0]) plus PTE_U [4], so the refill copies it into TLB_LO as it is.
// Interface: klaussCPU MMU_PLAN.md §3-§6 and §11, runtime exc.h.

#ifndef __ASSEMBLER__

// ── Pages and page tables ──────────────────────────────────────────────
#define PGSIZE 65536 // bytes per page (the TLB page)
#define PGSHIFT 16   // bits of offset within a page

#define PGROUNDUP(sz)  (((sz) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(a) (((a)) & ~(PGSIZE - 1))

#define PTE_V (1L << 0)
#define PTE_R (1L << 1)
#define PTE_W (1L << 2)
#define PTE_X (1L << 3)
#define PTE_U (1L << 4) // software: the refill only loads PTEs with U set

// The PFN field holds the physical page.  A kalloc() pointer is a kseg
// address (PA + KERNBASE); masking drops KERNBASE, and PTE2PA hands back
// the kseg alias, so xv6's "pa" values stay usable kernel pointers.
#define PA2PTE(pa) (((uint64)(pa)) & 0x07FF0000L)
#define PTE2PA(pte) ((((uint64)(pte)) & 0x07FF0000L) | 0x80000000L)
#define PTE_FLAGS(pte) ((pte) & 0x1F)
#define PTE_TLB_LO(pte) ((pte) & 0x07FF000FL) // what goes into TLB_LO

#define NPTE 8192 // PTEs in a page table: PGSIZE / 8
// one past the highest user virtual address: 8192 pages of 64 KiB
#define MAXVA ((uint64)NPTE * PGSIZE)

typedef uint64 pte_t;
typedef uint64 *pagetable_t; // NPTE PTEs

// ── System registers and instructions ──────────────────────────────────
#define r_sr(SR) __extension__({                                          \
    uint64 _v;                                                            \
    asm volatile("mfsr %0, " #SR : "=r"(_v) : : "memory");                \
    _v; })
#define w_sr(SR, VAL) asm volatile("mtsr " #SR ", %0" : : "r"((uint64)(VAL)) : "memory")

#define STATUS_DI 0x2
#define STATUS_TE 0x200

// The 64-bit frame an exception or interrupt pushes (and IRET pops).
#define FRAME_PC(f) ((f) & 0xFFFFFFFFL)
#define FRAME_PM (1L << 44)        // came from user mode
#define FRAME_INT_MASK_SHIFT 39    // [42:39]

// ECAUSE[7:0]
#define EXC_ILLEGAL 1
#define EXC_PRIVILEGED 2
#define EXC_TRAP 3
#define EXC_SYSCALL 4
#define EXC_MISALIGNED_LOAD 5
#define EXC_MISALIGNED_STORE 6
#define EXC_ACCESS_LOAD 7
#define EXC_ACCESS_STORE 8
#define EXC_ACCESS_FETCH 9
#define EXC_MISALIGNED_FETCH 10
#define EXC_TLB_MISS_LOAD 11
#define EXC_TLB_MISS_STORE 12
#define EXC_TLB_MISS_FETCH 13

static inline void tlbinv(void) { asm volatile("tlbinv" ::: "memory"); }
static inline void tlbwi(void) { asm volatile("tlbwi" ::: "memory"); }

// enable device interrupts
static inline void
intr_on()
{
  __builtin_klausscpu_ei(1); // EI rs1: bit 0 = 1 enables
}

// disable device interrupts
static inline void
intr_off()
{
  (void)__builtin_klausscpu_di();
}

// are device interrupts enabled?
static inline int
intr_get()
{
  return (r_sr(status) & STATUS_DI) == 0;
}

static inline uint64
r_sp()
{
  uint64 x;
  asm volatile("getsp %0" : "=r"(x));
  return x;
}

// ── Devices (runtime mmio.h) ───────────────────────────────────────────
#define MMIO_REG32(a) (*(volatile uint32 *)(uint64)(a))
#define MMIO_REG64(a) (*(volatile uint64 *)(uint64)(a))

#define UART_TX      MMIO_REG32(0xF0010000)
#define UART_RX      MMIO_REG32(0xF0010008)
#define UART_STATUS  MMIO_REG32(0xF0010010)
#define UART_STATUS_TX_BUSY  (1 << 0)
#define UART_STATUS_RX_EMPTY (1 << 1)

#define INT_MASK     MMIO_REG32(0xF00F0000)
#define INT_VEC(n)   MMIO_REG32(0xF00F0010 + 8 * (n))
#define TIMER_PER    MMIO_REG32(0xF00F0030)
#define CPU_FEATURES MMIO_REG32(0xF00F0048)
#define INT_SRC_TIMER 0

#define CACHE_CTRL   MMIO_REG32(0xF0050000)
#define CACHE_CTRL_ICACHE_INV (1 << 3)

#define CPU_FEAT_EXC  0x1
#define CPU_FEAT_TLB  0x4
#define CPU_FEAT_LRSC 0x8
#define CPU_FEAT_TLB_ENTRIES(f) (((f) >> 8) & 0xFF)

#endif // __ASSEMBLER__
