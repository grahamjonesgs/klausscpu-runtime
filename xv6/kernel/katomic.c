// klausscpu: the atomics clang calls out for (KlaussCPU has no atomic swap
// instruction; atomic exchange becomes __sync_lock_test_and_set_N).  The
// kernel runs on one CPU, so disabling interrupts is enough.  The symbols
// are bound with asm labels: the names are compiler builtins.
#include "types.h"

uint32 katomic_xchg4(volatile uint32 *p, uint32 v) __asm__("__sync_lock_test_and_set_4");
uint64 katomic_xchg8(volatile uint64 *p, uint64 v) __asm__("__sync_lock_test_and_set_8");

uint32
katomic_xchg4(volatile uint32 *p, uint32 v)
{
  unsigned int k = __builtin_klausscpu_di();
  uint32 old = *p;
  *p = v;
  __builtin_klausscpu_ei(k);
  return old;
}

uint64
katomic_xchg8(volatile uint64 *p, uint64 v)
{
  unsigned int k = __builtin_klausscpu_di();
  uint64 old = *p;
  *p = v;
  __builtin_klausscpu_ei(k);
  return old;
}
