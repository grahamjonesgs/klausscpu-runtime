/*
 * aliasjit: code written through one mapping and run through another, the
 * case __builtin___clear_cache / cacheflush(2) exist for on KlaussCPU.
 *
 * One page of a memfd is mapped twice: read/write at W and read/execute at
 * X.  Line 0 of X holds a NOP slide (function A), line 1 a function B that
 * returns a constant.  Each round calls A (the front end streams through
 * line 0 and may prefetch line 1 into a next-line buffer), rewrites B
 * through W with a new constant, optionally clears the cache for B, and
 * calls B through X.  The I-cache snoops the store by physical address,
 * but the fetch window and next-line buffers compare virtual addresses
 * (W != X), so without the clear B may run its old code.
 *
 *   aliasjit [rounds]   default 20000
 * A second shape needs no call between the write and the run: line 2 of X
 * (function S) stores B's new constant through W, then falls through into
 * line 3 (function T, B's twin).  The next-line buffer may already hold
 * line 3 when the store lands, so T can return the old constant; nothing
 * in user space can prevent that but not running code it has just written
 * that way (the store's own refetch check compares virtual addresses).
 *
 * A third shape is the narrow window itself (from the core's RTL: the
 * fetch window and next-line buffers hold code already fetched, checked by
 * virtual address): function F stores a new immediate through W into an
 * instruction 0-7 NOPs after the store, then falls into it, for each of
 * the store's four word positions in a 16-byte chunk.  The small gaps are
 * expected to run the old immediate; a JIT never runs code that way.  The
 * same F run after the write and __clear_cache (F's own store then goes
 * elsewhere) must always see the new one.  On today's core a store that
 * posts speculatively is checked against the I-cache by physical address
 * and refetches what follows it, so the sweep is repeated with the alias
 * store as the third of three back-to-back stores, which waits instead.
 *
 * Since none of that shows stale code on today's core, the test also
 * proves the flush happens: a 4 KiB NOP slide in X runs measurably slower
 * (CYCLE, readable in user mode) after cacheflush(ICACHE) than after
 * cacheflush(DCACHE), which leaves the I-cache alone.
 *
 * Exit 0 when the I-cache flush is measurable, every round with the clear returned the new constant and the
 * cacheflush(2) argument checks behave; the rounds without the clear are
 * only counted (a stale count shows the hazard on this core).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define NR_cacheflush 244	/* asm/cachectl.h: ICACHE 1, DCACHE 2 */

/* Position-independent templates, each one 32-byte line. */
__asm__(
	"	.text\n"
	"	.p2align 5\n"
	"tmpl_a:\n"
	"	nop\n	nop\n	nop\n	nop\n	nop\n	nop\n	nop\n"
	"	ret\n"
	"tmpl_a_end:\n"
	"	.p2align 5\n"
	"tmpl_b1:\n"
	"	setr r12, 1111\n"
	"	ret\n"
	"tmpl_b1_end:\n"
	"	.p2align 5\n"
	"tmpl_b2:\n"
	"	setr r12, 2222\n"
	"	ret\n"
	"tmpl_b2_end:\n"
	/* S: [r0] = r1, then fall through into T (the next line). */
	"	.p2align 5\n"
	"tmpl_s:\n"
	"	memset32 r1, r0\n"
	"	nop\n	nop\n	nop\n	nop\n	nop\n	nop\n	nop\n"
	"	setr r12, 1111\n"
	"	ret\n"
	"tmpl_s_end:\n");
extern const char tmpl_a[], tmpl_a_end[], tmpl_b1[], tmpl_b1_end[],
	tmpl_b2[], tmpl_b2_end[], tmpl_s[], tmpl_s_end[];

static long rounds(char *w, char *x, long n, int clear)
{
	void (*fa)(void) = (void (*)(void))x;
	long (*fb)(void) = (long (*)(void))(x + 32);
	long stale = 0;

	for (long i = 0; i < n; i++) {
		const char *t = (i & 1) ? tmpl_b2 : tmpl_b1;
		size_t len = (i & 1) ? tmpl_b2_end - tmpl_b2 : tmpl_b1_end - tmpl_b1;
		long want = (i & 1) ? 2222 : 1111;

		fa();
		memcpy(w + 32, t, len);
		if (clear)
			__builtin___clear_cache(x + 32, x + 32 + len);
		if (fb() != want)
			stale++;
	}
	return stale;
}

