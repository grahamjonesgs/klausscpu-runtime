// SPDX-License-Identifier: GPL-2.0
/*
 * sysloop MODE N: N iterations of one kernel-entry microbenchmark, for
 * perfstat -d N (per-iteration counts) or on its own (ns per iteration):
 *   loop    an empty loop (the baseline)
 *   enosys  syscall(-1): entry, the table bounds check and return
 *   getppid the lmbench "null" system call
 *   tlb     a pointer chase through 32 pages, one page apart (every access a
 *           TLB refill with a 16-entry TLB, the data lines cached)
 *   fault   touch a fresh anonymous page, then unmap it (page fault, zeroing
 *           64 KiB, munmap and the TLB flush)
 *   fork    fork, the child exits at once, wait for it
 *   exec    fork, the child execs PROGRAM (argument 3, default /bin/true),
 *           wait for it
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
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
	long n = argc > 2 ? atol(argv[2]) : 100000, i;
	const char *mode = argc > 1 ? argv[1] : "getppid";
	long page = sysconf(_SC_PAGESIZE);
	volatile long sink = 0;
	double t = now();

	if (!strcmp(mode, "loop")) {
		for (i = 0; i < n; i++)
			sink += i;
	} else if (!strcmp(mode, "enosys")) {
		for (i = 0; i < n; i++)
			sink += syscall(-1);
	} else if (!strcmp(mode, "getppid")) {
		for (i = 0; i < n; i++)
			sink += syscall(SYS_getppid);
	} else if (!strcmp(mode, "tlb")) {
		const int pages = 32;
		char *mem = mmap(NULL, (size_t)pages * page, PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		void **p;

		if (mem == MAP_FAILED)
			return 1;
		/* Page k's word (at line k) points to page (k + 13) % 32's: one cycle through all 32. */
		for (i = 0; i < pages; i++) {
			long j = (i + 13) % pages;

			*(void **)(mem + i * page + i * 64) = mem + j * page + j * 64;
		}
		p = (void **)mem;
		for (i = 0; i < pages * 2; i++)
			p = *p;
		t = now();
		for (i = 0; i < n; i++)
			p = *p;
		sink = (long)p;
	} else if (!strcmp(mode, "fault")) {
		for (i = 0; i < n; i++) {
			char *m = mmap(NULL, page, PROT_READ | PROT_WRITE,
				       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

			if (m == MAP_FAILED)
				return 1;
			m[0] = 1;
			munmap(m, page);
		}
	} else if (!strcmp(mode, "fork") || !strcmp(mode, "exec")) {
		const char *prog = argc > 3 ? argv[3] : "/bin/true";
		int ex = !strcmp(mode, "exec");

		for (i = 0; i < n; i++) {
			pid_t pid = fork();

			if (pid == 0) {
				if (ex)
					execl(prog, prog, (char *)NULL);
				_exit(0);
			}
			if (pid < 0 || waitpid(pid, NULL, 0) < 0)
				return 1;
		}
	} else {
		fprintf(stderr, "usage: sysloop loop|enosys|getppid|tlb|fault|fork|exec [N [PROGRAM]]\n");
		return 2;
	}
	t = now() - t;
	printf("sysloop %s: %ld iterations, %.1f ns each\n", mode, n, t / n);
	return 0;
}
