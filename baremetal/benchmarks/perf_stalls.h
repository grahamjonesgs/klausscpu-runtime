/* perf_stalls.h -- with -DPERF_STALLS, the benchmark ports print the core's
 * per-cause cycle counters (KlaussCPU.sv, MMIO 0xF00D_00B0-0xF8) over the
 * timed region, after their result line.  Off by default: a plain build reads
 * only REG_PERF_CYCLES / REG_PERF_INSTR, as before.
 *
 *   tools/validate/bench.sh <outdir> "-O2 -DPERF_STALLS"
 *
 * PERF_STALLS_START() sets PERF_CTRL[1], the exclusive mode (klaussCPU
 * e0bdc66 and later; older cores ignore the write): each cycle goes to at most
 * one bucket, by priority memwait > exbusy > data > flags > sp > ifmiss, so
 * those buckets plus the instructions add up to about the cycle count.
 * Load-use stays a subset of data; redirect and icmiss count events.  The bit
 * persists until the next PERF_CTRL write or power-off.  On an older core the
 * buckets overlap and do not add up. */
#ifndef PERF_STALLS_H
#define PERF_STALLS_H

#ifdef PERF_STALLS
#include <stdint.h>
#include <stdio.h>

#define PERF_STALL_N 10
static const char *const perf_stall_name[PERF_STALL_N] = {
    "data",      /* 0xB0 GPR RAW stall cycles */
    "loaduse",   /* 0xB8 load-use subset of data */
    "flags",     /* 0xC0 flag-reader stall cycles */
    "sp",        /* 0xC8 SP-serialisation stall cycles */
    "exbusy",    /* 0xD0 EX busy (mul/div/delay/lcd) */
    "redirect",  /* 0xD8 taken-redirect events (not cycles) */
    "ifmiss",    /* 0xE0 fetch-window miss cycles */
    "memwait",   /* 0xE8 MEM port wait cycles */
    "icmiss",    /* 0xF0 I-cache miss fills (events) */
    "icfill",    /* 0xF8 I-cache fill-in-flight cycles */
};
static uint64_t perf_stall_start[PERF_STALL_N];
static uint64_t perf_stall_stop[PERF_STALL_N];

static inline void perf_stall_read(uint64_t *v)
{
    for (int i = 0; i < PERF_STALL_N; i++)
        v[i] = *(volatile uint64_t *)(unsigned long)(0xF00D00B0u + 8u * i);
}

static void perf_stall_print(uint64_t cycles)
{
    printf("STALLS cycles=%lu", (unsigned long)cycles);
    for (int i = 0; i < PERF_STALL_N; i++)
        printf(" %s=%lu", perf_stall_name[i],
               (unsigned long)(perf_stall_stop[i] - perf_stall_start[i]));
    printf("\n");
}

#define PERF_STALLS_START() \
    (*(volatile uint32_t *)(unsigned long)0xF00D0000u = 2u, \
     perf_stall_read(perf_stall_start))
#define PERF_STALLS_STOP()  perf_stall_read(perf_stall_stop)
#define PERF_STALLS_PRINT(cycles) perf_stall_print(cycles)
#else
#define PERF_STALLS_START() ((void)0)
#define PERF_STALLS_STOP()  ((void)0)
#define PERF_STALLS_PRINT(cycles) ((void)0)
#endif

#endif
