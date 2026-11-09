// test_tlb — MMU stage 3: the software-refilled TLB and paging (klaussCPU
// MMU_PLAN.md §11, branch `mmu`; runtime: exc.h).  Prints "SKIP" and passes
// on a core without CPU_FEATURES bit 2.
//
// Setup: the program's own pages (0.._end) and the crt0 stack page are
// wired as global identity entries, so it keeps running when TE turns on
// (§11.2: the instruction after the enabling MTSR is fetched translated).
// The test body runs on a kseg stack and EVEC is exc_vector's kseg alias,
// because entry pushes are untranslated and need a kseg SP (§11.3).
// A small software page table maps test pages at VA 0x4000_0000+ to
// frames at PA 0x0600_0000+; the handler refills from it.
//
//  T1  wired entries read back (TLBR); TE on; identity and kseg aliases agree
//  T2  the kseg hole (0x8800_0000) faults ACCESS_LOAD
//  T3  load miss: TLB_MISS_LOAD, EADDR = VA, TLB_HI auto-loaded, refill, retry
//  T4  store to a W = 0 page: miss, refill R-only, ACCESS_STORE, the handler
//      sets W (TLBP finds the entry from the auto-loaded TLB_HI), retry
//  T5  fetch miss: code copied into a frame through kseg, ICACHE_INV, called
//  T6  ASIDs: a non-global entry matches only its ASID
//  T7  TLBP hit and miss (P set, index unchanged)
//  T8  LR/SC: TLBWI clears the reservation
//  T9  user mode with TE = 1: TLB use, a user miss, kseg denied, non-X fetch
//  T10 TE off, TLBINV
#include <stdint.h>
#include <stdio.h>

#include "../../exc.h"
#include "../../mmio.h"

static int pass, fail;
static void check(const char *name, int ok) {
    printf("%-56s %s\n", name, ok ? "PASS" : "FAIL");
    if (ok) pass++; else fail++;
}

extern char _end[];

// ── Software page table ───────────────────────────────────────────────────
struct pte { uint32_t va, pa, perms; int asid; int dirty_track; int dirty; };
#define VA0  0x40000000u
#define PA0  0x06000000u
static struct pte pt[] = {
    {VA0 + 0x00000, PA0 + 0x00000, TLB_LO_R | TLB_LO_W, -1, 0, 0},   // T3 data
    {VA0 + 0x10000, PA0 + 0x10000, TLB_LO_R | TLB_LO_W, -1, 1, 0},   // T4 dirty-tracked
    {VA0 + 0x20000, PA0 + 0x20000, TLB_LO_R | TLB_LO_X, -1, 0, 0},   // T5 code
    {VA0 + 0x30000, PA0 + 0x30000, TLB_LO_R | TLB_LO_W, 5, 0, 0},    // T6 ASID 5 only
    {VA0 + 0x40000, PA0 + 0x40000, TLB_LO_R | TLB_LO_W, -1, 0, 0},   // T9 user data
    {VA0 + 0x50000, PA0 + 0x20000, TLB_LO_R, -1, 0, 0},              // T9 code, not X
};
#define NPT (sizeof pt / sizeof pt[0])

static unsigned n_wired, rr, n_tlb = 32;
static volatile uint32_t n_miss, n_access, n_other, last_cause, last_eaddr, last_tlb_hi;
static volatile uint32_t last_pm;

static void tlb_write(unsigned idx, uint32_t hi, uint32_t lo) {
    exc_mtsr(SR_TLB_INDEX, idx);
    exc_mtsr(SR_TLB_HI, hi);
    exc_mtsr(SR_TLB_LO, lo);
    klauss_tlbwi();
}

static struct pte *pt_find(uint32_t va, uint32_t asid) {
    for (unsigned i = 0; i < NPT; i++)
        if ((va & ~(TLB_PAGE - 1)) == pt[i].va && (pt[i].asid < 0 || (uint32_t)pt[i].asid == asid))
            return &pt[i];
    return 0;
}

