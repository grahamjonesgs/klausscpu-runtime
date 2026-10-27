/*
 * core_portme.h — CoreMark port for the KlaussCPU bare-metal runtime.
 *
 * Follows the structure of CoreMark's barebones/core_portme.h (EEMBC,
 * Apache-2.0).  The upstream CoreMark sources are fetched by
 * ../get-benchmarks.sh and are not part of this repo.
 *
 * KlaussCPU is LP64 (int 32-bit, long and pointers 64-bit), so ee_ptr_int is
 * uintptr_t and ee_u32 is unsigned int.  Timing uses the 64-bit hardware cycle
 * counter REG_PERF_CYCLES; one tick = one 100 MHz core clock cycle.
 */
#ifndef CORE_PORTME_H
#define CORE_PORTME_H

#include <stddef.h>
#include <stdint.h>

/* Soft-float doubles (compiler-rt) — used only for the time/score report. */
#ifndef HAS_FLOAT
#define HAS_FLOAT 1
#endif
#ifndef HAS_TIME_H
#define HAS_TIME_H 0
#endif
#ifndef USE_CLOCK
#define USE_CLOCK 0
#endif
/* picolibc stdio over the runtime's UART (src/stdio_handles.c). */
#ifndef HAS_STDIO
#define HAS_STDIO 1
#endif
#ifndef HAS_PRINTF
#define HAS_PRINTF 1
#endif

#ifndef COMPILER_VERSION
#ifdef __clang__
#define COMPILER_VERSION "Clang " __clang_version__
#else
#define COMPILER_VERSION "unknown"
#endif
#endif
#ifndef COMPILER_FLAGS
#define COMPILER_FLAGS FLAGS_STR
#endif
#ifndef MEM_LOCATION
#define MEM_LOCATION "STACK"
#endif

typedef signed short   ee_s16;
typedef unsigned short ee_u16;
typedef signed int     ee_s32;
typedef double         ee_f32;
typedef unsigned char  ee_u8;
typedef unsigned int   ee_u32;
typedef uintptr_t      ee_ptr_int;
typedef size_t         ee_size_t;

/* Align an offset to point to a 32-bit value (matrix init). */
#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x) - 1) & ~3))

/* Ticks are raw core clock cycles (64-bit counter, never wraps in practice). */
#define CORETIMETYPE uint64_t
typedef uint64_t CORE_TICKS;

/* Core clock: the MIG ui_clk, 100 MHz (klaussCPU README / MEM_CLOCK_PLAN). */
#ifndef KLAUSSCPU_CLOCK_HZ
#define KLAUSSCPU_CLOCK_HZ 100000000u
#endif

#ifndef SEED_METHOD
#define SEED_METHOD SEED_VOLATILE
#endif
#ifndef MEM_METHOD
#define MEM_METHOD MEM_STACK
#endif

#ifndef MULTITHREAD
#define MULTITHREAD 1
#define USE_PTHREAD 0
#define USE_FORK    0
#define USE_SOCKET  0
#endif

/* crt0 calls main(0, NULL) and halts with its return value in R12. */
#ifndef MAIN_HAS_NOARGC
#define MAIN_HAS_NOARGC 1
#endif
#ifndef MAIN_HAS_NORETURN
#define MAIN_HAS_NORETURN 0
#endif

extern ee_u32 default_num_contexts;

typedef struct CORE_PORTABLE_S {
    ee_u8 portable_id;
} core_portable;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);

#if !defined(PROFILE_RUN) && !defined(PERFORMANCE_RUN) && !defined(VALIDATION_RUN)
#if (TOTAL_DATA_SIZE == 1200)
#define PROFILE_RUN 1
#elif (TOTAL_DATA_SIZE == 2000)
#define PERFORMANCE_RUN 1
#else
#define VALIDATION_RUN 1
#endif
#endif

#endif /* CORE_PORTME_H */
