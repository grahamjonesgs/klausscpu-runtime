// SPDX-License-Identifier: GPL-2.0
/*
 * dmaexec FILE: new code arriving by DMA into an executable mapping that
 * has already run (the SD DMA review's lazy I-cache case).  A private
 * RWX anonymous page gets a copy of code_a and runs it; then an O_DIRECT
 * read of FILE (code_b, written there by the first run with "dmaexec -w
 * FILE") puts new code into the same page by DMA, and it runs again.
 *
 * The page stays mapped throughout, so set_ptes() never sees it again:
 * the kernel must drop its I-cache lines when the DMA completes.  The
 * emulator has no I-cache, so the program passes either way there; what
 * shows a missing invalidate is klausscc --sd-dma's coherence checker
 * ("CPU executes line ..., which a DMA wrote, without an I-cache
 * invalidate since").  On the board a stale I-cache would run code_a
 * again: "dmaexec: FAIL".
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define CODE	256		/* bytes copied: more than either function */
#define BLOCK	4096		/* the O_DIRECT read */

__attribute__((noinline, aligned(64))) int code_a(int x)
{
	return x * 3 + 42;
}

__attribute__((noinline, aligned(64))) int code_b(int x)
{
	return x * 5 + 7;
}

int main(int argc, char **argv)
{
	static unsigned char buf[BLOCK] __attribute__((aligned(BLOCK)));
	unsigned char *page;
	int (*f)(int);
	int fd, a, b;

	if (argc == 3 && !strcmp(argv[1], "-w")) {
		memcpy(buf, (void *)code_b, CODE);
		fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (fd < 0 || write(fd, buf, BLOCK) != BLOCK || fsync(fd) || close(fd)) {
			perror(argv[2]);
			return 1;
		}
		printf("dmaexec: wrote %s\n", argv[2]);
		return 0;
	}
	if (argc != 2) {
		fprintf(stderr, "usage: dmaexec [-w] FILE\n");
		return 2;
	}
	page = mmap(NULL, 65536, PROT_READ | PROT_WRITE | PROT_EXEC,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (page == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	memcpy(page, (void *)code_a, CODE);
	f = (int (*)(int))(void *)page;
	a = f(1);
	fd = open(argv[1], O_RDONLY | O_DIRECT);
	if (fd < 0 || pread(fd, page, BLOCK, 0) != BLOCK) {
		perror(argv[1]);
		return 1;
	}
	close(fd);
	b = f(1);
	printf("dmaexec: %d %d %s\n", a, b, a == 45 && b == 12 ? "PASS" : "FAIL");
	return a == 45 && b == 12 ? 0 : 1;
}
