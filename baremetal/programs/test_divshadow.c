// test_divshadow — a divide right after a fused branch (board only).
//
// A taken FBR (ISA v3 B fused compare-and-branch) redirects from MEM and
// squashes the instruction behind it in EX.  If that instruction is a
// divide, it has already started the RTL divider.  Before klaussCPU 9ad6614
// the squash did not reset the divider (div_ph), so the next real divide
// that reached EX within ~64 cycles completed with the wrong-path quotient
// or remainder, and the compiler never fused a branch in front of a
// divide.  9ad6614 resets the divider in the squash, and klausscpu-llvm now
// fuses there freely: it supports only 9ad6614 and later.  This program
// checks that the shape is correct on the board.  The emulator has no
// pipeline and always passes; on an older bitstream T4 fails.
//
// Built with klausscpu-llvm after the workaround was dropped, T1, T2 and T4
// each put an FBR directly in front of a DIVR/DIVUR/MODR/MODUR (check with
// llvm-objdump -d); T3 does too once picolibc is rebuilt.
//
//  T1  a guard that skips a divide: `if (b == 0 || b == -1) return 0;` falls
//      through to the DIVR, then an unrelated remainder must be exact.
//  T2  a loop whose exit block starts with a divide / remainder: every taken
//      latch branch squashes a wrong-path divide, and the real one at the
//      exit must still return this call's result.
//  T3  picolibc's qsort, which divides right after a compare and branch.
//  T4  a fused branch with a divide behind it and a different divide at its
//      target, taken (the target divide starts a few cycles after the
//      squashed one) and not taken, for signed and unsigned quotients and
//      remainders, and again with another divide just before the branch.
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

// T4: a fused branch on c directly followed by one divide, with the other
// at its target (the layout decides which is which; T4 calls each function
// with c == 0 and c != 0, so the branch is taken once and not taken once).
// Four arguments, all in registers, and different divisors, so the two
// divides stay apart and nothing else sits between the branch and the
// divide.  Neither divisor is 0 (a divide by 0 never starts the divider).
NOINLINE static i64 sel_div(i64 c, i64 a, i64 b, i64 x) {
    if (c == 0) return a / b;
    return x / a;
}
NOINLINE static u64 sel_udiv(u64 c, u64 a, u64 b, u64 x) {
    if (c == 0) return a / b;
    return x / a;
}
NOINLINE static i64 sel_mod(i64 c, i64 a, i64 b, i64 x) {
    if (c == 0) return a % b;
    return x % a;
}
NOINLINE static u64 sel_umod(u64 c, u64 a, u64 b, u64 x) {
    if (c == 0) return a % b;
    return x % a;
}
// A divide before the branch too: the divider finishes that one, starts
// the squashed one, then the target's.
static volatile u64 sink;
NOINLINE static u64 sel_chain(u64 c, u64 a, u64 b, u64 x) {
    sink = x / b;
    if (c == 0) return a / b;
    return x % a;
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

    // T4: the divide the branch skips gets a large dividend (it runs long),
    // the one that runs another dividend and another result.  The expected
    // values are constants, not computed with a divide.
    static volatile i64 vc = 1, vbig = 0x7edcba9876543210l, v3 = 3, v7 = 7,
                        v1000 = 1000, vm1000 = -1000;
    static volatile u64 vubig = 0xfedcba9876543210ul;
    i64 c1 = vc, big = vbig, s3 = v3, s7 = v7, s1000 = v1000, m1000 = vm1000;
    u64 ubig = vubig, u1 = (u64)c1, u3 = (u64)s3, u7 = (u64)s7,
        u1000 = (u64)s1000;
    for (int i = 0; i < 4; i++) {
        check("T4 div c=0", sel_div(0, s1000, s7, big) == 142);
        check("T4 div c=0 neg", sel_div(0, m1000, s7, big) == -142);
        check("T4 div c=1", sel_div(c1, s7, s3, s1000) == 142);
        check("T4 div c=1 neg", sel_div(c1, s7, s3, m1000) == -142);
        check("T4 udiv c=0", sel_udiv(0, u1000, u7, ubig) == 142);
        check("T4 udiv c=1", sel_udiv(u1, u7, u3, u1000) == 142);
        check("T4 mod c=0", sel_mod(0, s1000, s7, big) == 6);
        check("T4 mod c=0 neg", sel_mod(0, m1000, s7, big) == -6);
        check("T4 mod c=1", sel_mod(c1, s7, s3, s1000) == 6);
        check("T4 mod c=1 neg", sel_mod(c1, s7, s3, m1000) == -6);
        check("T4 umod c=0", sel_umod(0, u1000, u7, ubig) == 6);
        check("T4 umod c=1", sel_umod(u1, u7, u3, u1000) == 6);
        check("T4 chain c=0", sel_chain(0, u1000, u7, ubig) == 142 &&
                                  sink == 0x2468acf13579be02ul);
        check("T4 chain c=1", sel_chain(u1, u7, u3, u1000) == 6 &&
                                  sink == 333);
    }

    printf("Results: %d pass, %d fail\n", pass, fail);
    printf("test_divshadow %s\n", fail ? "FAILED" : "PASSED");
    return fail != 0;
}
