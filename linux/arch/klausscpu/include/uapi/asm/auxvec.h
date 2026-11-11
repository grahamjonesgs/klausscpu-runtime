/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_ASM_KLAUSSCPU_AUXVEC_H
#define _UAPI_ASM_KLAUSSCPU_AUXVEC_H

/*
 * The size of the D-cache line DCBZ zeroes (klaussCPU HW_PERF_PLAN.md §1),
 * or 0 when user code may not use DCBZ (an older CPU, or `nodcbz`).  The
 * number is PowerPC's.  musl's memset/memcpy read it with getauxval.
 */
#define AT_DCACHEBSIZE		19

#define AT_VECTOR_SIZE_ARCH	1	/* entries ARCH_DLINFO adds */

#endif
