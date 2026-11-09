/*
 * exc.h — exceptions, user mode, system registers and LR/SC (MMU stage 1).
 *
 * Interface: klaussCPU MMU_PLAN.md §3–§6 (frozen at b6fa71c on branch `mmu`)
 * and ISA_ENCODING_V2_MAP.md §9.  None of this exists on a core whose
 * CPU_FEATURES bit 0 is clear (every bitstream before `mmu`): there the new
 * opcodes are illegal and park the core.  exc_init() probes first.
 *
 * Until klausscpu-llvm's MC layer knows SYSCALL/MFSR/MTSR/LR/SC, the
 * instructions are emitted as `.long` words with fixed registers (register
 * variables pin the operands).  Encodings:
 *   SYSCALL           0x6C080000
 *   MFSR rd, sr       0x6C400000 | sr << 12 | rd << 8
 *   MTSR sr, rs1      0x6C600000 | sr << 12 | rs1 << 4
 *   LR.D rd, [rs1]    0x5B001000 | rd << 8 | rs1 << 4     (LR.W 0x5A.., LR.W.S 0x5A80..)
 *   SC.D rd, rs2,[rs1] 0x5F001000 | rd << 8 | rs1 << 4 | rs2   (SC.W 0x5E..)
 *                     rd = 0 success / 1 failure; data is rs2
 */
#ifndef KLAUSS_EXC_H
#define KLAUSS_EXC_H

#include <stdint.h>

/* ── Feature probe (MMIO; reads 0 on older bitstreams) ─────────────────── */
#define CPU_FEATURES_ADDR   0xF00F0048u
#define CPU_FEAT_EXC        0x1u    /* exceptions, user mode, SYSCALL, MFSR/MTSR, TP, CYCLE */
#define CPU_FEAT_MPU        0x2u
#define CPU_FEAT_TLB        0x4u
#define CPU_FEAT_LRSC       0x8u

/* Set by exc_init() (0 until then).  atomic.c references it weakly. */
extern uint32_t __klauss_cpu_features;

/* ── System registers (plain integers: they are pasted into .long) ──────── */
#define SR_STATUS    0   /* user-readable: [0] UM, [1] DI */
#define SR_EVEC      1
#define SR_ECAUSE    2
#define SR_EADDR     3
#define SR_USP       4   /* the user SP while in supervisor mode */
#define SR_SCRATCH0  5
#define SR_SCRATCH1  6
#define SR_TP        7   /* user read/write */
#define SR_CYCLE     8   /* read-only, user-readable */

#define STATUS_UM     0x1u
#define STATUS_DI     0x2u
#define STATUS_MPU_EN 0x100u   /* stage 2 */
#define STATUS_TE     0x200u   /* stage 3 */

/* ── Stage 2: MPU (MMU_PLAN.md §10) ────────────────────────────────────── */
#define SR_MPU_BASE(n)  (0x10 + 2 * (n))   /* [31:5] base granule, attrs below */
#define SR_MPU_LIMIT(n) (0x11 + 2 * (n))   /* [31:5] LAST granule (inclusive) */
#define MPU_EN      0x01u
#define MPU_R       0x02u
#define MPU_W       0x04u
#define MPU_X       0x08u
#define MPU_GUARD   0x10u                  /* deny region, either mode */
#define MPU_GRANULE 32u
#define CPU_FEAT_MPU_REGIONS(f) ((((f) >> 4) & 0xFu) + 1u)

/* Program region n: [base, last] (both inclusive, 32-byte granules), attrs
 * = MPU_EN | MPU_R/W/X or MPU_EN | MPU_GUARD.  0 attrs disables it. */
void exc_mpu_set(unsigned n, uint32_t base, uint32_t last, uint32_t attrs);
/* Disable all regions and clear STATUS.MPU_EN. */
void exc_mpu_reset(void);
/* Set or clear STATUS.MPU_EN (read-modify-write of STATUS). */
void exc_mpu_enable(int on);

