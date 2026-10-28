/* Debugger test program (debug/run_debug_smoke.sh): calls, recursion, a
   struct, struct and 32-bit return values, a watched global,
   __builtin_debugtrap.  Build with -g, e.g.
   `make dbgtest.elf OPT="-O1 -g"`. */
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
  printf("d=%ld fib=%ld counter=%ld mid=%ld\n", d, results[1], (long)counter,
         results[3]);
  __builtin_debugtrap();
  results[2] = d + results[1];
  return (int)(results[2] & 0x7f);
}
