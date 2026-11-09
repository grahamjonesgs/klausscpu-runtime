// test_mpu — MMU stage 2: the MPU (klaussCPU MMU_PLAN.md §10, branch `mmu`;
// runtime: exc.h).  Prints "SKIP" and passes on a core without
// CPU_FEATURES bit 1.
//
//  M1  MPU_EN with no regions: supervisor is default allow.
//  M2  GUARD in supervisor: loads and stores denied with ECAUSE[8] set; the
//      vector's GUARD path turns the MPU off; nothing is written.
//  M3  user mode: default deny, the union of R/W/X regions (an unmapped
//      buffer, a read-only buffer, a non-executable function), GUARD in user
//      mode, MPU_EN off, and the user-MMIO rule on top.
//  M4  a supervisor stack overflow into a GUARD region: the PUSH faults, the
//      vector moves to its emergency stack (MMU_PLAN §10.2), and the
//      interrupted code resumes with SP intact.
#include <stdint.h>
#include <stdio.h>

#include "../../exc.h"
#include "../../mmio.h"

static int pass, fail;
static void check(const char *name, int ok) {
    printf("%-56s %s\n", name, ok ? "PASS" : "FAIL");
    if (ok) pass++; else fail++;
}

#define DDR_LAST 0x07FFFFFFu

static volatile uint32_t h_count, h_cause, h_eaddr;
static volatile uint64_t h_frame, h_regs_at;

static int handler(struct exc_regs *r, uint32_t cause, uint32_t eaddr) {
    h_count++;
    h_cause = cause;
    h_eaddr = eaddr;
    h_frame = r->frame;
    h_regs_at = (uintptr_t)r;
    if (EXC_CAUSE(cause) == EXC_ACCESS_FETCH && (r->frame & FRAME_PM)) {
        // A user CALL into a non-executable function: the CALL completed,
        // so return to its caller as a RET would, with r12 = 0xFE.
        uint64_t usp = exc_mfsr(SR_USP);
        uint64_t ret = *(volatile uint64_t *)(uintptr_t)usp;
        exc_mtsr(SR_USP, usp + 8);
        r->frame = (r->frame & ~FRAME_PC_MASK) | (uint32_t)ret;
        r->r[12] = 0xFE;
        return 1;
    }
    exc_skip(r);
    return 1;
}

static void reset_h(void) { h_count = 0; h_cause = 0; h_eaddr = 0; }

static int saw(uint32_t cause, int guard, uint32_t eaddr) {
    int ok = h_count == 1 && EXC_CAUSE(h_cause) == cause &&
             !!(h_cause & ECAUSE_GUARD) == guard && h_eaddr == eaddr;
    if (!ok)
        printf("  got count=%u ecause=%x eaddr=%08x (want cause %u%s eaddr %08x)\n",
               h_count, h_cause, h_eaddr, cause, guard ? "+GUARD" : "", eaddr);
    reset_h();
    return ok;
}

static int mpu_on(void) { return (exc_mfsr(SR_STATUS) & STATUS_MPU_EN) != 0; }

// ── Memory with special permissions (32-byte granules) ───────────────────
static volatile uint64_t noaccess[8] __attribute__((aligned(32)));   // user: none
static volatile uint64_t robuf[4] __attribute__((aligned(32)));      // user: R
static volatile uint64_t gbuf[4] __attribute__((aligned(32)));       // GUARD
static volatile uint64_t plain[4] __attribute__((aligned(32)));      // user: RWX
static uint64_t ovf_stack[16] __attribute__((aligned(32)));          // M4

// A function alone in its own 32-byte granule (user: RW, not X).
extern char mpu_nx_func[];
extern uint64_t mpu_push_at(uint64_t sp);
__asm__(
    "  .text\n"
    "  .p2align 5\n"
    "  .globl mpu_nx_func\n"
    "mpu_nx_func:\n"
    "  setr r12, 77\n"
    "  ret\n"
    "  .p2align 5\n"
    // mpu_push_at(sp): run a PUSH with SP = sp, then restore SP; r12 = 0x5A.
    "  .globl mpu_push_at\n"
    "mpu_push_at:\n"
    "  getsp r1\n"
    "  setsp r0\n"
    "  push r1\n"
    "  setsp r1\n"
    "  setr r12, 0x5A\n"
    "  ret\n");

