/*
 * KlaussCPU memset: a doubleword at a time (64-bit stores must be
 * aligned), four per pass, and with DCBZ (dcbz.h) each whole destination
 * line is zeroed first, so it is not fetched only to be overwritten.
 */
#include <string.h>
#include <stdint.h>
#include "dcbz.h"

void *memset(void *dest, int c, size_t n)
{
	unsigned char *p = dest;

	if (n >= 16) {
		uint64_t v = (unsigned char)c, *q;
		size_t line = __dcbz_line();

		v |= v << 8;
		v |= v << 16;
		v |= v << 32;
		for (; (uintptr_t)p & 7; n--)
			*p++ = c;
		q = (uint64_t *)p;
		if (line == 32 && n >= 64) {
			for (; (uintptr_t)q & 31; n -= 8)
				*q++ = v;
			for (; n >= 32; n -= 32, q += 4) {
				__dcbz(q);
				if (v) {
					q[0] = v;
					q[1] = v;
					q[2] = v;
					q[3] = v;
				}
			}
		} else if (line && n >= 2 * line) {
			size_t i;

			for (; (uintptr_t)q & (line - 1); n -= 8)
				*q++ = v;
			for (; n >= line; n -= line, q += line / 8) {
				__dcbz(q);
				if (v)
					for (i = 0; i < line / 8; i++)
						q[i] = v;
			}
		}
		for (; n >= 32; n -= 32, q += 4) {
			q[0] = v;
			q[1] = v;
			q[2] = v;
			q[3] = v;
		}
		for (; n >= 8; n -= 8)
			*q++ = v;
		p = (unsigned char *)q;
	}
	while (n--)
		*p++ = c;
	return dest;
}
