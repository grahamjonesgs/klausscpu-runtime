// SPDX-License-Identifier: GPL-2.0
/*
 * tlbmiss [max pages] [accesses]: TLB-miss cost.  For N = 1, 2, 4 ... max
 * pages (default 64), a pointer chain visits one word in each of N pages,
 * one page apart (the page size: 64 KiB on KlaussCPU), in a random order so
 * that no prefetch helps.  The word's line within its page varies, so the
 * data cache doesn't alias them, and N pages of lines stay cached: past the
 * TLB's reach (16 entries x 64 KiB on KlaussCPU) each access costs a refill.
 * Prints "pages N  ns/access T" per N.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e9 + t.tv_nsec;
}

int main(int argc, char **argv)
{
	long page = sysconf(_SC_PAGESIZE);
	int max = argc > 1 ? atoi(argv[1]) : 64;
	long accesses = argc > 2 ? atol(argv[2]) : 2000000;
	unsigned seed = 12345;

	printf("tlbmiss: page %ld bytes, %ld accesses per size\n", page, accesses);
	for (int n = 1; n <= max; n *= 2) {
		char *mem = mmap(NULL, (size_t)n * page, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		int *order = malloc(n * sizeof(*order));
		void **p;
		double t;
		long i;

		if (mem == MAP_FAILED || !order) {
			perror("tlbmiss");
			return 1;
		}
		for (i = 0; i < n; i++)
			order[i] = i;
		for (i = n - 1; i > 0; i--) {	/* a random cycle through the pages */
			int j = (seed = seed * 1103515245 + 12345) % (i + 1), x = order[i];

			order[i] = order[j];
			order[j] = x;
		}
		for (i = 0; i < n; i++) {
			void **from = (void **)(mem + (size_t)order[i] * page + (order[i] % 64) * 64);
			void **to = (void **)(mem + (size_t)order[(i + 1) % n] * page +
					      (order[(i + 1) % n] % 64) * 64);
			*from = to;
		}
		p = (void **)(mem + (size_t)order[0] * page + (order[0] % 64) * 64);
		for (i = 0; i < n * 4; i++)	/* warm the caches and the TLB */
			p = *p;
		t = now();
		for (i = 0; i < accesses; i += 8) {
			p = *p; p = *p; p = *p; p = *p;
			p = *p; p = *p; p = *p; p = *p;
		}
		t = now() - t;
		printf("pages %4d  ns/access %8.2f%s\n", n, t / accesses, p ? "" : " ");
		munmap(mem, (size_t)n * page);
		free(order);
	}
	return 0;
}