// Cover all of DDR with RWX except the holes, which get their own regions.
static void user_map(void) {
    struct { uint32_t lo, hi, attrs; } holes[] = {
        {(uint32_t)(uintptr_t)noaccess, (uint32_t)(uintptr_t)noaccess + 63, 0},
        {(uint32_t)(uintptr_t)robuf, (uint32_t)(uintptr_t)robuf + 31, MPU_EN | MPU_R},
        {(uint32_t)(uintptr_t)mpu_nx_func, (uint32_t)(uintptr_t)mpu_nx_func + 31,
         MPU_EN | MPU_R | MPU_W},
    };
    const unsigned nh = sizeof holes / sizeof holes[0];
    for (unsigned i = 1; i < nh; i++)               // sort by address
        for (unsigned j = i; j > 0 && holes[j].lo < holes[j - 1].lo; j--) {
            __typeof__(holes[0]) t = holes[j]; holes[j] = holes[j - 1]; holes[j - 1] = t;
        }
    unsigned n = 0;
    uint32_t cur = 0;
    for (unsigned i = 0; i < nh; i++) {
        if (holes[i].lo > cur)
            exc_mpu_set(n++, cur, holes[i].lo - 1, MPU_EN | MPU_R | MPU_W | MPU_X);
        if (holes[i].attrs)
            exc_mpu_set(n++, holes[i].lo, holes[i].hi, holes[i].attrs);
        cur = holes[i].hi + 1;
    }
    exc_mpu_set(n++, cur, DDR_LAST, MPU_EN | MPU_R | MPU_W | MPU_X);
}
#define GUARD_REGION 15

// ── User-mode code (must not touch MMIO) ─────────────────────────────────
static uint64_t ustack[1024] __attribute__((aligned(32)));
static uint64_t u_read_plain(uint64_t x) { (void)x; return plain[0]; }
static uint64_t u_read_noaccess(uint64_t x) { (void)x; return noaccess[2] + 1; }
static uint64_t u_write_noaccess(uint64_t x) { noaccess[2] = x; return 2; }
static uint64_t u_read_ro(uint64_t x) { (void)x; return robuf[1]; }
static uint64_t u_write_ro(uint64_t x) { robuf[1] = x; return 3; }
static uint64_t u_call_nx(uint64_t x) {
    (void)x;
    return ((uint64_t (*)(void))(uintptr_t)mpu_nx_func)();
}
static uint64_t u_read_nx_code(uint64_t x) {
    (void)x;
    return *(volatile uint32_t *)(uintptr_t)mpu_nx_func;
}
static uint64_t u_read_guard(uint64_t x) { (void)x; return gbuf[0] + 1; }
static uint64_t u_read_mmio(uint64_t x) { (void)x; return REG_INT_MASK + 1; }

static uint64_t run_user(uint64_t (*fn)(uint64_t), uint64_t arg) {
    return exc_run_user(fn, arg, &ustack[1024]);
}

