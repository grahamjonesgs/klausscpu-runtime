// SPDX-License-Identifier: GPL-2.0
/*
 * strtest [-b]: musl's memcpy, memset and memmove (linux/musl's KlaussCPU
 * versions, with DCBZ when the kernel offers it in AT_DCACHEBSIZE) against
 * byte-loop references: every size 0..300 at every source and destination
 * offset 0..15, overlapping memmoves both ways, and 64 KiB + odd copies at
 * every source offset; guard bytes either side must not change.  -b also
 * prints memset/memcpy bandwidth over 4 MiB.  Prints "strtest: PASS".
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <time.h>

#define BIG	(64 * 1024 + 77)
#define GUARD	64

static unsigned char *a, *b, *ref;
static int fails;

static void fill(unsigned char *p, size_t n, unsigned seed)
{
	for (size_t i = 0; i < n; i++)
		p[i] = (unsigned char)(seed * 131 + i * 7 + (i >> 8));
}

static void check(const char *what, size_t n, int doff, int soff, size_t len)
{
	if (memcmp(a, ref, len)) {
		size_t i = 0;

		while (a[i] == ref[i])
			i++;
		if (fails++ < 10)
			printf("strtest: %s n=%zu dst+%d src+%d: byte %zu is %02x, want %02x\n",
			       what, n, doff, soff, i, a[i], ref[i]);
	}
}

/* The references never use the functions under test. */
static void refcpy(unsigned char *d, const unsigned char *s, size_t n)
{
	for (volatile size_t i = 0; i < n; i++)
		d[i] = s[i];
}

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	size_t len = BIG + 2 * GUARD + 64;
	unsigned long line = getauxval(AT_DCACHEBSIZE);

	a = malloc(len);
	b = malloc(len);
	ref = malloc(len);
	if (!a || !b || !ref)
		return 1;
	printf("strtest: AT_DCACHEBSIZE %lu (%s)\n", line, line ? "DCBZ" : "no DCBZ");
	for (size_t n = 0; n <= 300; n++)
		for (int doff = 0; doff < 16; doff++)
			for (int soff = 0; soff < 16; soff++) {
				size_t w = GUARD + 32 + n + 64;

				fill(a, w, 1);
				fill(b, w, 2);
				refcpy(ref, a, w);
				for (size_t i = 0; i < n; i++)
					ref[GUARD + doff + i] = b[GUARD + soff + i];
				memcpy(a + GUARD + doff, b + GUARD + soff, n);
				check("memcpy", n, doff, soff, w);
				if (soff == 0) {
					fill(a, w, 3);
					refcpy(ref, a, w);
					for (size_t i = 0; i < n; i++)
						ref[GUARD + doff + i] = (unsigned char)(n + doff);
					memset(a + GUARD + doff, n + doff, n);
					check("memset", n, doff, 0, w);
				}
				/* Overlapping memmove, both directions. */
				fill(a, w + 16, 4);
				{
					unsigned char tmp[400];

					refcpy(tmp, a + GUARD + soff, n);
					refcpy(ref, a, w + 16);
					refcpy(ref + GUARD + doff, tmp, n);
				}
				memmove(a + GUARD + doff, a + GUARD + soff, n);
				check("memmove", n, doff, soff, w + 16);
			}
	for (int soff = 0; soff < 8; soff++)
		for (int doff = 0; doff < 2; doff++) {
			size_t n = BIG - 13;

			fill(a, len, 5);
			fill(b, len, 6);
			refcpy(ref, a, len);
			for (size_t i = 0; i < n; i++)
				ref[GUARD + doff + i] = b[GUARD + soff + i];
			memcpy(a + GUARD + doff, b + GUARD + soff, n);
			check("memcpy 64K", n, doff, soff, len);
			fill(a, len, 7);
			refcpy(ref, a, len);
			for (size_t i = 0; i < n; i++)
				ref[GUARD + soff + i] = 0;
			memset(a + GUARD + soff, 0, n);
			check("memset 64K", n, 0, soff, len);
		}
	if (argc > 1 && !strcmp(argv[1], "-b")) {
		size_t sz = 4 << 20;
		unsigned char *x = malloc(sz), *y = malloc(sz);
		double t;
		int reps = 8;

		if (!x || !y)
			return 1;
		memset(x, 1, sz);
		memset(y, 2, sz);
		t = now();
		/* The barrier keeps the compiler from dropping stores nothing reads. */
		for (int r = 0; r < reps; r++) {
			memset(x, r, sz);
			__asm__ __volatile__ ("" : : "r"(x) : "memory");
		}
		printf("strtest: memset %.1f MB/s\n", reps * sz / (now() - t) / 1e6);
		t = now();
		for (int r = 0; r < reps; r++) {
			memcpy(r & 1 ? x : y, r & 1 ? y : x, sz);
			__asm__ __volatile__ ("" : : "r"(x), "r"(y) : "memory");
		}
		printf("strtest: memcpy %.1f MB/s\n", reps * sz / (now() - t) / 1e6);
	}
	printf("strtest: %s\n", fails ? "FAIL" : "PASS");
	return fails != 0;
}
