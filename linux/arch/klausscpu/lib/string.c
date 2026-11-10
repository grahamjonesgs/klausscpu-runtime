// SPDX-License-Identifier: GPL-2.0
/*
 * memset, memcpy and memmove a doubleword at a time (lib/string.c's are a
 * byte at a time: clear_page and copy_page, a 64 KiB page each, use these).
 *
 * KlaussCPU's 64-bit accesses must be 8-byte aligned (CPU_ARCHITECTURE.md
 * §8.3), so: the head up to an aligned destination goes a byte at a time,
 * then whole doublewords, four (one 32-byte cache line) per loop pass.  A
 * source at another offset within its doubleword is read as aligned
 * doublewords and shifted into place (little-endian), reading nothing
 * outside the doublewords that hold source bytes.
 *
 * Built with -ffreestanding (no loop turned back into a memset/memcpy call).
 */
#include <linux/export.h>
#include <linux/string.h>
#include <linux/types.h>

#define WORD	8UL

void *memset(void *s, int c, size_t n)
{
	unsigned char *p = s;

	if (n >= 2 * WORD) {
		u64 v = (u8)c, *q;

		v |= v << 8;
		v |= v << 16;
		v |= v << 32;
		for (; (unsigned long)p & (WORD - 1); n--)
			*p++ = c;
		q = (u64 *)p;
		for (; n >= 4 * WORD; n -= 4 * WORD, q += 4) {
			q[0] = v;
			q[1] = v;
			q[2] = v;
			q[3] = v;
		}
		for (; n >= WORD; n -= WORD)
			*q++ = v;
		p = (unsigned char *)q;
	}
	while (n--)
		*p++ = c;
	return s;
}
EXPORT_SYMBOL(memset);

/* Forward copy; also correct for an overlapping copy to a lower address. */
static void copy_forward(unsigned char *d, const unsigned char *s, size_t n)
{
	if (n >= 2 * WORD) {
		unsigned int off;
		u64 *dq;

		for (; (unsigned long)d & (WORD - 1); n--)
			*d++ = *s++;
		dq = (u64 *)d;
		off = (unsigned long)s & (WORD - 1);
		if (!off) {
			const u64 *sq = (const u64 *)s;

			for (; n >= 4 * WORD; n -= 4 * WORD, dq += 4, sq += 4) {
				u64 a = sq[0], b = sq[1], c = sq[2], e = sq[3];

				dq[0] = a;
				dq[1] = b;
				dq[2] = c;
				dq[3] = e;
			}
			for (; n >= WORD; n -= WORD)
				*dq++ = *sq++;
			s = (const unsigned char *)sq;
		} else {
			/* The doubleword holding s, then the next: each output
			 * doubleword takes the top of one and the bottom of
			 * the next.  Only doublewords with source bytes in them
			 * are read: while n >= 8 the next one has some. */
			const u64 *sq = (const u64 *)(s - off);
			unsigned int lo = off * 8, hi = 64 - lo;
			u64 w = *sq++;

			for (; n >= WORD; n -= WORD) {
				u64 x = *sq++;

				*dq++ = (w >> lo) | (x << hi);
				w = x;
			}
			s = (const unsigned char *)sq - WORD + off;
		}
		d = (unsigned char *)dq;
	}
	while (n--)
		*d++ = *s++;
}

void *memcpy(void *dest, const void *src, size_t n)
{
	copy_forward(dest, src, n);
	return dest;
}
EXPORT_SYMBOL(memcpy);

void *memmove(void *dest, const void *src, size_t n)
{
	unsigned char *d = dest;
	const unsigned char *s = src;

	if (d <= s || d >= s + n) {
		copy_forward(d, s, n);
		return dest;
	}
	/* Backward: doublewords when both ends share their alignment. */
	d += n;
	s += n;
	if (n >= 2 * WORD && !(((unsigned long)d ^ (unsigned long)s) & (WORD - 1))) {
		u64 *dq;
		const u64 *sq;

		for (; (unsigned long)d & (WORD - 1); n--)
			*--d = *--s;
		dq = (u64 *)d;
		sq = (const u64 *)s;
		for (; n >= WORD; n -= WORD)
			*--dq = *--sq;
		d = (unsigned char *)dq;
		s = (const unsigned char *)sq;
	}
	while (n--)
		*--d = *--s;
	return dest;
}
EXPORT_SYMBOL(memmove);
