// test_atomic — C atomics on KlaussCPU (src/atomic.c + the backend's forced
// atomics: plain loads/stores, compiler-barrier fences, __sync_* libcalls).
//
//  T1  every RMW op, xchg and cmpxchg at 1, 2, 4 and 8 bytes: old value,
//      new value, wrap-around, signed vs unsigned min/max, and the bytes
//      around the object left alone.  Results whose top bit is set check
//      that the libcall's zero-extended result compares right.
//  T2  the __sync_* builtins (val/bool CAS, lock_test_and_set/release).
//  T3  C11 <stdatomic.h>: _Atomic objects, atomic_flag, fences.
//  T4  a libcall leaves INT_MASK as it found it.
//  T5  stress: a timer ISR and main both increment counters of every width
//      (fetch_add, and a CAS loop at 8 bytes); the totals must be exact.
//      A plain counter, read before those atomic ops and written back after
//      them, shows the race being exercised (its lost updates are expected
//      and reported, not checked).
//
// Atomic within one core only: see src/atomic.c.
//
// Built with -DKLAUSS_LLEXT it is also a Zephyr LLEXT extension
// (test_atomic.llext), whose __sync_* calls resolve against the kernel's
// exports (ssh/llext_exports.c).  An extension shares the CPU with the
// kernel, so it must not write INT_MASK or a vector: T4 then only checks that
// a libcall leaves the mask the kernel runs with as it was, and T5 (which
// takes over the timer) is left out.
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include "../../mmio.h"

static int pass, fail;
static void check(const char *name, int ok) {
    if (!ok) printf("FAIL %s\n", name);
    if (ok) pass++; else fail++;
}

#define SC __ATOMIC_SEQ_CST

// One object of each width with a guard on either side.  The guards are
// checked after every op: a sub-word store must write only its own bytes.
#define GUARD 0xA5A5A5A5A5A5A5A5ull
#define DEFINE_BOX(T)                                                        \
    static struct { T lo; T x; T hi; } __attribute__((aligned(8))) box_##T;
DEFINE_BOX(uint8_t)  DEFINE_BOX(uint16_t)  DEFINE_BOX(uint32_t)
DEFINE_BOX(uint64_t)
DEFINE_BOX(int8_t)   DEFINE_BOX(int16_t)   DEFINE_BOX(int32_t)
DEFINE_BOX(int64_t)

// Keep the compiler from folding the operands: the values come through here.
static uint64_t opaque(uint64_t v) {
    __asm__ volatile("" : "+r"(v));
    return v;
}

