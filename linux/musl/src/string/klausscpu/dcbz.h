/*
 * KlaussCPU DCBZ for memset/memcpy (klaussCPU HW_PERF_PLAN.md §1): zero
 * the whole D-cache line holding an address without the cache fetching it
 * first, so a run of whole destination lines is written without reading
 * each from DDR only to overwrite it.  The kernel passes the line size in
 * AT_DCACHEBSIZE, 0 when user code may not use it (an older CPU, or the
 * kernel's `nodcbz`).
 *
 * The aux vector is read directly, not with getauxval: memcpy runs in
 * __init_libc/__init_tls before TLS exists, and getauxval sets errno (a
 * TLS variable) when the entry is missing.  Until libc.auxv is set the
 * answer is 0 and is not cached.
 */
#include <elf.h>
#include <stddef.h>
#include "libc.h"

static inline size_t __dcbz_line(void)
{
	static size_t line = (size_t)-1;
	size_t l = line, *a = libc.auxv;

	if (l != (size_t)-1)
		return l;
	if (!a)
		return 0;
	for (l = 0; *a; a += 2)
		if (*a == AT_DCACHEBSIZE) {
			l = a[1];
			break;
		}
	if (l < 16 || (l & (l - 1)))
		l = 0;
	line = l;
	return l;
}

static inline void __dcbz(void *p)
{
	__asm__ __volatile__ ("dcbz %0" : : "r"(p) : "memory");
}