/* ── Stage 3: TLB and paging (MMU_PLAN.md §11) ────────────────────────── */
#define SR_TLB_HI     0x30   /* [31:16] VPN, [8] G, [7:0] ASID (auto-loaded on a TLB fault) */
#define SR_TLB_LO     0x31   /* [26:16] PFN, [3] X, [2] W, [1] R, [0] V */
#define SR_TLB_INDEX  0x32   /* index (entries from CPU_FEATURES[15:8]), [31] P = the last TLBP missed */
#define SR_ASID       0x33
#define SR_TLBVEC     0x34   /* TLB-miss vector, 0 = EVEC */
#define TLB_HI_G      0x100u
#define TLB_LO_V      0x1u
#define TLB_LO_R      0x2u
#define TLB_LO_W      0x4u
#define TLB_LO_X      0x8u
#define TLB_INDEX_P   0x80000000u
#define TLB_PAGE      0x10000u      /* 64 KiB pages */
#define KSEG_BASE     0x80000000u   /* kseg: PA = VA - KSEG_BASE (TE = 1) */
#define KMAP_BASE     0xC0000000u
#define CPU_FEAT_TLB_ENTRIES(f) (((f) >> 8) & 0xFFu)

static inline void klauss_tlbwi(void)  { __asm__ volatile(".long 0x6C090000" ::: "memory"); }
static inline void klauss_tlbr(void)   { __asm__ volatile(".long 0x6C0A0000" ::: "memory"); }
static inline void klauss_tlbp(void)   { __asm__ volatile(".long 0x6C0B0000" ::: "memory"); }
static inline void klauss_tlbinv(void) { __asm__ volatile(".long 0x6C0C0000" ::: "memory"); }

/* ── Causes (ECAUSE[7:0]) ──────────────────────────────────────────────── */
/* ECAUSE[8] (stage 2): the ACCESS_* fault was a denial by a GUARD region.
 * The handler's `cause` argument is the raw ECAUSE; use EXC_CAUSE(). */
#define ECAUSE_GUARD   0x100u
#define EXC_CAUSE(c)   ((c) & 0xFFu)
enum exc_cause {
    EXC_ILLEGAL          = 1,   /* EADDR = instruction word 0 */
    EXC_PRIVILEGED       = 2,   /* EADDR = instruction word 0 */
    EXC_TRAP             = 3,   /* EADDR = instruction word 0 */
    EXC_SYSCALL          = 4,   /* frame PC = the NEXT instruction */
    EXC_MISALIGNED_LOAD  = 5,   /* EADDR = effective address */
    EXC_MISALIGNED_STORE = 6,
    EXC_ACCESS_LOAD      = 7,
    EXC_ACCESS_STORE     = 8,
    EXC_ACCESS_FETCH     = 9,
    EXC_MISALIGNED_FETCH = 10,
    EXC_TLB_MISS_LOAD    = 11,  /* stage 3: EADDR = VA, TLB_HI auto-loaded */
    EXC_TLB_MISS_STORE   = 12,
    EXC_TLB_MISS_FETCH   = 13,
};

/* ── The 64-bit frame (pushed by entry, popped by IRET) ─────────────────── */
#define FRAME_PC_MASK       0xFFFFFFFFull
#define FRAME_PM            (1ull << 44)   /* previous mode: 1 = user */
#define FRAME_DI            (1ull << 43)   /* interrupts disabled */
#define FRAME_INT_MASK_SHIFT 39            /* [42:39] */

/* What exc_vector leaves on the stack and hands to the C handler.
 * r[n] = Rn at the time of the exception; writes are restored by the
 * vector's POPs.  `frame` is the hardware frame: write its PC to resume
 * elsewhere (e.g. exc_skip()).  In user mode SP is not here: it is SR USP. */
struct exc_regs {
    uint64_t r[16];
    uint64_t frame;
};

/* GUARD faults (MPU_PLAN §10.2): the vector touches no memory until it has
 * read ECAUSE.  On a GUARD denial the faulting SP may lie inside a guard
 * region, so it clears STATUS.MPU_EN (the frame, pushed into the guard
 * unchecked, is then readable), moves to an emergency stack, and calls the
 * handler there with a copy of the frame; the (possibly edited) frame is
 * written back before IRET.  The MPU stays disabled: the handler re-enables
 * it with exc_mpu_enable(1) when it has fixed the cause.
 *
 * Handler: return nonzero if handled (exc_vector then IRETs with the
 * possibly edited registers and frame); 0 to fall into the default report,
 * which prints the state and halts. */
typedef int (*exc_handler_t)(struct exc_regs *regs, uint32_t cause, uint32_t eaddr);

/* Probe CPU_FEATURES, record it in __klauss_cpu_features, and (if the core
 * has stage 1) point EVEC at exc_vector with `h` as the C handler.
 * Returns the feature bits; 0 means no exception support (nothing set). */
uint32_t exc_init(exc_handler_t h);