// T: unsigned type, S: its signed twin, W: name suffix.
#define TEST_WIDTH(T, S, W)                                                  \
static void test_##W(void) {                                                 \
    const T MAX = (T)~(T)0, TOP = (T)(MAX ^ (MAX >> 1));                     \
    T *x = &box_##T.x;                                                       \
    S *s = &box_##S.x;                                                       \
    T old;                                                                   \
    box_##T.lo = box_##T.hi = (T)GUARD;                                      \
    box_##S.lo = box_##S.hi = (S)GUARD;                                      \
                                                                             \
    __atomic_store_n(x, (T)opaque(TOP | 5), SC);                             \
    check(#W " store/load", __atomic_load_n(x, SC) == (T)(TOP | 5));         \
    check(#W " load relaxed", __atomic_load_n(x, __ATOMIC_RELAXED)          \
                                  == (T)(TOP | 5));                          \
                                                                             \
    __atomic_store_n(x, MAX - 1, SC);                                        \
    old = __atomic_fetch_add(x, (T)opaque(3), SC);                           \
    check(#W " fetch_add (wraps)", old == (T)(MAX - 1) && *x == 1);          \
    old = __atomic_fetch_sub(x, (T)opaque(2), SC);                           \
    check(#W " fetch_sub (wraps)", old == 1 && *x == MAX);                   \
    old = __atomic_fetch_and(x, (T)opaque(TOP | 0x0F), SC);                  \
    check(#W " fetch_and", old == MAX && *x == (T)(TOP | 0x0F));             \
    old = __atomic_fetch_or(x, (T)opaque(0x30), SC);                         \
    check(#W " fetch_or", old == (T)(TOP | 0x0F) && *x == (T)(TOP | 0x3F));  \
    old = __atomic_fetch_xor(x, (T)opaque(TOP | 0x11), SC);                  \
    check(#W " fetch_xor", old == (T)(TOP | 0x3F) && *x == 0x2E);            \
    old = __atomic_fetch_nand(x, (T)opaque(0x0C), SC);                       \
    check(#W " fetch_nand", old == 0x2E && *x == (T)~(T)0x0C);               \
    old = __atomic_exchange_n(x, (T)opaque(TOP), SC);                        \
    check(#W " exchange", old == (T)~(T)0x0C && *x == TOP);                  \
                                                                             \
    /* unsigned min/max: TOP is the larger */                                \
    old = __atomic_fetch_max(x, (T)opaque(1), SC);                           \
    check(#W " fetch_umax keeps", old == TOP && *x == TOP);                  \
    old = __atomic_fetch_min(x, (T)opaque(1), SC);                           \
    check(#W " fetch_umin takes", old == TOP && *x == 1);                    \
    old = __atomic_fetch_max(x, (T)opaque(MAX), SC);                         \
    check(#W " fetch_umax takes", old == 1 && *x == MAX);                    \
    old = __atomic_fetch_min(x, (T)opaque(MAX), SC);                         \
    check(#W " fetch_umin keeps", old == MAX && *x == MAX);                  \
                                                                             \
    /* signed min/max: TOP is the most negative */                           \
    S sold;                                                                  \
    __atomic_store_n(s, (S)TOP, SC);                                         \
    sold = __atomic_fetch_max(s, (S)opaque(1), SC);                          \
    check(#W " fetch_max takes", sold == (S)TOP && *s == 1);                 \
    sold = __atomic_fetch_max(s, (S)opaque(-7), SC);                         \
    check(#W " fetch_max keeps", sold == 1 && *s == 1);                      \
    sold = __atomic_fetch_min(s, (S)opaque(-7), SC);                         \
    check(#W " fetch_min takes", sold == 1 && *s == -7);                     \
    sold = __atomic_fetch_min(s, (S)opaque(3), SC);                          \
    check(#W " fetch_min keeps", sold == -7 && *s == -7);                    \
                                                                             \
    /* cmpxchg, unsigned with the top bit set and signed negative */         \
    T exp = TOP | 2;                                                         \
    __atomic_store_n(x, TOP | 2, SC);                                        \
    int ok = __atomic_compare_exchange_n(x, &exp, (T)opaque(9), 0, SC, SC);  \
    check(#W " cmpxchg succeeds", ok && exp == (T)(TOP | 2) && *x == 9);     \
    exp = TOP | 2;                                                           \
    ok = __atomic_compare_exchange_n(x, &exp, (T)opaque(5), 0, SC, SC);      \
    check(#W " cmpxchg fails", !ok && exp == 9 && *x == 9);                  \
    S sexp = -3;                                                             \
    __atomic_store_n(s, -3, SC);                                             \
    ok = __atomic_compare_exchange_n(s, &sexp, (S)opaque(-4), 1, SC,         \
                                     __ATOMIC_RELAXED);                      \
    check(#W " cmpxchg signed", ok && sexp == -3 && *s == -4);               \
    sexp = -3;                                                               \
    ok = __atomic_compare_exchange_n(s, &sexp, 0, 0, SC, SC);                \
    check(#W " cmpxchg signed fails", !ok && sexp == -4 && *s == -4);        \
                                                                             \
    /* the __sync builtins */                                                \
    __atomic_store_n(x, TOP, SC);                                            \
    old = __sync_val_compare_and_swap(x, (T)opaque(TOP), (T)7);              \
    check(#W " __sync_val_cas", old == TOP && *x == 7);                      \
    check(#W " __sync_bool_cas fails",                                       \
          !__sync_bool_compare_and_swap(x, (T)opaque(TOP), (T)8) && *x == 7);\
    check(#W " __sync_bool_cas", __sync_bool_compare_and_swap(x, (T)7,       \
                                         (T)opaque(MAX)) && *x == MAX);      \
    old = __sync_fetch_and_add(x, 2);                                        \
    check(#W " __sync_fetch_and_add", old == MAX && *x == 1);                \
    check(#W " __sync_add_and_fetch", __sync_add_and_fetch(x, 4) == 5);      \
    old = __sync_lock_test_and_set(x, (T)opaque(TOP));                       \
    check(#W " __sync_lock_test_and_set", old == 5 && *x == TOP);            \
    __sync_lock_release(x);                                                  \
    check(#W " __sync_lock_release", *x == 0);                               \
                                                                             \
    check(#W " guards intact",                                               \
          box_##T.lo == (T)GUARD && box_##T.hi == (T)GUARD &&                \
          box_##S.lo == (S)GUARD && box_##S.hi == (S)GUARD);                 \
}

TEST_WIDTH(uint8_t,  int8_t,  8)
TEST_WIDTH(uint16_t, int16_t, 16)
TEST_WIDTH(uint32_t, int32_t, 32)
TEST_WIDTH(uint64_t, int64_t, 64)

// ── T3: C11 <stdatomic.h> ─────────────────────────────────────────────────
static _Atomic int c11_int;
static _Atomic unsigned long c11_long;
static _Atomic _Bool c11_bool;
static atomic_flag c11_flag = ATOMIC_FLAG_INIT;

static void test_c11(void) {
    atomic_store(&c11_int, -10);
    check("c11 atomic_fetch_add", atomic_fetch_add(&c11_int, 3) == -10 &&
                                      atomic_load(&c11_int) == -7);
    c11_int += 10;                       // atomic compound assignment
    check("c11 compound +=", atomic_load(&c11_int) == 3);
    c11_long = 0xFFFFFFFFul;
    c11_long++;
    check("c11 ++ (64-bit)", atomic_load_explicit(&c11_long,
                                 memory_order_acquire) == 0x100000000ul);
    unsigned long e = 1;
    check("c11 compare_exchange fails",
          !atomic_compare_exchange_strong(&c11_long, &e, 2) &&
              e == 0x100000000ul);
    check("c11 compare_exchange_weak",
          atomic_compare_exchange_weak(&c11_long, &e, 2) && c11_long == 2);
    atomic_store_explicit(&c11_bool, 1, memory_order_release);
    check("c11 exchange (_Bool)", atomic_exchange(&c11_bool, 0) == 1 &&
                                      !atomic_load(&c11_bool));
    check("c11 atomic_flag first", !atomic_flag_test_and_set(&c11_flag));
    check("c11 atomic_flag second", atomic_flag_test_and_set(&c11_flag));
    atomic_flag_clear(&c11_flag);
    check("c11 atomic_flag cleared", !atomic_flag_test_and_set(&c11_flag));
    atomic_thread_fence(memory_order_seq_cst);
    atomic_signal_fence(memory_order_seq_cst);
    check("c11 lock-free int/long", atomic_is_lock_free(&c11_int) &&
                                        atomic_is_lock_free(&c11_long));
}

// ── T4: INT_MASK is restored ─────────────────────────────────────────────
#ifdef KLAUSS_LLEXT
static void test_mask(void) {
    static uint32_t v;
    uint32_t m = REG_INT_MASK;
    __atomic_fetch_add(&v, 1, SC);
    uint32_t after = REG_INT_MASK;
    __atomic_exchange_n(&v, 0, SC);
    uint32_t after2 = REG_INT_MASK;
    if (after != m || after2 != m)
        printf("   INT_MASK %lu -> %lu / %lu\n", (unsigned long)m,
               (unsigned long)after, (unsigned long)after2);
    check("INT_MASK restored", after == m && after2 == m);
}
#else
static void test_mask(void) {
    static uint32_t v;
    // Vectors are 0, so no source can fire whatever the mask says.
    for (uint32_t m = 0; m < 16; m += 5) {
        REG_INT_MASK = m;
        __atomic_fetch_add(&v, 1, SC);
        uint32_t after = REG_INT_MASK;
        __atomic_exchange_n(&v, 0, SC);
        uint32_t after2 = REG_INT_MASK;
        if (after != m || after2 != m)
            printf("   INT_MASK %lu -> %lu / %lu\n", (unsigned long)m,
                   (unsigned long)after, (unsigned long)after2);
        check("INT_MASK restored", after == m && after2 == m);
    }
    REG_INT_MASK = 0;
}
#endif

#ifndef KLAUSS_LLEXT
// ── T5: main vs timer ISR ─────────────────────────────────────────────────
#define ITERS        20000
#define TIMER_CYCLES 3000

static uint8_t  st8;
static uint16_t st16;
static uint32_t st32;
static uint64_t st64, stcas;
static volatile uint64_t splain;
static volatile uint32_t ticks;

static void cas_inc(uint64_t *p) {
    uint64_t o = __atomic_load_n(p, __ATOMIC_RELAXED);
    while (!__atomic_compare_exchange_n(p, &o, o + 1, 1, SC, __ATOMIC_RELAXED)) {
    }
}

static void bump_all(void) {
    uint64_t plain = splain;    // non-atomic read-modify-write around the rest
    __atomic_fetch_add(&st8, 1, SC);
    __atomic_fetch_add(&st16, 1, SC);
    __atomic_fetch_add(&st32, 1, SC);
    __atomic_fetch_add(&st64, 1, SC);
    cas_inc(&stcas);
    splain = plain + 1;
}

void timer_isr_c(void) {
    bump_all();
    ticks++;
}

// Vector target: save the caller-saved registers, run the C handler, IRET
// (which restores PC, flags and INT_MASK).
__asm__(
    "  .text\n  .p2align 3\n  .globl atomic_timer_isr\n"
    "atomic_timer_isr:\n"
    "  push r0\n  push r1\n  push r2\n  push r3\n  push r8\n  push r9\n"
    "  push r10\n  push r11\n  push r12\n  push r13\n  push r14\n"
    "  call timer_isr_c\n"
    "  pop r14\n  pop r13\n  pop r12\n  pop r11\n  pop r10\n  pop r9\n"
    "  pop r8\n  pop r3\n  pop r2\n  pop r1\n  pop r0\n"
    "  iret\n");
extern void atomic_timer_isr(void);

static void test_stress(void) {
    REG_INT_MASK = 0;
    REG_INT_VEC(INT_SRC_TIMER) = (uint32_t)(uintptr_t)atomic_timer_isr;
    REG_TIMER_PER = TIMER_CYCLES;
    REG_INT_MASK = 1u << INT_SRC_TIMER;
    for (int i = 0; i < ITERS; i++)
        bump_all();
    REG_INT_MASK = 0;
    REG_INT_VEC(INT_SRC_TIMER) = 0;

    uint64_t total = ITERS + (uint64_t)ticks;
    printf("   %d main + %lu ISR increments; plain counter lost %lu\n", ITERS,
           (unsigned long)ticks, (unsigned long)(total - splain));
    check("stress: ISR ran often", ticks >= 100);
    check("stress: the race is real (plain counter lost updates)",
          splain < total);
    check("stress: 8-bit total", st8 == (uint8_t)total);
    check("stress: 16-bit total", st16 == (uint16_t)total);
    check("stress: 32-bit total", st32 == (uint32_t)total);
    check("stress: 64-bit total", st64 == total);
    check("stress: 64-bit CAS-loop total", stcas == total);
    if (st64 != total || stcas != total)
        printf("   st8=%lu st16=%lu st32=%lu st64=%lu stcas=%lu\n",
               (unsigned long)st8, (unsigned long)st16, (unsigned long)st32,
               (unsigned long)st64, (unsigned long)stcas);
}
#endif

int main(void) {
    printf("\n=== test_atomic: forced atomics (single core) ===\n");
    test_8();
    test_16();
    test_32();
    test_64();
    printf("T1/T2 per-width ops: %d pass, %d fail\n", pass, fail);
    test_c11();
    test_mask();
    printf("T3/T4 C11 + INT_MASK: %d pass, %d fail\n", pass, fail);
#ifndef KLAUSS_LLEXT
    test_stress();
#endif
    printf("Results: %d pass, %d fail\n", pass, fail);
    printf("test_atomic %s\n", fail ? "FAILED" : "PASSED");
    return fail != 0;
}
