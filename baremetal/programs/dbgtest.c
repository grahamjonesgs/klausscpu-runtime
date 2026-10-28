/* Debugger test program (debug/run_debug_smoke.sh): calls, recursion, a
   struct, struct and 32-bit return values, a watched global, functions
   without a frame record (a frameless leaf, a shrink-wrapped early exit, a
   tail call), values kept in callee-saved registers across calls,
   __builtin_debugtrap.  Build with -g, e.g. `make dbgtest.elf OPT="-O1 -g"`.
   tools/validate/lldb_e2e.sh also runs it at -O0 and -O2 (lldb/e2e.lldb). */
#include <stdio.h>

struct point { long x, y; };

volatile long seed = 3;           /* volatile inputs defeat constant folding */
volatile long counter;            /* watchpoint target */
long results[8];

__attribute__((noinline)) static long square(long v) { return v * v; }

__attribute__((noinline)) long dist2(const struct point *a, const struct point *b) {
  long dx = a->x - b->x;
  long dy = a->y - b->y;
  return square(dx) + square(dy);
}

__attribute__((noinline)) long fib(long n) {
  if (n < 2)
    return n;
  return fib(n - 1) + fib(n - 2);
}

/* A 16-byte struct comes back in r12/r11, an unsigned int sign-extended in
   r12 (the ABI, llvm/lib/Target/KlaussCPU/ABI.md). */
__attribute__((noinline)) struct point midpoint(const struct point *a,
                                                const struct point *b) {
  struct point m = {(a->x + b->x) / 2, (a->y + b->y) / 2};
  return m;
}

__attribute__((noinline)) unsigned neg_u32(long v) { return (unsigned)-v; }

/* square() is a leaf without a frame; hop() has none either and jumps to
   square() (a tail call), so square() returns straight to main(); checked()
   builds its frame only on the path that calls (shrink-wrapping).  The
   debugger unwinds through all of them from the CFI. */
__attribute__((noinline)) long hop(long v) { return square(v + 1); }

__attribute__((noinline)) long checked(const long *p) {
  if (!p)
    return -1;
  return square(*p) + 1;
}

/* Callee-saved registers in outer frames: from -O1 on, keeper() keeps a and
   b in callee-saved registers (R4-R7) across its call, clobber() saves them
   to keep its own four values there across the call to leafsum(), and main()
   keeps d in one across all of it.  Stopped in leafsum(), the debugger finds
   keeper's a and b, and main's d, in clobber's and keeper's register saves
   (the CFI's DW_CFA_offset rules). */
__attribute__((noinline)) long leafsum(long x, long y) { return x * y + seed; }

__attribute__((noinline)) long clobber(long x) {
  long p = x + seed, q = x * seed, s = x ^ seed, t = x - seed;
  long r = leafsum(p, q);
  return ((r * p + q) * s) + t;
}

__attribute__((noinline)) long keeper(long k) {
  long a = k * 3 + seed, b = k - seed;
  long r = clobber(a);
  return a * r + b;
}

__attribute__((noinline)) void bump(int times) {
  for (int i = 0; i < times; i++)
    counter += i;
}

int main(void) {
  struct point p = {seed - 2, seed - 1}, q = {seed + 1, seed + 3};
  struct point m = midpoint(&p, &q);
  results[3] = m.x + m.y + neg_u32(seed);
  long d = dist2(&p, &q);
  results[0] = d;
  results[1] = fib(seed + 3);
  bump(4);
  long seven = seed + 4;
  results[4] = hop(seed + 1);
  results[5] = checked(0) + checked(&seven);
  results[6] = keeper(seed + 2);
  printf("d=%ld fib=%ld counter=%ld mid=%ld\n", d, results[1], (long)counter,
         results[3]);
  printf("hop=%ld checked=%ld\n", results[4], results[5]);
  __builtin_debugtrap();
  results[2] = d + results[1];
  return (int)(results[2] & 0x7f);
}