/* Turn exceptions back off (EVEC = 0: faults park the core as before). */
void exc_disable(void);

/* Length in bytes of the instruction at `pc` (LEN field of word 0). */
static inline uint32_t exc_insn_len(uint32_t pc) {
    uint32_t w = *(volatile uint32_t *)(uintptr_t)pc;
    uint32_t len = w >> 30;
    return (len ? len : 1) * 4u;
}

/* Resume after the faulting instruction (not for SYSCALL: its frame PC is
 * already the next one). */
static inline void exc_skip(struct exc_regs *regs) {
    uint32_t pc = (uint32_t)(regs->frame & FRAME_PC_MASK);
    regs->frame = (regs->frame & ~FRAME_PC_MASK) | (uint32_t)(pc + exc_insn_len(pc));
}

/* Run fn(arg) in user mode on the stack ending at ustack_top (8-aligned).
 * Returns when the user code calls exc_user_exit()-via-syscall or returns
 * from fn (its return value is the result).  The handler passed to exc_init
 * does not see the exit syscall.  Interrupts are enabled in user mode and
 * restored to the caller's state on return. */
uint64_t exc_run_user(uint64_t (*fn)(uint64_t), uint64_t arg, void *ustack_top);

/* System call numbers used by exc.c itself (r0 = number, r1.. = args,
 * result in r12).  Anything else goes to the exc_init handler. */
#define SYS_EXIT_USER   0x7FFF0000u

/* ── Instruction wrappers ──────────────────────────────────────────────── */
#define KX_STR2(x) #x
#define KX_STR(x)  KX_STR2(x)

/* rd = r12 */
#define exc_mfsr(SR) __extension__({                                          \
    register uint64_t _kx_v __asm__("r12");                                   \
    __asm__ volatile(".long 0x6C400C00 + ((" KX_STR(SR) ") << 12)"            \
                     : "=r"(_kx_v) : : "memory");                             \
    _kx_v; })

/* rs1 = r1 */
#define exc_mtsr(SR, VAL) do {                                                \
    register uint64_t _kx_x __asm__("r1") = (uint64_t)(VAL);                  \
    __asm__ volatile(".long 0x6C600010 + ((" KX_STR(SR) ") << 12)"            \
                     : : "r"(_kx_x) : "memory");                              \
} while (0)

/* SYSCALL with r0 = nr, r1..r3 = args; result in r12. */
static inline uint64_t exc_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    register uint64_t r0 __asm__("r0") = nr;
    register uint64_t r1 __asm__("r1") = a1;
    register uint64_t r2 __asm__("r2") = a2;
    register uint64_t r3 __asm__("r3") = a3;
    register uint64_t r12 __asm__("r12");
    __asm__ volatile(".long 0x6C080000"
                     : "=r"(r12), "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
                     : : "memory", "r8", "r9", "r10", "r11", "r13", "r14");
    return r12;
}

/* LR / SC (address in r1, data in r2, result in r12).  SC returns 0 on
 * success, 1 on failure. */
static inline uint64_t klauss_lr_d(volatile void *p) {
    register uintptr_t a __asm__("r1") = (uintptr_t)p;
    register uint64_t v __asm__("r12");
    __asm__ volatile(".long 0x5B001C10" : "=r"(v) : "r"(a) : "memory");
    return v;
}
static inline uint64_t klauss_lr_w(volatile void *p) {   /* zero-extends */
    register uintptr_t a __asm__("r1") = (uintptr_t)p;
    register uint64_t v __asm__("r12");
    __asm__ volatile(".long 0x5A001C10" : "=r"(v) : "r"(a) : "memory");
    return v;
}
static inline uint64_t klauss_sc_d(volatile void *p, uint64_t val) {
    register uintptr_t a __asm__("r1") = (uintptr_t)p;
    register uint64_t d __asm__("r2") = val;
    register uint64_t ok __asm__("r12");
    __asm__ volatile(".long 0x5F001C12" : "=r"(ok) : "r"(a), "r"(d) : "memory");
    return ok;
}
static inline uint64_t klauss_sc_w(volatile void *p, uint64_t val) {
    register uintptr_t a __asm__("r1") = (uintptr_t)p;
    register uint64_t d __asm__("r2") = val;
    register uint64_t ok __asm__("r12");
    __asm__ volatile(".long 0x5E001C12" : "=r"(ok) : "r"(a), "r"(d) : "memory");
    return ok;
}

#endif /* KLAUSS_EXC_H */
