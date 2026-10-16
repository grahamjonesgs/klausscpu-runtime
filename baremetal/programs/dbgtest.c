/* Debugger test program (debug/run_debug_smoke.sh): calls, recursion, a
   struct, a watched global, __builtin_debugtrap.  Build with -g, e.g.
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

__attribute__((noinline)) void bump(int times) {
  for (int i = 0; i < times; i++)
    counter += i;
}

int main(void) {
  struct point p = {seed - 2, seed - 1}, q = {seed + 1, seed + 3};
  long d = dist2(&p, &q);
  results[0] = d;
  results[1] = fib(seed + 3);
  bump(4);
  printf("d=%ld fib=%ld counter=%ld\n", d, results[1], (long)counter);
  __builtin_debugtrap();
  results[2] = d + results[1];
  return (int)(results[2] & 0x7f);
}
