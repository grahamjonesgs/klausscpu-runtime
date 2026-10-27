/*
 * boardsupport.c — Embench-IoT board support for the KlaussCPU bare-metal
 * runtime.  The upstream Embench sources are fetched by ../get-benchmarks.sh
 * and are not part of this repo.
 *
 * Speed build (default):
 *   start_trigger()/stop_trigger() snapshot the 64-bit hardware cycle and
 *   instruction counters (REG_PERF_CYCLES / REG_PERF_INSTR).  Interrupts stay
 *   disabled (crt0 never enables them), so the window is the benchmark alone.
 *   Embench's support/main.c is compiled with -Dmain=embench_main, and the link
 *   wraps verify_benchmark (--wrap=verify_benchmark) to capture its result.
 *   The real main() below runs embench_main() and then prints
 *       CSV,embench,<name>,<cycles>,<instr>,<verify>,<PASS|FAIL|NOVERIFY>
 *   where <verify> is verify_benchmark()'s return (1 ok, 0 wrong, -1 none).
 *   crt0 then halts with R12 = 0 on PASS and non-zero otherwise.
 *
 * Size build (-DEMBENCH_SIZE_BUILD): empty triggers and nothing else, linked
 * with Embench's dummy libraries, as the Embench size methodology asks.
 */
#include <support.h>

#ifndef EMBENCH_SIZE_BUILD
#include <stdint.h>
#include <stdio.h>
#include "../../../mmio.h"

#ifndef EMBENCH_NAME
#define EMBENCH_NAME "unknown"
#endif

static uint64_t start_cyc, stop_cyc, start_instr, stop_instr;
static int verify_result = -2;   /* -2: verify_benchmark() never ran */
#endif

void
initialise_board (void)
{
}

void __attribute__ ((noinline))
start_trigger (void)
{
#ifndef EMBENCH_SIZE_BUILD
  start_instr = REG_PERF_INSTR;
  start_cyc = REG_PERF_CYCLES;
#endif
  __asm__ volatile ("" : : : "memory");
}

void __attribute__ ((noinline))
stop_trigger (void)
{
  __asm__ volatile ("" : : : "memory");
#ifndef EMBENCH_SIZE_BUILD
  stop_cyc = REG_PERF_CYCLES;
  stop_instr = REG_PERF_INSTR;
#endif
}

#ifndef EMBENCH_SIZE_BUILD
int __real_verify_benchmark (int res);
int embench_main (int argc, char *argv[]);

int
__wrap_verify_benchmark (int res)
{
  verify_result = __real_verify_benchmark (res);
  return verify_result;
}

int
main (void)
{
  int rc = embench_main (0, 0);
  uint64_t cyc = stop_cyc - start_cyc;
  uint64_t instr = stop_instr - start_instr;
  const char *status = verify_result == 1 ? "PASS"
                     : verify_result == -1 ? "NOVERIFY" : "FAIL";

  printf ("Embench-IoT %s: CPU_MHZ=%d cycles=%lu instr=%lu verify=%d %s\n",
          EMBENCH_NAME, CPU_MHZ, (unsigned long) cyc, (unsigned long) instr,
          verify_result, status);
  printf ("CSV,embench,%s,%lu,%lu,%d,%s\n", EMBENCH_NAME,
          (unsigned long) cyc, (unsigned long) instr, verify_result, status);
  fflush (stdout);
  return verify_result == 1 ? 0 : rc ? rc : 1;
}
#endif
