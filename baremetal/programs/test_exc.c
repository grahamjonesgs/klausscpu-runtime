// test_exc — MMU stage 1: exceptions, user mode, SYSCALL, system registers,
// LR/SC (klaussCPU MMU_PLAN.md §3–§6, branch `mmu`; runtime: exc.h).
//
// On a core without CPU_FEATURES bit 0 it prints "SKIP" and passes, so it is
// safe on every bitstream.
//
//  S*  supervisor mode: SR round trips, CYCLE, TRAP / ILLEGAL / SYSCALL /
//      misaligned load and store / LR-SC faults, with cause, EADDR, frame PC
//      and the DI bit checked, then resumed.
//  U*  user mode (exc_run_user): STATUS.UM, TP, CYCLE, SYSCALL, the
//      privileged instructions and SRs, MMIO loads and stores, TRAP, a PUSH
//      through an MMIO stack pointer (restartable: SP unchanged, retried),
//      the kernel/user stack split, a timer interrupt taken in user mode,
//      and C atomics in user mode (LR/SC) racing a timer ISR's DI/EI atomics.
//  L*  LR/SC directly: success, failure after an SC, after an exception
//      entry, and on another doubleword.
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

#include "../../exc.h"
#include "../../mmio.h"

static int pass, fail;
static void check(const char *name, int ok) {
    printf("%-52s %s\n", name, ok ? "PASS" : "FAIL");
    if (ok) pass++; else fail++;
}

// ── What the handler saw ─────────────────────────────────────────────────
static volatile uint32_t h_count, h_cause, h_eaddr, h_pc, h_status;
static volatile uint64_t h_frame, h_usp, h_regs_at;

// Fix-ups the handler applies to specific faults.
static volatile uint32_t fix_push_bad_sp;   // U: ACCESS_STORE at this SP - 8
static volatile uint64_t fix_push_good_sp;  //    -> set USP to this and retry

#define SYS_ADD   1u      // r12 = r1 + r2 + r3
#define SYS_NOP   2u

static int handler(struct exc_regs *r, uint32_t cause, uint32_t eaddr) {
    h_count++;
    h_cause = cause;
    h_eaddr = eaddr;
    h_frame = r->frame;
    h_pc = (uint32_t)(r->frame & FRAME_PC_MASK);
    h_status = (uint32_t)exc_mfsr(SR_STATUS);
    h_regs_at = (uintptr_t)r;
    h_usp = (r->frame & FRAME_PM) ? exc_mfsr(SR_USP) : 0;

    switch (cause) {
    case EXC_SYSCALL:
        if (r->r[0] == SYS_ADD) r->r[12] = r->r[1] + r->r[2] + r->r[3];
        else if (r->r[0] == SYS_NOP) r->r[12] = 0;
        else return 0;
        return 1;                       // frame PC is already the next one
    case EXC_ACCESS_STORE:
        if (fix_push_bad_sp && eaddr == fix_push_bad_sp - 8) {
            exc_mtsr(SR_USP, fix_push_good_sp);   // retry on a real stack
            fix_push_bad_sp = 0;
            return 1;                   // re-execute the PUSH
        }
        exc_skip(r);
        return 1;
    default:
        exc_skip(r);                    // resume after the faulting instruction
        return 1;
    }
}

