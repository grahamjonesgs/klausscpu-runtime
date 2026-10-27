/*
 * core_portme.c — CoreMark port for the KlaussCPU bare-metal runtime.
 *
 * Follows the structure of CoreMark's barebones/core_portme.c (EEMBC,
 * Apache-2.0).  The upstream CoreMark sources are fetched by
 * ../get-benchmarks.sh and are not part of this repo.
 *
 *   Timing  : REG_PERF_CYCLES (64-bit core cycle counter, 0xF00D0008), read
 *             in start_time()/stop_time().  Interrupts stay disabled (crt0
 *             never enables them), so the window is pure benchmark code.
 *   Output  : CoreMark's ee_printf is picolibc printf (HAS_PRINTF), which
 *             writes to the UART through src/stdio_handles.c.
 *   Seeds   : SEED_VOLATILE.  PERFORMANCE_RUN (default) uses 0,0,0x66 and
 *             VALIDATION_RUN uses 0x3415,0x3415,0x66 — both have reference
 *             CRCs that core_main.c checks.  ITERATIONS is set by the Makefile
 *             (COREMARK_ITERATIONS); 0 asks core_main.c to auto-size the run
 *             to >= 10 s.
 *   Exit    : portable_fini() prints a summary (cycles, instructions,
 *             CoreMark/MHz) and a CSV line, then halts with R12 = 0 on a
 *             valid run, 1 if a CRC check failed, 2 if the CRCs matched but
 *             the timed run was shorter than 10 s.
 */
#include <stdio.h>
#include <stdlib.h>
#include "coremark.h"
#include "core_portme.h"
#include "../../../mmio.h"

#ifndef ITERATIONS
#define ITERATIONS 0
#endif

#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

#define EE_TICKS_PER_SEC KLAUSSCPU_CLOCK_HZ

static CORE_TICKS start_time_val, stop_time_val;
static uint64_t   start_instr_val, stop_instr_val;

void start_time(void) {
    start_instr_val = REG_PERF_INSTR;
    start_time_val  = REG_PERF_CYCLES;
}

void stop_time(void) {
    stop_time_val  = REG_PERF_CYCLES;
    stop_instr_val = REG_PERF_INSTR;
}

CORE_TICKS get_time(void) {
    return stop_time_val - start_time_val;
}

secs_ret time_in_secs(CORE_TICKS ticks) {
#if HAS_FLOAT
    return (secs_ret)ticks / (secs_ret)EE_TICKS_PER_SEC;
#else
    return (secs_ret)(ticks / EE_TICKS_PER_SEC);
#endif
}

ee_u32 default_num_contexts = 1;

void portable_init(core_portable *p, int *argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (sizeof(ee_ptr_int) != sizeof(ee_u8 *))
        ee_printf("ERROR! Please define ee_ptr_int to a type that holds a pointer!\n");
    if (sizeof(ee_u32) != 4)
        ee_printf("ERROR! Please define ee_u32 to a 32b unsigned type!\n");
    p->portable_id = 1;
}

/* Print a thousandths fixed-point value. */
static void milli(uint64_t v) {
    printf("%lu.%03lu", (unsigned long)(v / 1000), (unsigned long)(v % 1000));
}

void portable_fini(core_portable *p) {
    /* core_main.c passes &results[0].port; recover results[0] for the
     * iteration count and the CRC error tally it has just computed. */
    core_results *r = (core_results *)((char *)p - offsetof(core_results, port));
    uint64_t cyc   = stop_time_val - start_time_val;
    uint64_t instr = stop_instr_val - start_instr_val;
    uint64_t it    = r->iterations;
    uint64_t mhz   = EE_TICKS_PER_SEC / 1000000u;

    p->portable_id = 0;
    if (cyc == 0)
        cyc = 1;
    /* CoreMark/MHz = iterations / (cycles / 1e6); in thousandths. */
    uint64_t cm_mhz_milli = (it * 1000000000ull) / cyc;
    uint64_t cm_milli     = cm_mhz_milli * mhz;
    uint64_t cpi_milli    = instr ? (cyc * 1000ull) / instr : 0;

    printf("KlaussCPU cycles : %lu\n", (unsigned long)cyc);
    printf("KlaussCPU instr  : %lu (CPI ", (unsigned long)instr);
    milli(cpi_milli);
    printf(")\n");
    printf("CoreMark/MHz     : ");
    milli(cm_mhz_milli);
    printf(" @ %lu MHz\n", (unsigned long)mhz);
    /* FAIL: a CRC differs from the reference for these seeds.  SHORT: the
     * CRCs match but the timed run was under the 10 s the run rules ask. */
    int short_run = cyc < 10ull * EE_TICKS_PER_SEC;
    const char *status = r->err ? "FAIL" : short_run ? "SHORT" : "PASS";
    printf("CSV,coremark,%lu,%lu,%lu,%lu,%lu,%s\n",
           (unsigned long)it, (unsigned long)cyc, (unsigned long)instr,
           (unsigned long)cm_milli, (unsigned long)cm_mhz_milli, status);
    fflush(stdout);
    if (r->err)
        exit(1);
    if (short_run)
        exit(2);
}