static int handler(struct exc_regs *r, uint32_t ecause, uint32_t eaddr) {
    uint32_t c = EXC_CAUSE(ecause);
    last_cause = c;
    last_eaddr = eaddr;
    last_pm = (r->frame & FRAME_PM) != 0;
    if (c >= EXC_TLB_MISS_LOAD && c <= EXC_TLB_MISS_FETCH) {
        n_miss++;
        uint32_t hi = (uint32_t)exc_mfsr(SR_TLB_HI);
        last_tlb_hi = hi;
        struct pte *p = pt_find(eaddr, hi & 0xFF);
        if (p) {                       // refill: TLB_HI is already {VPN, G = 0, ASID}
            uint32_t lo = (p->pa & 0x07FF0000u) | TLB_LO_V | p->perms;
            if (p->dirty_track && !p->dirty) lo &= ~TLB_LO_W;   // first write faults
            if (p->asid < 0) hi |= TLB_HI_G;
            tlb_write(n_wired + (rr++ % (n_tlb - n_wired)), hi, lo);
            return 1;                  // retry the instruction
        }
        exc_skip(r);                   // not mapped: give up on the access
        return 1;
    }
    if (c == EXC_ACCESS_STORE && eaddr < KSEG_BASE) {
        uint32_t hi = (uint32_t)exc_mfsr(SR_TLB_HI);
        struct pte *p = pt_find(eaddr, hi & 0xFF);
        if (p && p->dirty_track && !p->dirty) {
            n_access++;
            p->dirty = 1;              // the page is dirty now: allow writes
            klauss_tlbp();             // TLB_HI was auto-loaded by the fault
            if (exc_mfsr(SR_TLB_INDEX) & TLB_INDEX_P) { n_other++; exc_skip(r); return 1; }
            klauss_tlbr();
            exc_mtsr(SR_TLB_LO, exc_mfsr(SR_TLB_LO) | TLB_LO_W);
            klauss_tlbwi();
            return 1;                  // retry the store
        }
    }
    if (c == EXC_ACCESS_FETCH && (r->frame & FRAME_PM)) {
        // A user CALL into a non-executable page: return to the caller.
        n_access++;
        uint64_t usp = exc_mfsr(SR_USP);
        uint64_t ret = *(volatile uint64_t *)(uintptr_t)usp;
        exc_mtsr(SR_USP, usp + 8);
        r->frame = (r->frame & ~FRAME_PC_MASK) | (uint32_t)ret;
        r->r[12] = 0xFE;
        return 1;
    }
    n_other++;
    exc_skip(r);
    return 1;
}

static void reset_counts(void) { n_miss = n_access = n_other = 0; last_cause = 0; }

// ── asm helpers ──────────────────────────────────────────────────────────
// run_on_stack(fn, sp, arg): call fn(arg) with SP = sp; restore SP after.
// tlb_code_tmpl..tlb_code_end: position-independent `return 0x77`.
extern uint64_t run_on_stack(uint64_t (*fn)(uint64_t), uint64_t sp, uint64_t arg);
extern char tlb_code_tmpl[], tlb_code_end[];
__asm__(
    "  .text\n"
    "  .globl run_on_stack\n"
    "run_on_stack:\n"
    "  getsp r3\n"
    "  setsp r1\n"
    "  push r3\n"
    "  copy r1, r0\n"
    "  copy r0, r2\n"
    "  callr r1\n"
    "  pop r3\n"
    "  setsp r3\n"
    "  ret\n"
    "  .p2align 3\n"
    "  .globl tlb_code_tmpl\n"
    "tlb_code_tmpl:\n"
    "  setr r12, 0x77\n"
    "  ret\n"
    "  .globl tlb_code_end\n"
    "tlb_code_end:\n");

static uint64_t kstack[2048] __attribute__((aligned(16)));
static uint64_t ustack[1024] __attribute__((aligned(16)));
static volatile uint64_t gword = 0x5151;
static uint64_t lrsc_cell __attribute__((aligned(8)));

static volatile uint64_t *kseg(const volatile void *pa) {
    return (volatile uint64_t *)((uintptr_t)pa + KSEG_BASE);
}

// ── User code (TE = 1; must not touch MMIO) ───────────────────────────────
static uint64_t u_read(uint64_t va) { return *(volatile uint64_t *)(uintptr_t)va; }
static uint64_t u_call(uint64_t va) { return ((uint64_t (*)(void))(uintptr_t)va)(); }