// ── Fault sites (labels give the expected frame PC) ──────────────────────
extern char site_trap[], site_illegal[], site_syscall_next[];
extern char site_user_trap[];
__asm__(
    "  .text\n"
    "  .globl do_trap\n"
    "do_trap:\n"
    "  .globl site_trap\n"
    "site_trap:\n"
    "  trap\n"
    "  ret\n"
    "  .globl do_illegal\n"
    "do_illegal:\n"
    "  .globl site_illegal\n"
    "site_illegal:\n"
    "  .long 0x00000000\n"           // LEN = 00: illegal
    "  ret\n"
    "  .globl do_syscall_nop\n"
    "do_syscall_nop:\n"
    "  setr r0, 2\n"                 // SYS_NOP
    "  .long 0x6C080000\n"           // SYSCALL
    "  .globl site_syscall_next\n"
    "site_syscall_next:\n"
    "  ret\n"
    "  .globl do_user_trap\n"
    "do_user_trap:\n"
    "  .globl site_user_trap\n"
    "site_user_trap:\n"
    "  trap\n"
    "  setr r12, 7\n"
    "  ret\n"
    // user_push_fault(bad_sp): SP = bad_sp; PUSH the old SP (faults, the
    // handler moves USP to a real stack, the PUSH is retried); POP it back;
    // return it.  r12 = the old SP if the retry pushed the right value.
    "  .globl user_push_fault\n"
    "user_push_fault:\n"
    "  getsp r1\n"
    "  setsp r0\n"
    "  push r1\n"
    "  pop r2\n"
    "  setsp r1\n"
    "  copy r12, r2\n"
    "  ret\n");
extern void do_trap(void), do_illegal(void), do_syscall_nop(void);
extern void do_user_trap(void);
extern uint64_t user_push_fault(uint64_t bad_sp);

static void expect(const char *name, uint32_t cause, uint32_t eaddr, uint32_t pc,
                   int user) {
    char buf[96];
    int ok = h_count == 1 && h_cause == cause && h_eaddr == eaddr && h_pc == pc &&
             (!!(h_frame & FRAME_PM)) == user &&
             (h_status & STATUS_UM) == 0 &&      // the handler runs in supervisor
             (h_status & STATUS_DI) != 0;        // with interrupts disabled
    if (!ok)
        printf("  got count=%u cause=%u eaddr=%08x pc=%08x frame=%016llx status=%x\n",
               h_count, h_cause, h_eaddr, h_pc, (unsigned long long)h_frame, h_status);
    snprintf(buf, sizeof buf, "%s", name);
    check(buf, ok);
    h_count = 0;
}

// ── User-mode code: runs with UM = 1; must not touch MMIO (no printf) ────
static uint64_t ustack[2048] __attribute__((aligned(16)));
#define USTACK_TOP (&ustack[2048])
static uint64_t ustack2[256] __attribute__((aligned(16)));

static uint64_t u_double(uint64_t x) { return x * 2; }
static uint64_t u_status(uint64_t x) { (void)x; return exc_mfsr(SR_STATUS); }
static uint64_t u_tp(uint64_t x) {
    exc_mtsr(SR_TP, x);
    return exc_mfsr(SR_TP);
}
static uint64_t u_cycle(uint64_t x) {
    (void)x;
    uint64_t a = exc_mfsr(SR_CYCLE);
    for (volatile int i = 0; i < 100; i++) {}
    uint64_t b = exc_mfsr(SR_CYCLE);
    return b > a;
}
static uint64_t u_syscall(uint64_t x) { return exc_syscall(SYS_ADD, x, 20, 300); }
static uint64_t u_di(uint64_t x) { (void)x; return __builtin_klausscpu_di() + 100; }
static uint64_t u_ei(uint64_t x) { (void)x; __builtin_klausscpu_ei(1); return 5; }
static uint64_t u_mfsr_ecause(uint64_t x) { (void)x; return exc_mfsr(SR_ECAUSE); }
static uint64_t u_mtsr_evec(uint64_t x) { (void)x; exc_mtsr(SR_EVEC, 0); return 6; }
static uint64_t u_mfsr_usp(uint64_t x) { (void)x; return exc_mfsr(SR_USP); }
static uint64_t u_mmio_load(uint64_t x) { (void)x; return REG_INT_MASK + 1000; }
static uint64_t u_mmio_store(uint64_t x) { (void)x; REG_LEDS = 0x5; return 9; }
static uint64_t u_halt(uint64_t x) { (void)x; __asm__ volatile("halt"); return 11; }
static uint64_t u_trap(uint64_t x) { (void)x; do_user_trap(); return 12; }
static uint64_t u_push(uint64_t x) { return user_push_fault(x); }
static uint64_t u_sp(uint64_t x) {
    (void)x;
    uint64_t sp;
    __asm__ volatile("getsp %0" : "=r"(sp));
    return sp;
}