/* S at line 2 stores T's constant (the setr's 4-byte immediate, 4 bytes
   into line 3) through W, then runs T. */
static long inline_rounds(char *w, char *x, long n)
{
	long (*fs)(int *, long) = (long (*)(int *, long))(x + 64);
	int *imm = (int *)(w + 96 + 4);
	long stale = 0;

	for (long i = 0; i < n; i++) {
		long want = (i & 1) ? 2222 : 1111;

		if (fs(imm, want) != want)
			stale++;
	}
	return stale;
}

/* Instruction words (memory order) for the generated F. */
static const unsigned char op_st[4] = { 0x00, 0x01, 0x00, 0x5e };   /* memset32 r1, r0 */
static const unsigned char op_st2[4] = { 0x20, 0x01, 0x00, 0x5e };  /* memset32 r1, r2 */
static const unsigned char op_st3[4] = { 0x30, 0x01, 0x00, 0x5e };  /* memset32 r1, r3 */
static const unsigned char op_nop[4] = { 0x00, 0x00, 0x00, 0x6c };
static const unsigned char op_setr[4] = { 0x00, 0x0c, 0xd0, 0x8b }; /* setr r12, imm32 */
static const unsigned char op_ret[4] = { 0x00, 0x00, 0x80, 0x65 };

/* F(imm, v, s1, s2) at w/x + off: a NOPs, [two stores of v to the data
   words s2 and s1, when triple,] the store of v to imm, g NOPs,
   setr r12, imm32; ret.  Returns the offset of the immediate. */
static long gen_f(char *w, long off, int a, int g, int triple)
{
	char *p = w + off;

	for (int i = 0; i < a; i++, p += 4)
		memcpy(p, op_nop, 4);
	if (triple) {
		memcpy(p, op_st2, 4), p += 4;
		memcpy(p, op_st3, 4), p += 4;
	}
	memcpy(p, op_st, 4), p += 4;
	for (int i = 0; i < g; i++, p += 4)
		memcpy(p, op_nop, 4);
	memcpy(p, op_setr, 4), p += 4;
	memset(p, 0, 4), p += 4;
	memcpy(p, op_ret, 4);
	return p - 4 - w;
}

static int scratch[16];

static int window_sweep(char *w, char *x, long n, int triple)
{
	int bad = 0;
	int dummy;

	for (int a = 0; a < 4; a++) {
		printf("aliasjit: %s at word %d of a 16 B chunk, stale per gap 0..7:",
		       triple ? "third of three stores" : "store", a);
		for (int g = 0; g < 8; g++) {
			long off = 4096 + (triple * 32 + a * 8 + g) * 64;
			long imm = gen_f(w, off, a, g, triple);
			long (*f)(void *, long, int *, int *) =
				(long (*)(void *, long, int *, int *))(x + off);
			long stale = 0, cstale = 0;

			__builtin___clear_cache(x + off, x + off + 64);
			for (long i = 0; i < n; i++) {
				int want = (int)(i * 2 + 3);

				/* F writes its own next instruction through W. */
				if (f(w + imm, want, &scratch[0], &scratch[8]) != want)
					stale++;
				/* The JIT order: write, clear, run (F's store goes
				   to a dummy word). */
				*(volatile int *)(w + imm) = want + 1;
				__builtin___clear_cache(x + imm - 4, x + imm + 4);
				if (f(&dummy, 0, &scratch[0], &scratch[8]) != want + 1)
					cstale++;
			}
			printf(" %ld", stale);
			if (cstale) {
				printf("(+%ld with clear!)", cstale);
				bad++;
			}
		}
		printf("\n");
	}
	return bad;
}

static inline unsigned long cycles(void)
{
	unsigned long v;

	__asm__ volatile("mfsr %0, cycle" : "=r"(v));
	return v;
}