// T1 (on the physical stack: kseg addresses only exist once TE = 1).
static void setup_te(void) {
    // Wire the program's pages and the crt0 stack page.
    n_wired = 0;
    uint32_t last = ((uint32_t)(uintptr_t)_end + TLB_PAGE - 1) & ~(TLB_PAGE - 1);
    for (uint32_t va = 0; va < last; va += TLB_PAGE)
        tlb_write(n_wired++, va | TLB_HI_G, va | TLB_LO_V | TLB_LO_R | TLB_LO_W | TLB_LO_X);
    tlb_write(n_wired++, 0x07FF0000u | TLB_HI_G,
              0x07FF0000u | TLB_LO_V | TLB_LO_R | TLB_LO_W | TLB_LO_X);
    for (unsigned i = n_wired; i < n_tlb; i++) tlb_write(i, 0, 0);
    printf("  %u wired identity entries (program ends at %08lx)\n", n_wired,
           (unsigned long)(uintptr_t)_end);
    exc_mtsr(SR_TLB_INDEX, 0);
    klauss_tlbr();
    check("T1 TLBR reads back a wired entry",
          (exc_mfsr(SR_TLB_HI) & 0xFFFF01FFu) == TLB_HI_G &&
              (exc_mfsr(SR_TLB_LO) & 0x07FF000Fu) == 0xF);
    exc_mtsr(SR_EVEC, KSEG_BASE | (uint32_t)exc_mfsr(SR_EVEC));
    exc_mtsr(SR_ASID, 5);
    exc_mtsr(SR_STATUS, exc_mfsr(SR_STATUS) | STATUS_TE);
    check("T1 TE on, still running", (exc_mfsr(SR_STATUS) & STATUS_TE) != 0);
    check("T1 identity and kseg aliases agree", gword == 0x5151 && *kseg(&gword) == 0x5151);
}

// T2..T9 run on a kseg stack: with TE = 1 an exception entry needs one.
static uint64_t body(uint64_t feat) {
    (void)feat;

    // T2
    reset_counts();
    (void)*(volatile uint64_t *)(uintptr_t)0x88000000u;
    check("T2 the kseg hole faults ACCESS_LOAD",
          last_cause == EXC_ACCESS_LOAD && last_eaddr == 0x88000000u && n_other == 1);

    // T3
    *kseg((void *)(uintptr_t)(PA0 + 8)) = 0xABCD;
    reset_counts();
    uint64_t v = *(volatile uint64_t *)(uintptr_t)(VA0 + 8);
    check("T3 load miss: TLB_MISS_LOAD, EADDR, TLB_HI auto-loaded",
          n_miss == 1 && last_cause == EXC_TLB_MISS_LOAD && last_eaddr == VA0 + 8 &&
              last_tlb_hi == (VA0 | 5));
    check("T3 refilled and retried: the value through kseg", v == 0xABCD);
    reset_counts();
    v = *(volatile uint64_t *)(uintptr_t)(VA0 + 16);
    check("T3 the next access hits", n_miss == 0 && v == *kseg((void *)(uintptr_t)(PA0 + 16)));

    // T4
    reset_counts();
    *(volatile uint64_t *)(uintptr_t)(VA0 + 0x10000 + 24) = 0x7777;
    check("T4 store: miss, then ACCESS_STORE on W = 0, then W set",
          n_miss == 1 && n_access == 1 && n_other == 0 && pt[1].dirty == 1);
    check("T4 the store landed", *kseg((void *)(uintptr_t)(PA0 + 0x10000 + 24)) == 0x7777);

    // T5
    {
        volatile uint32_t *dst = (volatile uint32_t *)kseg((void *)(uintptr_t)(PA0 + 0x20000));
        const uint32_t *src = (const uint32_t *)tlb_code_tmpl;
        for (unsigned i = 0; i < (unsigned)(tlb_code_end - tlb_code_tmpl) / 4; i++)
            dst[i] = src[i];
        icache_invalidate();
        reset_counts();
        uint64_t rv = ((uint64_t (*)(void))(uintptr_t)(VA0 + 0x20000))();
        check("T5 fetch miss: TLB_MISS_FETCH, refilled, the code ran",
              n_miss == 1 && last_cause == EXC_TLB_MISS_FETCH && last_eaddr == VA0 + 0x20000 &&
                  rv == 0x77);
    }

    // T6
    *kseg((void *)(uintptr_t)(PA0 + 0x30000)) = 0x3030;
    reset_counts();
    v = *(volatile uint64_t *)(uintptr_t)(VA0 + 0x30000);
    check("T6 ASID 5: miss, refill, value", n_miss == 1 && v == 0x3030);
    exc_mtsr(SR_ASID, 6);
    reset_counts();
    (void)*(volatile uint64_t *)(uintptr_t)(VA0 + 0x30000);
    check("T6 ASID 6: the ASID-5 entry does not match (miss)",
          n_miss == 1 && last_tlb_hi == ((VA0 + 0x30000) | 6));
    exc_mtsr(SR_ASID, 5);
    reset_counts();
    v = *(volatile uint64_t *)(uintptr_t)(VA0 + 0x30000);
    check("T6 back to ASID 5: hits the old entry", n_miss == 0 && v == 0x3030);

    // T7
    exc_mtsr(SR_TLB_HI, VA0 | 5);
    klauss_tlbp();
    uint32_t idx = (uint32_t)exc_mfsr(SR_TLB_INDEX);
    check("T7 TLBP finds the VA0 entry", !(idx & TLB_INDEX_P) && (idx & 31) >= n_wired);
    exc_mtsr(SR_TLB_INDEX, 3);
    exc_mtsr(SR_TLB_HI, 0x41000000u | 5);
    klauss_tlbp();
    idx = (uint32_t)exc_mfsr(SR_TLB_INDEX);
    check("T7 TLBP miss: P set, index unchanged", (idx & TLB_INDEX_P) && (idx & 31) == 3);

    // T8
    lrsc_cell = 1;
    (void)klauss_lr_d(&lrsc_cell);
    exc_mtsr(SR_TLB_INDEX, 0);
    klauss_tlbr();
    klauss_tlbwi();                     // rewrite wired entry 0 unchanged
    check("T8 SC after TLBWI fails", klauss_sc_d(&lrsc_cell, 2) == 1 && lrsc_cell == 1);
    (void)klauss_lr_d(&lrsc_cell);
    check("T8 SC without a TLB change succeeds", klauss_sc_d(&lrsc_cell, 3) == 0 && lrsc_cell == 3);

    // T9: user mode.
    reset_counts();
    check("T9 user reads a mapped page (TLB hit)",
          exc_run_user(u_read, VA0 + 8, &ustack[1024]) == 0xABCD && n_miss == 0);
    *kseg((void *)(uintptr_t)(PA0 + 0x40000)) = 0x4444;
    reset_counts();
    check("T9 user miss: refilled, value",
          exc_run_user(u_read, VA0 + 0x40000, &ustack[1024]) == 0x4444 && n_miss == 1 &&
              last_pm == 1);
    reset_counts();
    (void)exc_run_user(u_read, (uintptr_t)kseg(&gword), &ustack[1024]);
    check("T9 user access to kseg: ACCESS_LOAD",
          last_cause == EXC_ACCESS_LOAD && last_pm == 1 &&
              last_eaddr == (uint32_t)(uintptr_t)kseg(&gword));
    reset_counts();
    uint64_t rv = exc_run_user(u_call, VA0 + 0x50000, &ustack[1024]);
    check("T9 user call into a page without X: miss, then ACCESS_FETCH",
          n_miss == 1 && n_access == 1 && last_cause == EXC_ACCESS_FETCH && rv == 0xFE);

    return 0;
}