int main(void) {
    printf("\n=== test_mpu: MMU stage 2 (MPU) ===\n");
    uint32_t feat = exc_init(handler);
    printf("CPU_FEATURES = %08x\n", (unsigned)feat);
    if (!(feat & CPU_FEAT_MPU)) {
        printf("SKIP: no MPU on this core\n");
        printf("test_mpu PASSED\n");
        return 0;
    }
    check("F0 16 regions", CPU_FEAT_MPU_REGIONS(feat) == 16);
    exc_mpu_reset();

    // M1
    exc_mpu_enable(1);
    plain[0] = 0x1234;
    noaccess[0] = 0x55;
    check("M1 MPU_EN, no regions: supervisor default allow",
          mpu_on() && plain[0] == 0x1234 && noaccess[0] == 0x55 && h_count == 0);

    // M2: GUARD in supervisor.
    gbuf[0] = 0xAA;
    exc_mpu_set(GUARD_REGION, (uint32_t)(uintptr_t)gbuf, (uint32_t)(uintptr_t)gbuf + 31,
                MPU_EN | MPU_GUARD);
    reset_h();
    uint64_t gv = gbuf[1];
    (void)gv;
    check("M2 supervisor load from GUARD: ACCESS_LOAD + GUARD",
          saw(EXC_ACCESS_LOAD, 1, (uint32_t)(uintptr_t)&gbuf[1]));
    check("M2 the vector's GUARD path turned the MPU off", !mpu_on());
    exc_mpu_enable(1);
    gbuf[2] = 0xBB;
    check("M2 supervisor store to GUARD: ACCESS_STORE + GUARD",
          saw(EXC_ACCESS_STORE, 1, (uint32_t)(uintptr_t)&gbuf[2]));
    check("M2 nothing was written", gbuf[2] == 0);   // MPU is off again here
    exc_mpu_enable(1);
    exc_mpu_set(GUARD_REGION, 0, 0, 0);
    check("M2 the guard region removed: supervisor access works", gbuf[0] == 0xAA);

    // M3: user mode.
    user_map();
    plain[0] = 0x777;
    robuf[1] = 0x4242;
    noaccess[2] = 0x99;
    exc_mpu_enable(1);
    reset_h();
    check("M3 user reads a RWX region", run_user(u_read_plain, 0) == 0x777 && h_count == 0);
    (void)run_user(u_read_noaccess, 0);
    check("M3 user load with no region: ACCESS_LOAD",
          saw(EXC_ACCESS_LOAD, 0, (uint32_t)(uintptr_t)&noaccess[2]));
    (void)run_user(u_write_noaccess, 0x1111);
    check("M3 user store with no region: ACCESS_STORE",
          saw(EXC_ACCESS_STORE, 0, (uint32_t)(uintptr_t)&noaccess[2]));
    check("M3 supervisor still default allow, store did not land",
          noaccess[2] == 0x99);
    check("M3 user reads an R region", run_user(u_read_ro, 0) == 0x4242 && h_count == 0);
    (void)run_user(u_write_ro, 0x5555);
    check("M3 user store to an R region: ACCESS_STORE",
          saw(EXC_ACCESS_STORE, 0, (uint32_t)(uintptr_t)&robuf[1]) && robuf[1] == 0x4242);
    uint64_t nx = run_user(u_call_nx, 0);
    check("M3 user call into a non-X region: ACCESS_FETCH",
          saw(EXC_ACCESS_FETCH, 0, (uint32_t)(uintptr_t)mpu_nx_func) && nx == 0xFE);
    check("M3 the non-X code is readable as data (R)",
          run_user(u_read_nx_code, 0) == *(volatile uint32_t *)(uintptr_t)mpu_nx_func &&
              h_count == 0);
    check("M3 supervisor can execute it (default allow)",
          ((uint64_t (*)(void))(uintptr_t)mpu_nx_func)() == 77);
    (void)run_user(u_read_mmio, 0);
    check("M3 user MMIO still faults with the MPU on",
          saw(EXC_ACCESS_LOAD, 0, (uint32_t)(uintptr_t)&REG_INT_MASK));
    exc_mpu_set(GUARD_REGION, (uint32_t)(uintptr_t)gbuf, (uint32_t)(uintptr_t)gbuf + 31,
                MPU_EN | MPU_GUARD);
    (void)run_user(u_read_guard, 0);
    check("M3 GUARD wins over a RWX region in user mode",
          saw(EXC_ACCESS_LOAD, 1, (uint32_t)(uintptr_t)&gbuf[0]));
    exc_mpu_set(GUARD_REGION, 0, 0, 0);
    exc_mpu_enable(0);
    check("M3 MPU_EN off: the user reaches the unmapped buffer",
          run_user(u_read_noaccess, 0) == 0x9A && h_count == 0);
    exc_mpu_reset();

    // M4: stack overflow into a guard.
    for (unsigned i = 0; i < 16; i++) ovf_stack[i] = 0;
    exc_mpu_set(0, (uint32_t)(uintptr_t)ovf_stack, (uint32_t)(uintptr_t)ovf_stack + 31,
                MPU_EN | MPU_GUARD);
    exc_mpu_enable(1);
    extern char exc_emergency_stack[], exc_emergency_top[];
    uint64_t top = (uintptr_t)&ovf_stack[4];          // just above the guard
    uint64_t rv = mpu_push_at(top);
    check("M4 PUSH into the guard: ACCESS_STORE + GUARD, EADDR = SP - 8",
          h_count == 1 && EXC_CAUSE(h_cause) == EXC_ACCESS_STORE &&
              (h_cause & ECAUSE_GUARD) && h_eaddr == top - 8);
    check("M4 the handler ran on the emergency stack",
          h_regs_at >= (uintptr_t)exc_emergency_stack && h_regs_at < (uintptr_t)exc_emergency_top);
    check("M4 the interrupted code resumed with SP intact", rv == 0x5A);
    check("M4 the PUSH did not store (only the entry frame is in the guard)",
          ovf_stack[2] == 0 && ovf_stack[0] == 0 && ovf_stack[1] == 0);
    check("M4 MPU left off by the vector", !mpu_on());
    reset_h();
    exc_mpu_reset();

    exc_disable();
    printf("Results: %d pass, %d fail\n", pass, fail);
    printf("test_mpu %s\n", fail ? "FAILED" : "PASSED");
    return fail != 0;
}