// Interrupt in user mode: spin until the timer ISR has run N times.
static volatile uint32_t ticks;
static volatile uint64_t isr_frame, isr_sp;
static uint64_t u_wait_ticks(uint64_t n) {
    uint32_t t0 = ticks;
    for (uint32_t i = 0; i < 5000000 && ticks - t0 < n; i++) {}
    return ticks - t0;
}

// C atomics in user mode: __sync libcalls -> the LR/SC path in atomic.c.
static _Atomic uint64_t a64;
static _Atomic uint32_t a32;
static _Atomic uint16_t a16[2];
static _Atomic uint8_t a8[4];
static volatile uint32_t isr_adds;
#define RACE_N 20000
static uint64_t u_atomics(uint64_t x) {
    (void)x;
    uint64_t bad = 0;
    for (uint32_t i = 0; i < RACE_N; i++) {
        atomic_fetch_add(&a64, 1);
        atomic_fetch_add(&a32, 1);
        atomic_fetch_add(&a16[1], 1);
        atomic_fetch_add(&a8[2], 1);
    }
    uint32_t e = 5;
    if (!atomic_compare_exchange_strong(&a8[1], (uint8_t *)&(uint8_t){0}, 9)) bad |= 1;
    if (atomic_compare_exchange_strong(&a8[1], (uint8_t *)&(uint8_t){0}, 3)) bad |= 2;
    if (atomic_exchange(&a16[0], 0x1234) != 0) bad |= 4;
    (void)e;
    return bad;
}

void timer_isr_c(uint64_t *sp_at) {
    ticks++;
    isr_sp = (uintptr_t)sp_at;
    isr_frame = sp_at[11];                 // above the 11 pushed registers
    atomic_fetch_add(&a64, 1);              // supervisor: DI/EI path
    atomic_fetch_add(&a32, 1);
    atomic_fetch_add(&a16[1], 1);
    atomic_fetch_add(&a8[2], 1);
    isr_adds++;
}
__asm__(
    "  .text\n  .p2align 3\n  .globl exc_timer_isr\n"
    "exc_timer_isr:\n"
    "  push r0\n  push r1\n  push r2\n  push r3\n  push r8\n  push r9\n"
    "  push r10\n  push r11\n  push r12\n  push r13\n  push r14\n"
    "  getsp r0\n"
    "  call timer_isr_c\n"
    "  pop r14\n  pop r13\n  pop r12\n  pop r11\n  pop r10\n  pop r9\n"
    "  pop r8\n  pop r3\n  pop r2\n  pop r1\n  pop r0\n"
    "  iret\n");
extern void exc_timer_isr(void);

static uint64_t run_user(uint64_t (*fn)(uint64_t), uint64_t arg) {
    return exc_run_user(fn, arg, USTACK_TOP);
}

static int in_ustack(uint64_t a) {
    return a >= (uintptr_t)ustack && a <= (uintptr_t)USTACK_TOP;
}

