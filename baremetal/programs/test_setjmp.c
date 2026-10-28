// test_setjmp.c — setjmp / longjmp (src/setjmp.S) regression test.
//
// After longjmp, the function that called setjmp must continue with the SP a
// normal return from setjmp leaves, CallerSP (the return popped).  setjmp.S
// used to save its incoming SP (CallerSP - 8) as the caller's SP, so SP was
// 8 bytes low after longjmp.  That went unnoticed: locals are R15-relative,
// outgoing stack arguments are stored at the current SP (GETSP), and the
// epilogue sets SP from R15; but the frame layout no longer held.
//
// Checks, each after a normal return from setjmp and again after longjmp
// (from a function two calls down, with every callee-saved register live):
// SP, the setjmp return values, the callee-saved registers R4-R7, a call
// with stack arguments and a variadic call with stack arguments.  Prints
// "ALL PASS" when everything holds.
//
// picolibc's <setjmp.h> has no jmp_buf for KlaussCPU, so the prototypes are
// declared here: the buffer is setjmp.S's 7 slots of 8 bytes.

#include <stdarg.h>

int printf(const char *fmt, ...);

typedef long klauss_jmp_buf[7];
__attribute__((returns_twice)) int setjmp(long *env);
__attribute__((noreturn)) void longjmp(long *env, int val);

static klauss_jmp_buf env;
static int fails;

static void check(const char *what, long got, long want) {
  if (got != want) {
    printf("FAIL %s: got %ld, want %ld\n", what, got, want);
    fails++;
  } else {
    printf("PASS %s\n", what);
  }
}

// The arguments come from volatiles, so that no interprocedural constant
// propagation takes them out of the calls.
static volatile long arg[6] = {1, 2, 3, 4, 5, 6};

// Six arguments: the last two go on the stack.
__attribute__((noinline)) static long sum6(long a, long b, long c, long d,
                                           long e, long f) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f;
}

// Variadic, called with six values: two of them on the stack.
__attribute__((noinline)) static long vsum(int n, ...) {
  va_list ap;
  long s = 0;
  va_start(ap, n);
  for (int i = 0; i < n; i++)
    s += (i + 1) * va_arg(ap, long);
  va_end(ap);
  return s;
}

__attribute__((noinline)) static void jump2(int v) { longjmp(env, v); }
__attribute__((noinline)) static void jump1(int v) {
  // Clobber the callee-saved registers (this function saves and restores
  // them, but longjmp never returns through its epilogue).
  __asm__ volatile("setr r4, 0x1111\n\tsetr r5, 0x2222\n\t"
                   "setr r6, 0x3333\n\tsetr r7, 0x4444" ::: "r4", "r5", "r6",
                   "r7");
  jump2(v);
}

// What setjmp returned and the registers right after it, stored by the asm
// below: the return value, R4-R7, SP.
static volatile long after[6];

// setjmp is called from inside one asm statement, so that R4-R7 hold known
// values across it whatever the register allocator does, and both returns
// (the normal one and longjmp's) store the registers before any compiled
// code runs.
#define SETJMP_RECORD()                                                        \
  __asm__ volatile("setr r4, 0x44\n\tsetr r5, 0x55\n\t"                       \
                   "setr r6, 0x66\n\tsetr r7, 0x77\n\t"                       \
                   "leapc r0, env\n\t"                                         \
                   "callrel setjmp\n\t"                                        \
                   "leapc r13, after\n\t"                                      \
                   "stidx64 r12, r13, 0\n\t"                                   \
                   "stidx64 r4, r13, 8\n\t"                                    \
                   "stidx64 r5, r13, 16\n\t"                                   \
                   "stidx64 r6, r13, 24\n\t"                                   \
                   "stidx64 r7, r13, 32\n\t"                                   \
                   "getsp r14\n\t"                                             \
                   "stidx64 r14, r13, 40" ::                                    \
                       : "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8",  \
                         "r9", "r10", "r11", "r12", "r13", "r14", "memory")

__attribute__((noinline)) static void run(void) {
  volatile unsigned long sp_normal = 0;
  volatile int pass = 0;
  SETJMP_RECORD();
  printf("-- %s\n", pass == 0 ? "normal return" : "after longjmp");
  check("callee-saved r4", after[1], 0x44);
  check("callee-saved r5", after[2], 0x55);
  check("callee-saved r6", after[3], 0x66);
  check("callee-saved r7", after[4], 0x77);
  check("call with stack arguments",
        sum6(arg[0], arg[1], arg[2], arg[3], arg[4], arg[5]), 91);
  check("variadic call with stack arguments",
        vsum(6, arg[0], arg[1], arg[2], arg[3], arg[4], arg[5]), 91);
  if (pass == 0) {
    check("setjmp returns 0", after[0], 0);
    sp_normal = after[5];
    pass = 1;
    jump1(7);
  }
  check("setjmp returns the longjmp value", after[0], 7);
  check("SP after longjmp - SP after a normal return",
        (long)(after[5] - sp_normal), 0);
}

__attribute__((noinline)) static void run_zero(void) {
  volatile int pass = 0;
  int r = setjmp(env);
  if (pass == 0) {
    pass = 1;
    longjmp(env, 0);
  }
  check("longjmp(env, 0) makes setjmp return 1", r, 1);
}

int main(void) {
  printf("test_setjmp\n");
  run();
  run_zero();
  printf(fails ? "FAILED: %d\n" : "ALL PASS\n", fails);
  return fails != 0;
}