/* The NOP slide at x + 16 KiB timed right after cacheflush(cache): the best
   of 8 runs. */
static unsigned long timed_after(char *x, int cache)
{
	void (*slide)(void) = (void (*)(void))(x + 16384);
	unsigned long best = ~0UL;

	for (int i = 0; i < 8; i++) {
		slide();
		slide();
		syscall(NR_cacheflush, x, 64, cache);
		unsigned long t = cycles();
		slide();
		t = cycles() - t;
		if (t < best)
			best = t;
	}
	return best;
}

int main(int argc, char **argv)
{
	long n = argc > 1 ? atol(argv[1]) : 20000;
	long page = sysconf(_SC_PAGESIZE);
	int fd = memfd_create("aliasjit", 0), bad = 0;
	char *w, *x;
	long r;

	if (page < 32768) {
		printf("aliasjit: needs pages of at least 32 KiB (have %ld)\n", page);
		return 2;
	}
	if (fd < 0 || ftruncate(fd, page)) {
		perror("aliasjit: memfd");
		return 2;
	}
	w = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	x = mmap(NULL, page, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
	if (w == MAP_FAILED || x == MAP_FAILED || w == x) {
		perror("aliasjit: mmap");
		return 2;
	}
	memcpy(w, tmpl_a, tmpl_a_end - tmpl_a);
	memcpy(w + 32, tmpl_b1, tmpl_b1_end - tmpl_b1);
	memcpy(w + 64, tmpl_s, tmpl_s_end - tmpl_s);
	for (int i = 0; i < 4096 - 4; i += 4)
		memcpy(w + 16384 + i, op_nop, 4);
	memcpy(w + 16384 + 4096 - 4, op_ret, 4);
	__builtin___clear_cache(x, x + page);

	/* 1,024 NOPs in 128 lines: refetching them all costs at least a few
	   cycles a line. */
	unsigned long t_d = timed_after(x, 2), t_i = timed_after(x, 1);
	printf("aliasjit: 4 KiB of code after cacheflush(DCACHE) %lu cycles, after cacheflush(ICACHE) %lu\n",
	       t_d, t_i);
	if (t_i < t_d + 2 * 128) {
		printf("aliasjit: FAIL cacheflush(ICACHE) did not empty the I-cache\n");
		bad++;
	}

	/* cacheflush(2) arguments: ICACHE | DCACHE is fine, other bits and a
	   wrapping range are refused. */
	r = syscall(NR_cacheflush, x, 64, 3);
	if (r != 0) {
		printf("aliasjit: FAIL cacheflush(x, 64, BCACHE) = %ld errno %d\n", r, errno);
		bad++;
	}
	r = syscall(NR_cacheflush, x, 64, 4);
	if (r != -1 || errno != EINVAL) {
		printf("aliasjit: FAIL cacheflush(x, 64, 4) = %ld errno %d (want EINVAL)\n", r, errno);
		bad++;
	}
	r = syscall(NR_cacheflush, x, 64, 0);
	if (r != -1 || errno != EINVAL) {
		printf("aliasjit: FAIL cacheflush(x, 64, 0) = %ld errno %d (want EINVAL)\n", r, errno);
		bad++;
	}
	r = syscall(NR_cacheflush, -16L, 64, 1);
	if (r != -1 || errno != EFAULT) {
		printf("aliasjit: FAIL cacheflush(-16, 64, ICACHE) = %ld errno %d (want EFAULT)\n", r, errno);
		bad++;
	}

	long with = rounds(w, x, n, 1);
	long without = rounds(w, x, n, 0);
	long with2 = rounds(w, x, n, 1);
	long inl = inline_rounds(w, x, n);
	bad += window_sweep(w, x, n / 20 ? n / 20 : 1, 0);
	bad += window_sweep(w, x, n / 20 ? n / 20 : 1, 1);

	printf("aliasjit: %ld rounds: stale with __clear_cache %ld + %ld, without %ld; "
	       "written and run with no call between: %ld\n",
	       n, with, with2, without, inl);
	if (with || with2)
		bad++;
	printf("aliasjit: %s\n", bad ? "FAIL" : "PASS");
	return bad ? 1 : 0;
}
