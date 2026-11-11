/*
 * KlaussCPU memcpy: a doubleword at a time.  64-bit accesses must be
 * aligned, so a source at another offset within its doubleword is read as
 * aligned doublewords and shifted into place (little-endian), reading only
 * doublewords that hold source bytes.  With DCBZ (dcbz.h) each whole
 * destination line is zeroed before it is written: memcpy's buffers don't
 * overlap, so no source byte can be in a destination line.  (musl's
 * memmove calls memcpy only when they don't.)
 */
#include <string.h>
#include <stdint.h>
#include "dcbz.h"

void *memcpy(void *restrict dest, const void *restrict src, size_t n)
{
	unsigned char *d = dest;
	const unsigned char *s = src;

	if (n >= 16) {
		size_t line = __dcbz_line();
		unsigned int off;
		uint64_t *dq;

		for (; (uintptr_t)d & 7; n--)
			*d++ = *s++;
		dq = (uint64_t *)d;
		off = (uintptr_t)s & 7;
		if (line != 32 || n < 64)
			line = 0;	/* DCBZ only for 32-byte lines, as all KlaussCPUs have */
		if (!off) {
			const uint64_t *sq = (const uint64_t *)s;

			if (line) {
				for (; (uintptr_t)dq & 31; n -= 8)
					*dq++ = *sq++;
				for (; n >= 32; n -= 32, dq += 4, sq += 4) {
					uint64_t a = sq[0], b = sq[1], c = sq[2], e = sq[3];

					__dcbz(dq);
					dq[0] = a;
					dq[1] = b;
					dq[2] = c;
					dq[3] = e;
				}
			}
			for (; n >= 32; n -= 32, dq += 4, sq += 4) {
				uint64_t a = sq[0], b = sq[1], c = sq[2], e = sq[3];

				dq[0] = a;
				dq[1] = b;
				dq[2] = c;
				dq[3] = e;
			}
			for (; n >= 8; n -= 8)
				*dq++ = *sq++;
			s = (const unsigned char *)sq;
		} else {
			const uint64_t *sq = (const uint64_t *)(s - off);
			unsigned int lo = off * 8, hi = 64 - lo;
			uint64_t w = *sq++;

			if (line) {
				for (; (uintptr_t)dq & 31; n -= 8) {
					uint64_t x = *sq++;

					*dq++ = (w >> lo) | (x << hi);
					w = x;
				}
				for (; n >= 32; n -= 32, dq += 4, sq += 4) {
					uint64_t a = sq[0], b = sq[1], c = sq[2], e = sq[3];

					__dcbz(dq);
					dq[0] = (w >> lo) | (a << hi);
					dq[1] = (a >> lo) | (b << hi);
					dq[2] = (b >> lo) | (c << hi);
					dq[3] = (c >> lo) | (e << hi);
					w = e;
				}
			}
			for (; n >= 8; n -= 8) {
				uint64_t x = *sq++;

				*dq++ = (w >> lo) | (x << hi);
				w = x;
			}
			s = (const unsigned char *)sq - 8 + off;
		}
		d = (unsigned char *)dq;
	}
	while (n--)
		*d++ = *s++;
	return dest;
}
