/*
 * dummy-libklausscpu.c — compiler-rt stubs for the Embench-IoT size build.
 *
 * Embench measures code size with dummy versions of every library (see
 * embench-iot/support/dummy-*.c).  Its dummy-libgcc.c only covers Arm's
 * __aeabi_* helpers; this is the KlaussCPU equivalent for the compiler-rt
 * soft-float and integer helpers our code calls.  Each stub is a trivial
 * function so the benchmark links; with --gc-sections only the ones a
 * benchmark references are kept.  Never run these.
 */

#define STUB(name) long name (void) { return 0; }

/* single precision */
STUB (__addsf3) STUB (__subsf3) STUB (__mulsf3) STUB (__divsf3)
STUB (__negsf2) STUB (__eqsf2) STUB (__nesf2) STUB (__ltsf2) STUB (__lesf2)
STUB (__gtsf2) STUB (__gesf2) STUB (__unordsf2)
STUB (__floatsisf) STUB (__floatunsisf) STUB (__floatdisf) STUB (__floatundisf)
STUB (__fixsfsi) STUB (__fixsfdi) STUB (__fixunssfsi) STUB (__fixunssfdi)
/* double precision */
STUB (__adddf3) STUB (__subdf3) STUB (__muldf3) STUB (__divdf3)
STUB (__negdf2) STUB (__eqdf2) STUB (__nedf2) STUB (__ltdf2) STUB (__ledf2)
STUB (__gtdf2) STUB (__gedf2) STUB (__unorddf2)
STUB (__floatsidf) STUB (__floatunsidf) STUB (__floatdidf) STUB (__floatundidf)
STUB (__fixdfsi) STUB (__fixdfdi) STUB (__fixunsdfsi) STUB (__fixunsdfdi)
STUB (__extendsfdf2) STUB (__truncdfsf2)
/* integer */
STUB (__udivsi3) STUB (__divsi3) STUB (__umodsi3) STUB (__modsi3)
STUB (__udivdi3) STUB (__divdi3) STUB (__umoddi3) STUB (__moddi3)
STUB (__udivmoddi4) STUB (__udivti3) STUB (__umodti3) STUB (__udivmodti4)
STUB (__multi3) STUB (__muldi3)

/* KlaussCPU has no FPU and clang emits fabs/fabsf as libm calls (Arm GCC
 * inlines them, so Embench's dummy-libm.c lacks them). */
STUB (fabs) STUB (fabsf)

/* picolibc's <ctype.h> macros index this table (newlib's is _ctype_, which
 * dummy-libc.c provides).  Read-only data: not part of the .text metric. */
const char _ctype_b[257];
