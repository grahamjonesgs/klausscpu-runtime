// SPDX-License-Identifier: GPL-2.0
/*
 * unaligntest: the kernel's emulation of misaligned user loads and stores
 * (arch/klausscpu/kernel/unaligned.c, LINUX_PLAN "Misaligned accesses").
 *
 * Every class 6/7 form that can raise MISALIGNED_LOAD/STORE is emitted as a
 * raw word (so the exact encoding is under test, not what the compiler
 * picks) with rd = r3, rs1 = r1, rs2 = r2, and run at every offset 0-7 from
 * an 8-aligned address: 16/32-bit loads (unsigned and signed) and stores in
 * the [rs1], short rs1+simm8<<SIZE, rs1+imm32, [imm32] and rs1+rs2 modes,
 * and, when STATUS.MAL64 is set, the 64-bit ones with A=0 and A=1.  Loads
 * check all 64 bits of rd; stores check the bytes around them.  Then: a
 * load with rd = rs1, C code that stores through a misaligned uint32_t *
 * (git-style), the /proc/klausscpu_perf counter, faults inside an emulated
 * access (SIGSEGV), LR/SC (SIGBUS), prctl(PR_SET_UNALIGN, PR_UNALIGN_SIGBUS)
 * (SIGBUS), and the cost of one emulated access.
 *
 * Build: clang --target=klausscpu-unknown-linux-musl --sysroot=linux-ws/sysroot
 * -static -O2 linux/userland/unaligntest.c -o unaligntest (in minish's initramfs)
 * Prints "unaligntest: PASS" or "unaligntest: FAIL (n of m checks)"; -q
 * skips the cost measurement.  Mount /proc first for the counter checks.
 */
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define STR(x) #x
#define XSTR(x) STR(x)
/* Word 0 of a class 6/7 instruction (ISA_ENCODING_V2_MAP §1, §2). */
#define ENC(len, cls, size, sgn, mode, a, imm8, rd, rs1, rs2)                 \
	(((len) << 30) | ((cls) << 26) | ((size) << 24) | ((sgn) << 23) |     \
	 ((mode) << 21) | ((a) << 20) | ((imm8) << 12) | ((rd) << 8) |         \
	 ((rs1) << 4) | (rs2))

typedef unsigned long (*form_fn)(unsigned long b, unsigned long i, unsigned long v);

/* r1 = base, r2 = index, r3 = load destination / store data. */
#define FORM(name, words)                                                     \
	static __attribute__((noinline)) unsigned long name(unsigned long b,  \
		unsigned long i, unsigned long v)                             \
	{                                                                     \
		register unsigned long r1 asm("r1") = b;                      \
		register unsigned long r2 asm("r2") = i;                      \
		register unsigned long r3 asm("r3") = v;                      \
		asm volatile(words : "+r"(r3), "+r"(r1) : "r"(r2) : "memory"); \
		return r3;                                                    \
	}
#define W1(w0) ".long " XSTR(w0) "\n"
#define W2(w0, w1) ".long " XSTR(w0) "\n\t.long " XSTR(w1) "\n"

unsigned char ua_buf[64] __attribute__((aligned(64)));

