// test_divshadow — a divide right after a taken fused branch (board only).
//
// A taken FBR (ISA v3 B fused compare-and-branch) redirects from MEM and
// squashes the instruction behind it in EX.  If that instruction is a
// divide, the RTL divider has already latched its operands and keeps
// running: the squash does not reset it (pipeline_core.sv: the MD_IDLE start
// has no squash gate, and the FBR squash clears ex_valid but not div_ph).
// The next real divide that reaches EX while it runs then completes with the
// wrong-path quotient or remainder.  The compiler never fuses a branch whose
// next instruction is a divide (KlaussCPUFuseCmpBr.cpp); this program checks
// that on the board, where the bug shows.  The emulator has no pipeline and
// always passes.
//
//  T1  a guard that skips a divide: `if (b == 0 || b == -1) return 0;` falls
//      through to the DIVR, then an unrelated remainder must be exact.
//  T2  a loop whose exit block starts with a divide / remainder: every taken
//      latch branch would start a wrong-path divide, and the real one at the
//      exit would return an earlier iteration's result.
//  T3  picolibc's qsort, which divides right after a compare and branch: a
//      picolibc built by a compiler without the workaround sorts wrongly
//      (rebuild it with ../build-picolibc.sh).
#include <stdio.h>
#include <stdlib.h>

typedef unsigned long u64;
typedef long i64;
#define NOINLINE __attribute__((noinline))

static int pass, fail;
static void check(const char *name, int ok) {
    if (!ok) printf("FAIL %s\n", name);
    if (ok) pass++; else fail++;
}

NOINLINE static int guarded_div(i64 a, i64 b) {
    if (b == 0 || b == -1) return 0;
    i64 r = a / b;
    if (r == 0) return 1;
    return 2;
}
NOINLINE static u64 umod(u64 a, u64 b) { return a % b; }

NOINLINE static u64 loop_div(const u64 *p, u64 n, u64 d) {
    u64 s = 0, i = 0;
    do { s += p[i]; } while (++i != n);
    return s / d;
}
NOINLINE static i64 loop_rem(const i64 *p, u64 n, i64 d) {
    i64 s = 0;
    u64 i = 0;
    do { s += p[i]; } while (++i != n);
    return s % d;
}

static int cmpu(const void *a, const void *b) {
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}

int main(void) {
    // T1: a large dividend keeps the wrong-path signed a / -1 running long
    // enough for the remainder to find it.
    static volatile i64 va = 0x0123456789abcdefl, vb = -1;
    i64 a = va, b = vb;
    check("T1 guard", guarded_div(a, b) == 0);
    check("T1 remainder after guard", umod(a, b) == (u64)a);

    // T2
    static u64 p[64];
    static i64 q[64];
    for (int i = 0; i < 64; i++) {
        p[i] = 0x0123456789abcdeful * (u64)(i + 1);
        q[i] = (i64)p[i] ^ (i64)(i * 77);
    }
    // Divisors the compiler cannot see (no divide-by-constant expansion).
    static volatile u64 vd = 3;
    static volatile i64 vr = 1000003;
    u64 d = vd;
    i64 r = vr;
    for (u64 n = 1; n <= 64; n *= 2) {
        u64 s = 0;
        i64 t = 0;
        for (u64 i = 0; i < n; i++) { s += p[i]; t += q[i]; }
        char name[32];
        snprintf(name, sizeof name, "T2 loop_div n=%lu", n);
        check(name, loop_div(p, n, d) == s / d);
        snprintf(name, sizeof name, "T2 loop_rem n=%lu", n);
        check(name, loop_rem(q, n, r) == t % r);
    }

    // T3
    static u64 arr[200];
    u64 x = 88172645463325252ul;
    for (int i = 0; i < 200; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        arr[i] = x;
    }
    qsort(arr, 200, sizeof arr[0], cmpu);
    int sorted = 1;
    for (int i = 1; i < 200; i++) sorted &= arr[i - 1] <= arr[i];
    check("T3 qsort", sorted);

    printf("Results: %d pass, %d fail\n", pass, fail);
    printf("test_divshadow %s\n", fail ? "FAILED" : "PASSED");
    return fail != 0;
}