int main(void) {
    printf("\n=== test_exc: MMU stage 1 ===\n");
    uint32_t feat = exc_init(handler);
    printf("CPU_FEATURES = %08x\n", (unsigned)*(volatile uint32_t *)CPU_FEATURES_ADDR);
    if (!(feat & CPU_FEAT_EXC)) {
        printf("SKIP: no MMU stage 1 on this core\n");
        printf("test_exc PASSED\n");
        return 0;
    }
    check("F0 CPU_FEATURES: exceptions and LR/SC",
          (feat & (CPU_FEAT_EXC | CPU_FEAT_LRSC)) == (CPU_FEAT_EXC | CPU_FEAT_LRSC));

    // ── S: supervisor ────────────────────────────────────────────────────
    uint64_t st = exc_mfsr(SR_STATUS);
    check("S1 STATUS: supervisor, interrupts enabled", (st & 3) == 0);
    unsigned k = __builtin_klausscpu_di();
    st = exc_mfsr(SR_STATUS);
    __builtin_klausscpu_ei(k);
    check("S1 STATUS.DI follows DI", (st & STATUS_DI) != 0);

    exc_mtsr(SR_SCRATCH0, 0x0123456789ABCDEFull);
    exc_mtsr(SR_SCRATCH1, 0xFEDCBA9876543210ull);
    exc_mtsr(SR_TP, 0x1122334455667788ull);
    check("S2 SCRATCH0/1, TP keep 64 bits",
          exc_mfsr(SR_SCRATCH0) == 0x0123456789ABCDEFull &&
          exc_mfsr(SR_SCRATCH1) == 0xFEDCBA9876543210ull &&
          exc_mfsr(SR_TP) == 0x1122334455667788ull);
    extern void exc_vector(void);
    check("S2 EVEC reads back exc_vector", exc_mfsr(SR_EVEC) == (uintptr_t)exc_vector);
    exc_mtsr(SR_USP, 0x1000);
    check("S2 USP round trip", exc_mfsr(SR_USP) == 0x1000);

    uint64_t c0 = exc_mfsr(SR_CYCLE);
    for (volatile int i = 0; i < 1000; i++) {}
    uint64_t c1 = exc_mfsr(SR_CYCLE);
    check("S3 CYCLE counts up", c1 > c0);

    h_count = 0;
    do_trap();
    expect("S4 TRAP: cause, EADDR = word, PC = the TRAP", EXC_TRAP, 0x6C040000u,
           (uint32_t)(uintptr_t)site_trap, 0);
    do_illegal();
    expect("S5 ILLEGAL: cause, EADDR = word, PC", EXC_ILLEGAL, 0,
           (uint32_t)(uintptr_t)site_illegal, 0);
    do_syscall_nop();
    expect("S6 SYSCALL: frame PC = next instruction", EXC_SYSCALL, 0,
           (uint32_t)(uintptr_t)site_syscall_next, 0);
    check("S6 SYSCALL returns the handler's r12",
          exc_syscall(SYS_ADD, 1, 2, 3) == 6 && h_count == 1);
    h_count = 0;

    static uint8_t buf[16] __attribute__((aligned(8)));
    volatile uint32_t *mis = (volatile uint32_t *)(buf + 2);
    uint32_t v = *mis;
    (void)v;
    check("S7 misaligned 32-bit load: MISALIGNED_LOAD, EADDR",
          h_count == 1 && h_cause == EXC_MISALIGNED_LOAD &&
          h_eaddr == (uint32_t)(uintptr_t)(buf + 2));
    h_count = 0;
    *mis = 0xAABBCCDD;
    check("S7 misaligned 32-bit store: MISALIGNED_STORE, nothing written",
          h_count == 1 && h_cause == EXC_MISALIGNED_STORE &&
          h_eaddr == (uint32_t)(uintptr_t)(buf + 2) && buf[2] == 0 && buf[5] == 0);
    h_count = 0;

    (void)klauss_lr_d(buf + 4);
    check("S8 LR.D not 8-aligned: MISALIGNED_LOAD",
          h_count == 1 && h_cause == EXC_MISALIGNED_LOAD &&
          h_eaddr == (uint32_t)(uintptr_t)(buf + 4));
    h_count = 0;
    (void)klauss_lr_w((void *)(uintptr_t)0xF00F0000u);
    check("S8 LR.W to MMIO in supervisor: ACCESS_LOAD",
          h_count == 1 && h_cause == EXC_ACCESS_LOAD && h_eaddr == 0xF00F0000u);
    h_count = 0;
    (void)klauss_sc_w((void *)(uintptr_t)0xF00F0000u, 1);
    check("S8 SC.W to MMIO in supervisor: ACCESS_STORE",
          h_count == 1 && h_cause == EXC_ACCESS_STORE);
    h_count = 0;

    // ── L: LR/SC ─────────────────────────────────────────────────────────
    static uint64_t cell[2] __attribute__((aligned(8)));
    cell[0] = 10;
    uint64_t lv = klauss_lr_d(&cell[0]);
    uint64_t r1 = klauss_sc_d(&cell[0], lv + 1);
    uint64_t r2 = klauss_sc_d(&cell[0], 99);
    check("L1 LR.D + SC.D succeeds (0), stores", lv == 10 && r1 == 0 && cell[0] == 11);
    check("L1 a second SC without LR fails (1), no store", r2 == 1 && cell[0] == 11);
    (void)klauss_lr_d(&cell[0]);
    (void)exc_syscall(SYS_NOP, 0, 0, 0);
    h_count = 0;
    check("L2 SC after an exception entry fails",
          klauss_sc_d(&cell[0], 50) == 1 && cell[0] == 11);
    (void)klauss_lr_d(&cell[0]);
    check("L3 SC to another doubleword fails",
          klauss_sc_d(&cell[1], 50) == 1 && cell[1] == 0);
    (void)klauss_lr_d(&cell[0]);
    cell[0] = 12;                       // an ordinary store keeps the reservation
    check("L4 an ordinary store does not clear the reservation",
          klauss_sc_d(&cell[0], 13) == 0 && cell[0] == 13);
    uint32_t *w = (uint32_t *)&cell[1];
    w[0] = 0xFFFFFFFFu;
    uint64_t wv = klauss_lr_w(w);
    check("L5 LR.W zero-extends; SC.W stores 32 bits",
          wv == 0xFFFFFFFFull && klauss_sc_w(w, 0x12345678) == 0 &&
          cell[1] == 0x12345678ull);

    // ── U: user mode ─────────────────────────────────────────────────────
    check("U1 user code runs and returns", run_user(u_double, 21) == 42);
    check("U2 STATUS in user mode: UM = 1, DI = 0", run_user(u_status, 0) == STATUS_UM);
    check("U3 TP read/write in user mode",
          run_user(u_tp, 0xCAFEF00DDEADBEEFull) == 0xCAFEF00DDEADBEEFull &&
          exc_mfsr(SR_TP) == 0xCAFEF00DDEADBEEFull);
    check("U4 CYCLE readable in user mode", run_user(u_cycle, 0) == 1);
    h_count = 0;
    check("U5 SYSCALL from user mode", run_user(u_syscall, 4000) == 4320);
    check("U5 the handler saw PM = 1, kernel stack, USP in the user stack",
          h_count == 1 && (h_frame & FRAME_PM) && !in_ustack(h_regs_at) &&
          in_ustack(h_usp));
    h_count = 0;

    struct { const char *name; uint64_t (*fn)(uint64_t); uint32_t cause; } priv[] = {
        {"U6 DI in user mode: PRIVILEGED",          u_di,          EXC_PRIVILEGED},
        {"U6 EI in user mode: PRIVILEGED",          u_ei,          EXC_PRIVILEGED},
        {"U6 HALT in user mode: PRIVILEGED",        u_halt,        EXC_PRIVILEGED},
        {"U6 MFSR ecause in user mode: PRIVILEGED", u_mfsr_ecause, EXC_PRIVILEGED},
        {"U6 MFSR usp in user mode: PRIVILEGED",    u_mfsr_usp,    EXC_PRIVILEGED},
        {"U6 MTSR evec in user mode: PRIVILEGED",   u_mtsr_evec,   EXC_PRIVILEGED},
        {"U7 MMIO load in user mode: ACCESS_LOAD",  u_mmio_load,   EXC_ACCESS_LOAD},
        {"U7 MMIO store in user mode: ACCESS_STORE",u_mmio_store,  EXC_ACCESS_STORE},
    };
    for (unsigned i = 0; i < sizeof priv / sizeof priv[0]; i++) {
        h_count = 0;
        (void)run_user(priv[i].fn, 0);
        int ok = h_count == 1 && h_cause == priv[i].cause && (h_frame & FRAME_PM);
        if (!ok)
            printf("  got count=%u cause=%u eaddr=%08x\n", h_count, h_cause, h_eaddr);
        check(priv[i].name, ok);
    }
    check("U6 EVEC unchanged after the user MTSR", exc_mfsr(SR_EVEC) == (uintptr_t)exc_vector);
    h_count = 0;
    (void)run_user(u_mmio_load, 0);
    check("U7 ACCESS_LOAD EADDR = the MMIO address", h_eaddr == (uint32_t)(uintptr_t)&REG_INT_MASK);
    h_count = 0;
    check("U8 TRAP in user mode resumes after the handler", run_user(u_trap, 0) == 12);
    expect("U8 TRAP: cause, PC, PM = 1", EXC_TRAP, 0x6C040000u,
           (uint32_t)(uintptr_t)site_user_trap, 1);

    uint64_t usp0 = run_user(u_sp, 0);
    check("U9 user SP is on the user stack", in_ustack(usp0));
    const uint32_t bad_sp = 0xF0040000u + 0x100;      // MMIO: the PUSH must fault
    fix_push_bad_sp = bad_sp;
    fix_push_good_sp = (uintptr_t)&ustack2[256];
    h_count = 0;
    uint64_t pr = run_user(u_push, bad_sp);
    check("U10 PUSH via an MMIO SP: ACCESS_STORE, SP unchanged at the fault",
          h_count == 1 && h_cause == EXC_ACCESS_STORE && h_eaddr == bad_sp - 8 &&
          h_usp == bad_sp);
    check("U10 the retried PUSH pushed the right value", in_ustack(pr) && pr != 0);
    check("U10 nothing reached the device", REG_LEDS != 0x5);
    h_count = 0;

    // Timer interrupts in user mode, and atomics racing the ISR.
    REG_INT_MASK = 0;
    REG_INT_VEC(INT_SRC_TIMER) = (uint32_t)(uintptr_t)exc_timer_isr;
    REG_TIMER_PER = 3000;
    REG_INT_MASK = 1u << INT_SRC_TIMER;
    isr_frame = 0;
    uint64_t got = run_user(u_wait_ticks, 3);
    check("U11 timer interrupts are taken in user mode", got >= 3);
    check("U11 interrupt frame has PM = 1, ISR ran on the kernel stack",
          (isr_frame & FRAME_PM) && !in_ustack(isr_sp));

    isr_adds = 0;
    uint64_t bad = run_user(u_atomics, 0);
    REG_INT_MASK = 0;
    REG_INT_VEC(INT_SRC_TIMER) = 0;
    uint32_t n = isr_adds;
    printf("  (timer ISR added %u during the user loop)\n", n);
    check("U12 user atomics: CAS / exchange results", bad == 0);
    check("U12 64-bit counter = user + ISR increments", a64 == RACE_N + n);
    check("U12 32-bit counter", a32 == RACE_N + n);
    check("U12 16-bit counter (sub-word LR/SC), neighbour intact",
          a16[1] == (uint16_t)(RACE_N + n) && a16[0] == 0x1234);
    check("U12 8-bit counter (sub-word LR/SC), neighbours intact",
          a8[2] == (uint8_t)(RACE_N + n) && a8[1] == 9 && a8[0] == 0 && a8[3] == 0);
    check("U12 the ISR interrupted the user loop", n > 0);

    exc_disable();
    check("Z EVEC cleared by exc_disable", exc_mfsr(SR_EVEC) == 0);
    printf("Results: %d pass, %d fail\n", pass, fail);
    printf("test_exc %s\n", fail ? "FAILED" : "PASSED");
    return fail != 0;
}