/* MODE 00 [rs1] */
FORM(ld16_m0, W1(ENC(1, 6, 1, 0, 0, 0, 0, 3, 1, 0)))
FORM(ld16s_m0, W1(ENC(1, 6, 1, 1, 0, 0, 0, 3, 1, 0)))
FORM(ld32_m0, W1(ENC(1, 6, 2, 0, 0, 0, 0, 3, 1, 0)))	/* MEMGET32: tolerant */
FORM(ld64_m0, W1(ENC(1, 6, 3, 0, 0, 0, 0, 3, 1, 0)))	/* MEMREADRR */
FORM(ld64a_m0, W1(ENC(1, 6, 3, 0, 0, 1, 0, 3, 1, 0)))	/* MEMGET64 */
FORM(st16_m0, W1(ENC(1, 7, 1, 0, 0, 0, 0, 3, 1, 0)))
FORM(st32_m0, W1(ENC(1, 7, 2, 0, 0, 0, 0, 3, 1, 0)))
FORM(st64_m0, W1(ENC(1, 7, 3, 0, 0, 0, 0, 3, 1, 0)))	/* MEMSET64RR */
FORM(st64a_m0, W1(ENC(1, 7, 3, 0, 0, 1, 0, 3, 1, 0)))	/* MEMSET64 */
/* MODE 01 short: rs1 + (simm8 << SIZE); +6, -8, +8 bytes */
FORM(ld16_sh, W1(ENC(1, 6, 1, 0, 1, 0, 3, 3, 1, 0)))
FORM(ld16s_sh, W1(ENC(1, 6, 1, 1, 1, 0, 3, 3, 1, 0)))
FORM(ld32_sh, W1(ENC(1, 6, 2, 0, 1, 0, 0xfe, 3, 1, 0)))
FORM(ld32s_sh, W1(ENC(1, 6, 2, 1, 1, 0, 0xfe, 3, 1, 0)))
FORM(ld64_sh, W1(ENC(1, 6, 3, 0, 1, 0, 1, 3, 1, 0)))
FORM(ld64a_sh, W1(ENC(1, 6, 3, 0, 1, 1, 1, 3, 1, 0)))
FORM(st16_sh, W1(ENC(1, 7, 1, 0, 1, 0, 3, 3, 1, 0)))
FORM(st32_sh, W1(ENC(1, 7, 2, 0, 1, 0, 0xfe, 3, 1, 0)))
FORM(st64_sh, W1(ENC(1, 7, 3, 0, 1, 0, 1, 3, 1, 0)))
FORM(st64a_sh, W1(ENC(1, 7, 3, 0, 1, 1, 1, 3, 1, 0)))
/* MODE 01 long: rs1 + imm32; +100, -100 */
FORM(ld16_lg, W2(ENC(2, 6, 1, 0, 1, 0, 0, 3, 1, 0), 100))
FORM(ld16s_lg, W2(ENC(2, 6, 1, 1, 1, 0, 0, 3, 1, 0), 100))
FORM(ld32_lg, W2(ENC(2, 6, 2, 0, 1, 0, 0, 3, 1, 0), 0xffffff9c))
FORM(ld32s_lg, W2(ENC(2, 6, 2, 1, 1, 0, 0, 3, 1, 0), 0xffffff9c))
FORM(ld64_lg, W2(ENC(2, 6, 3, 0, 1, 0, 0, 3, 1, 0), 100))
FORM(ld64a_lg, W2(ENC(2, 6, 3, 0, 1, 1, 0, 3, 1, 0), 0xffffff9c))
FORM(st16_lg, W2(ENC(2, 7, 1, 0, 1, 0, 0, 3, 1, 0), 100))
FORM(st32_lg, W2(ENC(2, 7, 2, 0, 1, 0, 0, 3, 1, 0), 0xffffff9c))
FORM(st64_lg, W2(ENC(2, 7, 3, 0, 1, 0, 0, 3, 1, 0), 100))
FORM(st64a_lg, W2(ENC(2, 7, 3, 0, 1, 1, 0, 3, 1, 0), 0xffffff9c))
/* MODE 11: rs1 + rs2 */
FORM(ld16_rr, W1(ENC(1, 6, 1, 0, 3, 0, 0, 3, 1, 2)))
FORM(ld16s_rr, W1(ENC(1, 6, 1, 1, 3, 0, 0, 3, 1, 2)))
FORM(ld32_rr, W1(ENC(1, 6, 2, 0, 3, 0, 0, 3, 1, 2)))
FORM(ld32s_rr, W1(ENC(1, 6, 2, 1, 3, 0, 0, 3, 1, 2)))
FORM(ld64_rr, W1(ENC(1, 6, 3, 0, 3, 0, 0, 3, 1, 2)))	/* LDIDX64R */
FORM(st16_rr, W1(ENC(1, 7, 1, 0, 3, 0, 0, 3, 1, 2)))
FORM(st32_rr, W1(ENC(1, 7, 2, 0, 3, 0, 0, 3, 1, 2)))
FORM(st64_rr, W1(ENC(1, 7, 3, 0, 3, 0, 0, 3, 1, 2)))	/* STIDX64R */
/* rd = rs1: the load overwrites its own base; returns r1 */
static __attribute__((noinline)) unsigned long ld16_self(unsigned long b)
{
	register unsigned long r1 asm("r1") = b;

	asm volatile(W1(ENC(1, 6, 1, 0, 0, 0, 0, 1, 1, 0)) : "+r"(r1) : : "memory");
	return r1;
}