// T10 (back on the physical stack).
static void teardown_te(void) {
    exc_mtsr(SR_STATUS, exc_mfsr(SR_STATUS) & ~(uint64_t)STATUS_TE);
    exc_mtsr(SR_EVEC, exc_mfsr(SR_EVEC) & ~(uint64_t)KSEG_BASE);
    check("T10 TE off: physical again", gword == 0x5151 && !(exc_mfsr(SR_STATUS) & STATUS_TE));
    klauss_tlbinv();
    int any = 0;
    for (unsigned i = 0; i < n_tlb; i++) {
        exc_mtsr(SR_TLB_INDEX, i);
        klauss_tlbr();
        any |= (int)(exc_mfsr(SR_TLB_LO) & TLB_LO_V);
    }
    check("T10 TLBINV clears every V bit", !any);
    exc_mtsr(SR_ASID, 0);
}

int main(void) {
    printf("\n=== test_tlb: MMU stage 3 (TLB) ===\n");
    uint32_t feat = exc_init(handler);
    printf("CPU_FEATURES = %08x\n", (unsigned)feat);
    if (!(feat & CPU_FEAT_TLB)) {
        printf("SKIP: no TLB on this core\n");
        printf("test_tlb PASSED\n");
        return 0;
    }
    n_tlb = CPU_FEAT_TLB_ENTRIES(feat);
    printf("  %u TLB entries\n", n_tlb);
    // Any count the INDEX field [4:0] can address, with room to refill.
    check("T0 TLB entry count", n_tlb >= 8 && n_tlb <= 32);
    if (n_tlb < 8 || n_tlb > 32) {
        printf("Results: %d pass, %d fail\n", pass, fail);
        printf("test_tlb FAILED\n");
        return 1;
    }
    setup_te();
    run_on_stack(body, KSEG_BASE + (uintptr_t)&kstack[2048], feat);
    teardown_te();
    exc_disable();
    printf("Results: %d pass, %d fail\n", pass, fail);
    printf("test_tlb %s\n", fail ? "FAILED" : "PASSED");
    return fail != 0;
}
