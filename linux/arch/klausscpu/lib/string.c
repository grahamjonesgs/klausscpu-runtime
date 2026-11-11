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
 * With DCBZ (klaussCPU HW_PERF_PLAN.md §1, CPU_FEATURES[22]) a destination
 * run of whole D-cache lines is zeroed a line at a time without the cache
 * fetching it first, then written: a write miss otherwise reads the line
 * from DDR only to overwrite it (97% of fork's write misses).  Only for
 * RAM (memset_io/memcpy_toio may reach these with MMIO addresses, where
 * DCBZ faults) and never for an overlapping copy (zeroing the destination
 * line could destroy source bytes not yet read).  `nodcbz` on the command
 * line turns it off.
 *
 * Built with -ffreestanding (no loop turned back into a memset/memcpy call).
 */
#include <linux/export.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/log2.h>
#include <linux/printk.h>
#include <linux/string.h>
#include <linux/types.h>
#include <asm/klausscpu.h>

#define WORD	8UL

/* The D-cache line DCBZ zeroes, or 0 without DCBZ (klausscpu_dcbz_init). */
static unsigned long klausscpu_dcbz_line;

static inline void dcbz(void *p)
{
	asm volatile("dcbz %0" : : "r"(p) : "memory");
}

/* Whether [p, p + n) may be zeroed by DCBZ: there is DCBZ, and it is RAM. */
static inline bool dcbz_ok(const void *p, size_t n)
{
	return klausscpu_dcbz_line && (unsigned long)p + n <= MMIO_BASE &&
	       n >= 2 * klausscpu_dcbz_line;
}

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
		if (dcbz_ok(q, n)) {
			unsigned long line = klausscpu_dcbz_line, i;

			for (; ((unsigned long)q & (line - 1)); n -= WORD)
				*q++ = v;
			if (line == 4 * WORD) {		/* every bitstream so far */
				for (; n >= 4 * WORD; n -= 4 * WORD, q += 4) {
					dcbz(q);
					if (v) {
						q[0] = v;
						q[1] = v;
						q[2] = v;
						q[3] = v;
					}
				}
			}
			for (; n >= line; n -= line, q += line / WORD) {
				dcbz(q);
				if (v)
					for (i = 0; i < line / WORD; i++)
						q[i] = v;
			}
		}
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

/*
 * Forward copy; also correct for an overlapping copy to a lower address
 * when !may_zero (DCBZ would zero destination bytes that are source).
 */
static void copy_forward(unsigned char *d, const unsigned char *s, size_t n,
			 bool may_zero)
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

			if (may_zero && dcbz_ok(dq, n)) {
				unsigned long line = klausscpu_dcbz_line, i;

				for (; ((unsigned long)dq & (line - 1)); n -= WORD)
					*dq++ = *sq++;
				if (line == 4 * WORD) {
					for (; n >= 4 * WORD; n -= 4 * WORD, dq += 4, sq += 4) {
						u64 a = sq[0], b = sq[1], c = sq[2], e = sq[3];

						dcbz(dq);
						dq[0] = a;
						dq[1] = b;
						dq[2] = c;
						dq[3] = e;
					}
				}
				for (; n >= line; n -= line, dq += line / WORD, sq += line / WORD) {
					dcbz(dq);
					for (i = 0; i < line / WORD; i++)
						dq[i] = sq[i];
				}
			}
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

			if (may_zero && dcbz_ok(dq, n)) {
				unsigned long line = klausscpu_dcbz_line, i;

				for (; ((unsigned long)dq & (line - 1)); n -= WORD) {
					u64 x = *sq++;

					*dq++ = (w >> lo) | (x << hi);
					w = x;
				}
				if (line == 4 * WORD) {
					for (; n >= 4 * WORD; n -= 4 * WORD, dq += 4, sq += 4) {
						u64 a = sq[0], b = sq[1], c = sq[2], e = sq[3];

						dcbz(dq);
						dq[0] = (w >> lo) | (a << hi);
						dq[1] = (a >> lo) | (b << hi);
						dq[2] = (b >> lo) | (c << hi);
						dq[3] = (c >> lo) | (e << hi);
						w = e;
					}
				}
				for (; n >= line; n -= line) {
					dcbz(dq);
					for (i = 0; i < line / WORD; i++) {
						u64 x = *sq++;

						*dq++ = (w >> lo) | (x << hi);
						w = x;
					}
				}
			}
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
	copy_forward(dest, src, n, true);
	return dest;
}
EXPORT_SYMBOL(memcpy);

void *memmove(void *dest, const void *src, size_t n)
{
	unsigned char *d = dest;
	const unsigned char *s = src;

	if (d + n <= s || d >= s + n) {
		copy_forward(d, s, n, true);	/* no overlap */
		return dest;
	}
	if (d <= s) {
		copy_forward(d, s, n, false);
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

#define CPU_FEAT_DCBZ	(1UL << 22)	/* HW_PERF_PLAN.md §1 */
#define CACHE_INFO	0x08		/* [31:24]: the D-cache line size */

extern unsigned long klausscpu_cpu_features;
static bool nodcbz __initdata;

static int __init set_nodcbz(char *arg)
{
	nodcbz = true;
	return 0;
}
early_param("nodcbz", set_nodcbz);

/*
 * After setup_arch has read CPU_FEATURES; until then the copies go without
 * DCBZ.  The line size is CACHE_INFO's, never assumed.
 */
static int __init klausscpu_dcbz_init(void)
{
	unsigned long line;

	if (!(klausscpu_cpu_features & CPU_FEAT_DCBZ))
		return 0;
	line = (readq((void __iomem *)(KL_CACHE_BASE + CACHE_INFO)) >> 24) & 0xff;
	if (nodcbz || line < 2 * WORD || !is_power_of_2(line)) {
		pr_info("KlaussCPU: DCBZ off%s\n", nodcbz ? " (nodcbz)" : " (no line size)");
		return 0;
	}
	klausscpu_dcbz_line = line;
	pr_info("KlaussCPU: DCBZ on, %lu-byte lines\n", line);
	return 0;
}
early_initcall(klausscpu_dcbz_init);