/* MODE 10 [imm32]: one function per offset. */
#define ABS8(name, size, sgn, cls, a)                                         \
	FORM(name##0, W2(ENC(2, cls, size, sgn, 2, a, 0, 3, 0, 0), ua_buf + 8)) \
	FORM(name##1, W2(ENC(2, cls, size, sgn, 2, a, 0, 3, 0, 0), ua_buf + 9)) \
	FORM(name##2, W2(ENC(2, cls, size, sgn, 2, a, 0, 3, 0, 0), ua_buf + 10)) \
	FORM(name##3, W2(ENC(2, cls, size, sgn, 2, a, 0, 3, 0, 0), ua_buf + 11)) \
	FORM(name##4, W2(ENC(2, cls, size, sgn, 2, a, 0, 3, 0, 0), ua_buf + 12)) \
	FORM(name##5, W2(ENC(2, cls, size, sgn, 2, a, 0, 3, 0, 0), ua_buf + 13)) \
	FORM(name##6, W2(ENC(2, cls, size, sgn, 2, a, 0, 3, 0, 0), ua_buf + 14)) \
	FORM(name##7, W2(ENC(2, cls, size, sgn, 2, a, 0, 3, 0, 0), ua_buf + 15)) \
	static const form_fn name[8] = { name##0, name##1, name##2, name##3,  \
		name##4, name##5, name##6, name##7 };
ABS8(ld16_ab, 1, 0, 6, 0)
ABS8(ld16s_ab, 1, 1, 6, 0)
ABS8(ld32_ab, 2, 0, 6, 0)
ABS8(ld32s_ab, 2, 1, 6, 0)
ABS8(ld64_ab, 3, 0, 6, 0)
ABS8(ld64a_ab, 3, 0, 6, 1)	/* MEMREADR */
ABS8(st16_ab, 1, 0, 7, 0)
ABS8(st32_ab, 2, 0, 7, 0)
ABS8(st64_ab, 3, 0, 7, 0)
ABS8(st64a_ab, 3, 0, 7, 1)	/* MEMSETR */

enum { M0, OFF, ABS, RR };
struct form {
	const char *name;
	form_fn fn;
	const form_fn *abs;
	int kind, bytes, sgn, store, tolerant;
	long off;	/* OFF: the immediate */
};
static const struct form forms[] = {
	{ "memget16 [r]", ld16_m0, 0, M0, 2, 0, 0, 0, 0 },
	{ "memget16s [r]", ld16s_m0, 0, M0, 2, 1, 0, 0, 0 },
	{ "memget32 [r]", ld32_m0, 0, M0, 4, 0, 0, 1, 0 },
	{ "memreadrr [r]", ld64_m0, 0, M0, 8, 0, 0, 0, 0 },
	{ "memget64 [r]", ld64a_m0, 0, M0, 8, 0, 0, 0, 0 },
	{ "memset16 [r]", st16_m0, 0, M0, 2, 0, 1, 0, 0 },
	{ "memset32 [r]", st32_m0, 0, M0, 4, 0, 1, 0, 0 },
	{ "memset64rr [r]", st64_m0, 0, M0, 8, 0, 1, 0, 0 },
	{ "memset64 [r]", st64a_m0, 0, M0, 8, 0, 1, 0, 0 },
	{ "ldidx16.s r+6", ld16_sh, 0, OFF, 2, 0, 0, 0, 6 },
	{ "ldidx16s.s r+6", ld16s_sh, 0, OFF, 2, 1, 0, 0, 6 },
	{ "ldidx32.s r-8", ld32_sh, 0, OFF, 4, 0, 0, 0, -8 },
	{ "ldidx32s.s r-8", ld32s_sh, 0, OFF, 4, 1, 0, 0, -8 },
	{ "ldidx64u.s r+8", ld64_sh, 0, OFF, 8, 0, 0, 0, 8 },
	{ "ldidx64.s r+8", ld64a_sh, 0, OFF, 8, 0, 0, 0, 8 },
	{ "stidx16.s r+6", st16_sh, 0, OFF, 2, 0, 1, 0, 6 },
	{ "stidx32.s r-8", st32_sh, 0, OFF, 4, 0, 1, 0, -8 },
	{ "stidx64u.s r+8", st64_sh, 0, OFF, 8, 0, 1, 0, 8 },
	{ "stidx64.s r+8", st64a_sh, 0, OFF, 8, 0, 1, 0, 8 },
	{ "ldidx16 r+100", ld16_lg, 0, OFF, 2, 0, 0, 0, 100 },
	{ "ldidx16s r+100", ld16s_lg, 0, OFF, 2, 1, 0, 0, 100 },
	{ "ldidx32 r-100", ld32_lg, 0, OFF, 4, 0, 0, 0, -100 },
	{ "ldidx32s r-100", ld32s_lg, 0, OFF, 4, 1, 0, 0, -100 },
	{ "ldidx64u r+100", ld64_lg, 0, OFF, 8, 0, 0, 0, 100 },
	{ "ldidx64 r-100", ld64a_lg, 0, OFF, 8, 0, 0, 0, -100 },
	{ "stidx16 r+100", st16_lg, 0, OFF, 2, 0, 1, 0, 100 },
	{ "stidx32 r-100", st32_lg, 0, OFF, 4, 0, 1, 0, -100 },
	{ "stidx64u r+100", st64_lg, 0, OFF, 8, 0, 1, 0, 100 },
	{ "stidx64 r-100", st64a_lg, 0, OFF, 8, 0, 1, 0, -100 },
	{ "ld16 [abs]", 0, ld16_ab, ABS, 2, 0, 0, 0, 0 },
	{ "ld16s [abs]", 0, ld16s_ab, ABS, 2, 1, 0, 0, 0 },
	{ "ld32 [abs]", 0, ld32_ab, ABS, 4, 0, 0, 0, 0 },
	{ "ld32s [abs]", 0, ld32s_ab, ABS, 4, 1, 0, 0, 0 },
	{ "ld64 [abs]", 0, ld64_ab, ABS, 8, 0, 0, 0, 0 },
	{ "memreadr [abs]", 0, ld64a_ab, ABS, 8, 0, 0, 0, 0 },
	{ "st16 [abs]", 0, st16_ab, ABS, 2, 0, 1, 0, 0 },
	{ "st32 [abs]", 0, st32_ab, ABS, 4, 0, 1, 0, 0 },
	{ "st64 [abs]", 0, st64_ab, ABS, 8, 0, 1, 0, 0 },
	{ "memsetr [abs]", 0, st64a_ab, ABS, 8, 0, 1, 0, 0 },
	{ "ld16 r+r", ld16_rr, 0, RR, 2, 0, 0, 0, 0 },
	{ "ld16s r+r", ld16s_rr, 0, RR, 2, 1, 0, 0, 0 },
	{ "ld32 r+r", ld32_rr, 0, RR, 4, 0, 0, 0, 0 },
	{ "ld32s r+r", ld32s_rr, 0, RR, 4, 1, 0, 0, 0 },
	{ "ldidx64r r+r", ld64_rr, 0, RR, 8, 0, 0, 0, 0 },
	{ "st16 r+r", st16_rr, 0, RR, 2, 0, 1, 0, 0 },
	{ "st32 r+r", st32_rr, 0, RR, 4, 0, 1, 0, 0 },
	{ "stidx64r r+r", st64_rr, 0, RR, 8, 0, 1, 0, 0 },
};

static int fails, checks;
static int mal64;

#define CHECK(cond, ...)                                                      \
	do {                                                                  \
		checks++;                                                     \
		if (!(cond)) {                                                \
			fails++;                                              \
			printf("FAIL: " __VA_ARGS__);                         \
			putchar('\n');                                        \
		}                                                             \
	} while (0)

static unsigned char pat(int i)
{
	return (unsigned char)(0xa1 + 0x37 * i);	/* mixes high bit set and clear */
}

static void fill(void)
{
	for (int i = 0; i < (int)sizeof(ua_buf); i++)
		ua_buf[i] = pat(i);
}

static unsigned long perf_value(const char *name)
{
	char line[96];
	unsigned long v = ~0UL;
	size_t n = strlen(name);
	FILE *f = fopen("/proc/klausscpu_perf", "r");

	if (!f)
		return ~0UL;
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, name, n) && line[n] == ' ')
			v = strtoul(line + n + 1, NULL, 10);
	fclose(f);
	return v;
}

/* Run form f so that its EA is ua_buf + 8 + o; v = r3 in. */
static unsigned long run(const struct form *f, int o, unsigned long v)
{
	unsigned long ea = (unsigned long)ua_buf + 8 + o;

	switch (f->kind) {
	case M0:
		return f->fn(ea, 0, v);
	case OFF:
		return f->fn(ea - f->off, 0, v);
	case ABS:
		return f->abs[o](0, 0, v);
	default:	/* base + index, the index negative to test the 32-bit wrap */
		return f->fn(ea + 24, (unsigned long)-24L, v);
	}
}

static int traps(const struct form *f, int o)
{
	if (f->tolerant || (f->bytes == 8 && !mal64))
		return 0;
	return o % f->bytes != 0;
}

static unsigned long test_forms(void)
{
	const unsigned long data = 0x8899aabbccddeeffUL;
	unsigned long expect_emulated = 0;

	for (size_t k = 0; k < sizeof(forms) / sizeof(forms[0]); k++) {
		const struct form *f = &forms[k];

		if (f->bytes == 8 && !mal64)
			continue;	/* aligns down silently: nothing to emulate */
		for (int o = 0; o < 8; o++) {
			int ea = 8 + o;

			fill();
			expect_emulated += traps(f, o);
			if (!f->store) {
				unsigned long got = run(f, o, 0x5a5a5a5a5a5a5a5aUL), want = 0;

				for (int i = 0; i < f->bytes; i++)
					want |= (unsigned long)pat(ea + i) << (8 * i);
				if (f->sgn && f->bytes < 8)
					want = (unsigned long)((long)(want << (64 - 8 * f->bytes)) >>
							       (64 - 8 * f->bytes));
				CHECK(got == want, "%s +%d: got %016lx want %016lx", f->name, o, got, want);
			} else {
				int bad = -1;

				run(f, o, data);
				for (int i = 0; i < (int)sizeof(ua_buf); i++) {
					unsigned char want = pat(i);

					if (i >= ea && i < ea + f->bytes)
						want = (unsigned char)(data >> (8 * (i - ea)));
					if (ua_buf[i] != want && bad < 0)
						bad = i;
				}
				CHECK(bad < 0, "%s +%d: byte %d is %02x", f->name, o, bad,
				      bad < 0 ? 0 : ua_buf[bad]);
			}
		}
	}
	/* rd = rs1: the base register gets the loaded value. */
	fill();
	{
		unsigned long got = ld16_self((unsigned long)ua_buf + 9);

		CHECK(got == ((unsigned long)pat(10) << 8 | pat(9)), "rd=rs1 load: %lx", got);
		expect_emulated++;
	}
	return expect_emulated;
}

/* What compiled x86/ARM-style code does: the compiler assumes alignment. */
__attribute__((noinline)) void c_store32(uint32_t *p, uint32_t v) { *p = v; }
__attribute__((noinline)) uint32_t c_load32(const uint32_t *p) { return *p; }
__attribute__((noinline)) int32_t c_load32s(const int32_t *p) { return *p; }
__attribute__((noinline)) void c_store16(uint16_t *p, uint16_t v) { *p = v; }
__attribute__((noinline)) int16_t c_load16s(const int16_t *p) { return *p; }
__attribute__((noinline)) void c_store64(uint64_t *p, uint64_t v) { *p = v; }
__attribute__((noinline)) uint64_t c_load64(const uint64_t *p) { return *p; }

static unsigned long test_c(void)
{
	unsigned long n = 5;

	memset(ua_buf, 0, sizeof(ua_buf));
	c_store32((uint32_t *)(ua_buf + 13), 0xdeadbeef);
	CHECK(ua_buf[13] == 0xef && ua_buf[14] == 0xbe && ua_buf[15] == 0xad &&
	      ua_buf[16] == 0xde && ua_buf[12] == 0 && ua_buf[17] == 0, "C store32");
	CHECK(c_load32((uint32_t *)(ua_buf + 13)) == 0xdeadbeef, "C load32");
	CHECK(c_load32s((int32_t *)(ua_buf + 13)) == (int32_t)0xdeadbeef, "C load32s");
	c_store16((uint16_t *)(ua_buf + 21), 0x8001);
	CHECK(c_load16s((int16_t *)(ua_buf + 21)) == -32767, "C 16-bit");
	if (mal64) {
		c_store64((uint64_t *)(ua_buf + 27), 0x0123456789abcdefUL);
		CHECK(c_load64((uint64_t *)(ua_buf + 27)) == 0x0123456789abcdefUL &&
		      ua_buf[27] == 0xef && ua_buf[34] == 0x01 && ua_buf[35] == 0, "C 64-bit");
		n += 2;
	}
	return n;
}

/*
 * Run fn in a child; the child reports the signal it died of, with si_code
 * and si_addr, through its exit status.  Returns the signal (0 = exited).
 */
static volatile unsigned long expect_addr;
static void on_sig(int sig, siginfo_t *si, void *uc)
{
	(void)uc;
	printf("  child: signal %d code %d addr %08lx\n", sig, si->si_code,
	       (unsigned long)si->si_addr);
	fflush(stdout);
	_exit(100 + sig + (expect_addr && (unsigned long)si->si_addr != expect_addr ? 50 : 0));
}

static int in_child(void (*fn)(void), unsigned long addr)
{
	pid_t pid;
	int st;

	fflush(stdout);
	expect_addr = addr;
	pid = fork();
	if (pid == 0) {
		struct sigaction sa = { .sa_sigaction = on_sig, .sa_flags = SA_SIGINFO };

		sigaction(SIGSEGV, &sa, 0);
		sigaction(SIGBUS, &sa, 0);
		fn();
		_exit(0);
	}
	waitpid(pid, &st, 0);
	if (WIFEXITED(st) && WEXITSTATUS(st) >= 150)
		return -1;	/* wrong si_addr */
	return WIFEXITED(st) && WEXITSTATUS(st) > 100 ? WEXITSTATUS(st) - 100 : 0;
}

static unsigned char *pages;
static long pagesz;

static void straddle_load(void) { ld32s_lg((unsigned long)pages + pagesz - 2 + 100, 0, 0); }
static void straddle_store(void) { st32_m0((unsigned long)pages + pagesz - 2, 0, 0x11223344); }
static void unmapped_load(void) { ld16_m0((unsigned long)pages + pagesz + 1, 0, 0); }
static void readonly_store(void) { st16_rr((unsigned long)pages, 2 * pagesz + 5, 0x1234); }
static void straddle_load64(void) { ld64a_m0((unsigned long)pages + pagesz - 3, 0, 0); }

static void lr_w(void)
{
	unsigned long v, p = (unsigned long)ua_buf + 2;

	asm volatile("lr32 %0, %1" : "=r"(v) : "r"(p) : "memory");
}

static void lr_d(void)
{
	unsigned long v, p = (unsigned long)ua_buf + 4;

	asm volatile("lr64 %0, %1" : "=r"(v) : "r"(p) : "memory");
}

static void sc_w(void)
{
	unsigned long r, p = (unsigned long)ua_buf + 1, v = 7;

	asm volatile("sc32 %0, %1, %2" : "=&r"(r) : "r"(v), "r"(p) : "memory");	/* rd, data, [addr] */
}

static void sigbus_ctl(void)
{
	unsigned int v = 99;

	if (prctl(PR_SET_UNALIGN, PR_UNALIGN_SIGBUS) || prctl(PR_GET_UNALIGN, &v) ||
	    v != PR_UNALIGN_SIGBUS)
		_exit(1);
	ld16_m0((unsigned long)ua_buf + 1, 0, 0);
}

static void sigbus_ctl64(void)
{
	if (prctl(PR_SET_UNALIGN, PR_UNALIGN_SIGBUS))
		_exit(1);
	st64a_m0((unsigned long)ua_buf + 4, 0, 0);
}

static void test_faults(void)
{
	int s;

	pagesz = sysconf(_SC_PAGESIZE);
	pages = mmap(0, 3 * pagesz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	CHECK(pages != MAP_FAILED, "mmap");
	if (pages == MAP_FAILED)
		return;
	munmap(pages + pagesz, pagesz);
	mprotect(pages + 2 * pagesz, pagesz, PROT_READ);
	pages[pagesz - 2] = pages[pagesz - 1] = 0x55;

	s = in_child(straddle_load, (unsigned long)pages + pagesz);
	CHECK(s == SIGSEGV, "load straddling into an unmapped page: signal %d", s);
	s = in_child(straddle_store, (unsigned long)pages + pagesz);
	CHECK(s == SIGSEGV, "store straddling into an unmapped page: signal %d", s);
	s = in_child(unmapped_load, (unsigned long)pages + pagesz + 1);
	CHECK(s == SIGSEGV, "load in an unmapped page: signal %d", s);
	s = in_child(readonly_store, (unsigned long)pages + 2 * pagesz + 5);
	CHECK(s == SIGSEGV, "store to a read-only page: signal %d", s);
	if (mal64) {
		s = in_child(straddle_load64, (unsigned long)pages + pagesz);
		CHECK(s == SIGSEGV, "64-bit load straddling into an unmapped page: signal %d", s);
	}
	s = in_child(lr_w, 0);
	CHECK(s == SIGBUS, "misaligned lr32: signal %d", s);
	s = in_child(lr_d, 0);
	CHECK(s == SIGBUS, "misaligned lr64: signal %d", s);
	s = in_child(sc_w, 0);
	CHECK(s == SIGBUS, "misaligned sc32: signal %d", s);
	s = in_child(sigbus_ctl, 0);
	CHECK(s == SIGBUS, "prctl(PR_SET_UNALIGN, PR_UNALIGN_SIGBUS): signal %d", s);
	if (mal64) {
		s = in_child(sigbus_ctl64, 0);
		CHECK(s == SIGBUS, "PR_UNALIGN_SIGBUS, 64-bit store: signal %d", s);
	}
	munmap(pages, 3 * pagesz);
}

/*
 * Instructions and cycles per access, from the global counters: n calls of
 * fn, less the same n calls doing an aligned access (base).  The /proc
 * reads and the call cancel out.
 */
static unsigned long base_i, base_c;
static void cost(const char *what, form_fn fn, unsigned long b, int n, int base)
{
	unsigned long i0 = perf_value("instructions"), c0 = perf_value("cycles");
	unsigned long i, c;

	for (int k = 0; k < n; k++)
		fn(b, 0, 0);
	i = perf_value("instructions") - i0;
	c = perf_value("cycles") - c0;
	if (base) {
		base_i = i;
		base_c = c;
		return;
	}
	printf("cost %-22s +%lu instructions +%lu cycles per access\n", what,
	       (i - base_i) / n, (c - base_c) / n);
}

int main(int argc, char **argv)
{
	unsigned long before, after, expect;
	unsigned long status;
	int quick = argc > 1 && !strcmp(argv[1], "-q");

	asm volatile("mfsr %0, status" : "=r"(status));
	mal64 = (status >> 10) & 1;
	printf("unaligntest: STATUS.MAL64 = %d%s\n", mal64,
	       mal64 ? "" : " (64-bit forms skipped)");
	before = perf_value("misaligned_emulated");
	expect = test_forms();
	expect += test_c();
	after = perf_value("misaligned_emulated");
	if (before != ~0UL)
		CHECK(after - before == expect, "misaligned_emulated went up by %lu, want %lu",
		      after - before, expect);
	printf("unaligntest: %lu accesses emulated\n", expect);
	test_faults();
	if (!quick && before != ~0UL) {
		prctl(PR_SET_UNALIGN, PR_UNALIGN_NOPRINT);
		cost("aligned memget16", ld16_m0, (unsigned long)ua_buf + 2, 2000, 1);
		cost("emulated memget16", ld16_m0, (unsigned long)ua_buf + 3, 2000, 0);
		cost("emulated stidx32 r-100", st32_lg, (unsigned long)ua_buf + 5 + 100, 2000, 0);
		if (mal64)
			cost("emulated memget64", ld64a_m0, (unsigned long)ua_buf + 3, 2000, 0);
		prctl(PR_SET_UNALIGN, 0);
	}
	if (fails)
		printf("unaligntest: FAIL (%d of %d checks)\n", fails, checks);
	else
		printf("unaligntest: %d checks\nunaligntest: PASS\n", checks);
	return fails != 0;
}
