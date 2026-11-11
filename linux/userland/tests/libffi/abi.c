/*
 * abi.c: the KlaussCPU libffi port against clang's own calling convention
 * (klausscpu-llvm ABI.md, version 3), in the corners libffi's testsuite
 * does not look at closely: the exact argument words (32-bit values
 * sign-extended whatever their signedness, 8/16-bit ones by theirs, float
 * in the low half, aggregate bytes in memory order), two-word arguments
 * split between R3 and the first stack slot, aggregates over 16 bytes by
 * reference to a copy, sret in R0, the 32-byte home area of variadic
 * calls, the R12/R11 results, and closures called from compiled code.
 *
 * kl_capture (abi_helpers.S) records R0-R3 and the stack slots; each
 * case calls it once through a C function pointer of the case's
 * prototype (clang's placement, the reference) and once through
 * ffi_call, and compares the words that the ABI defines (masks leave out
 * the upper half of a float, padding, the home area).  Closures are
 * called by compiled code, and kl_call_raw shows their R12 as is.
 * Prints FAIL lines and "DONE checks=N failures=M".
 */
#include <ffi.h>
#include <complex.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCAP 16
uint64_t kl_cap[NCAP];
uint64_t kl_ret[2];
extern void kl_capture(void);
struct kl_raw {
	void *fn;
	uint64_t r[4];
	uint64_t out[2];
};
extern void kl_call_raw(struct kl_raw *);

static int checks, failures;
#define CHECK(name, cond)                                                   \
	do {                                                                \
		checks++;                                                   \
		if (!(cond)) {                                              \
			failures++;                                         \
			printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); \
		}                                                           \
	} while (0)
#define CHECKX(name, got, want)                                             \
	do {                                                                \
		uint64_t g_ = (uint64_t)(got), w_ = (uint64_t)(want);       \
		checks++;                                                   \
		if (g_ != w_) {                                             \
			failures++;                                         \
			printf("FAIL %s: %s = %#" PRIx64 ", want %#" PRIx64 " (line %d)\n", \
			       name, #got, g_, w_, __LINE__);               \
		}                                                           \
	} while (0)

#define M (~0ULL)
#define LO32 0xFFFFFFFFULL

static uint64_t ref[NCAP];
static void clear_cap(void)
{
	memset(kl_cap, 0xA5, sizeof kl_cap);
}
/* The masked words of kl_cap against ref: regs[0..3], then slots. */
static void cmp_cap(const char *name, const uint64_t *mask, int n)
{
	for (int i = 0; i < n; i++) {
		checks++;
		if ((kl_cap[i] ^ ref[i]) & mask[i]) {
			failures++;
			printf("FAIL %s: word %d (%s%d) ffi %#" PRIx64 " clang %#" PRIx64 " mask %#" PRIx64 "\n",
			       name, i, i < 4 ? "R" : "slot ", i < 4 ? i : i - 4, kl_cap[i], ref[i], mask[i]);
		}
	}
}
static void ffi_capture(ffi_cif *cif, void *rv, void **av)
{
	clear_cap();
	ffi_call(cif, FFI_FN(kl_capture), rv, av);
}

/* ---- aggregates ---- */
struct s3 { unsigned char a, b, c; };
struct sc { char c; short s; };		/* padding byte 1 */
struct s12 { int a, b, c; };
struct s16 { long a, b; };
struct sfd { float f; double d; };	/* padding bytes 4-7 */
struct s24 { long a, b, c; };

static ffi_type *s3_el[] = { &ffi_type_uint8, &ffi_type_uint8, &ffi_type_uint8, NULL };
static ffi_type t_s3 = { 0, 0, FFI_TYPE_STRUCT, s3_el };
static ffi_type *sc_el[] = { &ffi_type_schar, &ffi_type_sshort, NULL };
static ffi_type t_sc = { 0, 0, FFI_TYPE_STRUCT, sc_el };
static ffi_type *s12_el[] = { &ffi_type_sint, &ffi_type_sint, &ffi_type_sint, NULL };
static ffi_type t_s12 = { 0, 0, FFI_TYPE_STRUCT, s12_el };
static ffi_type *s16_el[] = { &ffi_type_slong, &ffi_type_slong, NULL };
static ffi_type t_s16 = { 0, 0, FFI_TYPE_STRUCT, s16_el };
static ffi_type *sfd_el[] = { &ffi_type_float, &ffi_type_double, NULL };
static ffi_type t_sfd = { 0, 0, FFI_TYPE_STRUCT, sfd_el };
static ffi_type *s24_el[] = { &ffi_type_slong, &ffi_type_slong, &ffi_type_slong, NULL };
static ffi_type t_s24 = { 0, 0, FFI_TYPE_STRUCT, s24_el };

static uint64_t f2w(float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}

/* ================= arguments: libffi's words against clang's ================= */
static void args_cases(void)
{
	ffi_cif cif;
	ffi_type *at[12];
	void *av[12];

	/* 8- and 16-bit: extended by their signedness. */
	{
		uint8_t a = 0xF0; int8_t b = -3; uint16_t c = 0xF00F; int16_t d = -300;
		clear_cap();
		((void (*)(uint8_t, int8_t, uint16_t, int16_t))kl_capture)(a, b, c, d);
		memcpy(ref, kl_cap, sizeof ref);
		CHECKX("narrow-clang", ref[0], 0xF0);
		CHECKX("narrow-clang", ref[1], (uint64_t)-3);
		CHECKX("narrow-clang", ref[2], 0xF00F);
		CHECKX("narrow-clang", ref[3], (uint64_t)-300);
		at[0] = &ffi_type_uint8; at[1] = &ffi_type_sint8; at[2] = &ffi_type_uint16; at[3] = &ffi_type_sint16;
		av[0] = &a; av[1] = &b; av[2] = &c; av[3] = &d;
		CHECK("narrow", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 4, &ffi_type_void, at) == FFI_OK);
		ffi_capture(&cif, NULL, av);
		cmp_cap("narrow", (uint64_t[]){ M, M, M, M }, 4);
	}
	/* 32-bit: sign-extended, unsigned int too; float in the low half. */
	{
		uint32_t a = 0x80000000u, c = 0xFFFFFFFFu; int32_t b = -5; float d = 1.5f;
		clear_cap();
		((void (*)(uint32_t, int32_t, uint32_t, float))kl_capture)(a, b, c, d);
		memcpy(ref, kl_cap, sizeof ref);
		CHECKX("int32-clang", ref[0], 0xFFFFFFFF80000000ULL);
		CHECKX("int32-clang", ref[2], M);
		CHECKX("int32-clang", ref[3] & LO32, 0x3FC00000);
		at[0] = &ffi_type_uint32; at[1] = &ffi_type_sint32; at[2] = &ffi_type_uint32; at[3] = &ffi_type_float;
		av[0] = &a; av[1] = &b; av[2] = &c; av[3] = &d;
		CHECK("int32", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 4, &ffi_type_void, at) == FFI_OK);
		ffi_capture(&cif, NULL, av);
		cmp_cap("int32", (uint64_t[]){ M, M, M, LO32 }, 4);
		/* FFI_TYPE_INT and unsigned int via ffi_type_uint */
		unsigned int e = 0x9abcdef0u; int f = -7;
		at[0] = &ffi_type_uint; at[1] = &ffi_type_sint;
		av[0] = &e; av[1] = &f;
		CHECK("int", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &ffi_type_void, at) == FFI_OK);
		ffi_capture(&cif, NULL, av);
		CHECKX("int", kl_cap[0], 0xFFFFFFFF9abcdef0ULL);
		CHECKX("int", kl_cap[1], (uint64_t)-7);
	}
	/* 64-bit scalars, then the stack. */
	{
		double a = -2.25; long double b = 3.5L; void *c = &kl_cap; int64_t d = -1234567890123LL;
		uint64_t e = 0xFEDCBA9876543210ULL, f = 42; float g = -0.5f; int8_t h = -1;
		clear_cap();
		((void (*)(double, long double, void *, int64_t, uint64_t, uint64_t, float, int8_t))kl_capture)(
			a, b, c, d, e, f, g, h);
		memcpy(ref, kl_cap, sizeof ref);
		CHECKX("scalars-clang", ref[4], e);
		CHECKX("scalars-clang", ref[7], (uint64_t)-1);
		at[0] = &ffi_type_double; at[1] = &ffi_type_longdouble; at[2] = &ffi_type_pointer;
		at[3] = &ffi_type_sint64; at[4] = &ffi_type_uint64; at[5] = &ffi_type_uint64;
		at[6] = &ffi_type_float; at[7] = &ffi_type_sint8;
		av[0] = &a; av[1] = &b; av[2] = &c; av[3] = &d; av[4] = &e; av[5] = &f; av[6] = &g; av[7] = &h;
		CHECK("scalars", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 8, &ffi_type_void, at) == FFI_OK);
		ffi_capture(&cif, NULL, av);
		cmp_cap("scalars", (uint64_t[]){ M, M, M, M, M, M, LO32, M }, 8);
	}
	/* Small aggregates: bytes in memory order; padding undefined. */
	{
		struct s3 a = { 0x11, 0x22, 0x33 };
		struct sc b = { -2, -1000 };
		struct s12 c = { 1, -2, 3 };
		long x = 77;
		clear_cap();
		((void (*)(struct s3, struct sc, struct s12, long))kl_capture)(a, b, c, x);
		memcpy(ref, kl_cap, sizeof ref);
		CHECKX("small-agg-clang", ref[0] & 0xFFFFFF, 0x332211);
		CHECKX("small-agg-clang", ref[2], 0xFFFFFFFE00000001ULL);
		CHECKX("small-agg-clang", ref[4], 77);	/* s12 took R2-R3: x is the first slot */
		at[0] = &t_s3; at[1] = &t_sc; at[2] = &t_s12; at[3] = &ffi_type_slong;
		av[0] = &a; av[1] = &b; av[2] = &c; av[3] = &x;
		CHECK("small-agg", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 4, &ffi_type_void, at) == FFI_OK);
		ffi_capture(&cif, NULL, av);
		cmp_cap("small-agg", (uint64_t[]){ 0xFFFFFF, 0xFFFF00FF, M, LO32, M }, 5);
	}
	/* Two-word arguments split between R3 and the first stack slot. */
	{
		long a = 1, b = 2, c = 3, z = 9;
		struct s16 d = { 0x1111, 0x2222 };
		struct s12 e = { 4, 5, 6 };
		struct sfd f = { 2.5f, -8.0 };
		double _Complex g = 1.25 + 7.5 * I;
		float _Complex h = -1.0f + 2.0f * I;

		clear_cap();
		((void (*)(long, long, long, struct s16, long))kl_capture)(a, b, c, d, z);
		memcpy(ref, kl_cap, sizeof ref);
		CHECKX("split-s16-clang", ref[3], 0x1111);
		CHECKX("split-s16-clang", ref[4], 0x2222);
		CHECKX("split-s16-clang", ref[5], 9);
		at[0] = at[1] = at[2] = at[4] = &ffi_type_slong; at[3] = &t_s16;
		av[0] = &a; av[1] = &b; av[2] = &c; av[3] = &d; av[4] = &z;
		CHECK("split-s16", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 5, &ffi_type_void, at) == FFI_OK);
		ffi_capture(&cif, NULL, av);
		cmp_cap("split-s16", (uint64_t[]){ M, M, M, M, M, M }, 6);

		clear_cap();
		((void (*)(long, long, long, struct s12, long))kl_capture)(a, b, c, e, z);
		memcpy(ref, kl_cap, sizeof ref);
		at[3] = &t_s12; av[3] = &e;
		CHECK("split-s12", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 5, &ffi_type_void, at) == FFI_OK);
		ffi_capture(&cif, NULL, av);
		cmp_cap("split-s12", (uint64_t[]){ M, M, M, M, LO32, M }, 6);

		clear_cap();
		((void (*)(long, long, long, struct sfd, long))kl_capture)(a, b, c, f, z);
		memcpy(ref, kl_cap, sizeof ref);
		at[3] = &t_sfd; av[3] = &f;
		CHECK("split-sfd", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 5, &ffi_type_void, at) == FFI_OK);
		ffi_capture(&cif, NULL, av);
		cmp_cap("split-sfd", (uint64_t[]){ M, M, M, LO32, M, M }, 6);

		clear_cap();
		((void (*)(long, long, long, double _Complex, long))kl_capture)(a, b, c, g, z);
		memcpy(ref, kl_cap, sizeof ref);
		at[3] = &ffi_type_complex_double; av[3] = &g;
		CHECK("split-cdouble", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 5, &ffi_type_void, at) == FFI_OK);
		ffi_capture(&cif, NULL, av);
		cmp_cap("split-cdouble", (uint64_t[]){ M, M, M, M, M, M }, 6);

		clear_cap();
		((void (*)(float _Complex, long))kl_capture)(h, z);
		memcpy(ref, kl_cap, sizeof ref);
		CHECKX("cfloat-clang", ref[0], (f2w(2.0f) << 32) | f2w(-1.0f));
		at[0] = &ffi_type_complex_float; at[1] = &ffi_type_slong;
		av[0] = &h; av[1] = &z;
		CHECK("cfloat", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &ffi_type_void, at) == FFI_OK);
		ffi_capture(&cif, NULL, av);
		cmp_cap("cfloat", (uint64_t[]){ M, M }, 2);
	}
	/* sret: R0 is the result buffer, the arguments move up by one. */
	{
		long a = 5, b = 6, c = 7, d = 8;
		struct s24 r;
		clear_cap();
		r = ((struct s24 (*)(long, long, long, long))kl_capture)(a, b, c, d);
		(void)r;
		memcpy(ref, kl_cap, sizeof ref);
		CHECK("sret-clang", ref[0] != 5 && ref[1] == 5 && ref[3] == 7 && ref[4] == 8);
		at[0] = at[1] = at[2] = at[3] = &ffi_type_slong;
		av[0] = &a; av[1] = &b; av[2] = &c; av[3] = &d;
		CHECK("sret", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 4, &t_s24, at) == FFI_OK);
		struct s24 rv;
		ffi_capture(&cif, &rv, av);
		CHECKX("sret", kl_cap[0], (uintptr_t)&rv);
		cmp_cap("sret", (uint64_t[]){ 0, M, M, M, M }, 5);
	}
	/* Variadic: the home area, then the stack arguments; a two-word
	   argument at R3 continues after the home area. */
	{
		int n = 1;
		long b = 2, c = 3, z = 9;
		struct s16 d = { 0x4444, 0x5555 };
		double e = 6.5;
		clear_cap();
		((void (*)(int, ...))kl_capture)(n, b, c, d, z, e);
		memcpy(ref, kl_cap, sizeof ref);
		CHECKX("var-clang", ref[3], 0x4444);
		CHECKX("var-clang", ref[8], 0x5555);	/* slot 4: after the 32-byte home area */
		CHECKX("var-clang", ref[9], 9);
		at[0] = &ffi_type_sint; at[1] = at[2] = at[4] = &ffi_type_slong; at[3] = &t_s16; at[5] = &ffi_type_double;
		av[0] = &n; av[1] = &b; av[2] = &c; av[3] = &d; av[4] = &z; av[5] = &e;
		CHECK("var", ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, 1, 6, &ffi_type_void, at) == FFI_OK);
		ffi_capture(&cif, NULL, av);
		cmp_cap("var", (uint64_t[]){ M, M, M, M, 0, 0, 0, 0, M, M, M }, 11);
		/* The same arguments without the variadic call: no home area. */
		CHECK("nonvar", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 6, &ffi_type_void, at) == FFI_OK);
		ffi_capture(&cif, NULL, av);
		CHECKX("nonvar", kl_cap[4], 0x5555);
		CHECKX("nonvar", kl_cap[5], 9);
		/* All arguments in registers: still a home area below them. */
		long w = 0x7777;
		clear_cap();
		((void (*)(int, ...))kl_capture)(n, w);
		memcpy(ref, kl_cap, sizeof ref);
		av[1] = &w;
		CHECK("var-regs", ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, 1, 2, &ffi_type_void, at) == FFI_OK);
		ffi_capture(&cif, NULL, av);
		cmp_cap("var-regs", (uint64_t[]){ M, M }, 2);
	}
}

/* ================= results: what libffi reads against what clang reads ================= */
static void ret_cases(void)
{
	ffi_cif cif;
	ffi_arg r;

#define RET_INT(name, T, ftype, r12, want)                                  \
	do {                                                                \
		kl_ret[0] = (r12);                                          \
		T c_ = ((T(*)(void))kl_capture)();                          \
		CHECK(name "-clang", c_ == (T)(want));                      \
		CHECK(name, ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 0, &ftype, NULL) == FFI_OK); \
		r = 0xA5A5A5A5A5A5A5A5ULL;                                  \
		ffi_call(&cif, FFI_FN(kl_capture), &r, NULL);               \
		CHECKX(name, r, (ffi_arg)(T)(want));                        \
	} while (0)
	/* R12 as a correct producer leaves it (extended per the ABI); libffi
	   gives an ffi_arg extended by the type's own signedness. */
	RET_INT("ret-u8", uint8_t, ffi_type_uint8, 0xF0, 0xF0);
	RET_INT("ret-s8", int8_t, ffi_type_sint8, (uint64_t)-16, -16);
	RET_INT("ret-u16", uint16_t, ffi_type_uint16, 0xF00F, 0xF00F);
	RET_INT("ret-s16", int16_t, ffi_type_sint16, (uint64_t)-300, -300);
	RET_INT("ret-u32", uint32_t, ffi_type_uint32, 0xFFFFFFFF80000000ULL, 0x80000000u);
	RET_INT("ret-s32", int32_t, ffi_type_sint32, 0xFFFFFFFF80000000ULL, INT32_MIN);
	RET_INT("ret-u64", uint64_t, ffi_type_uint64, 0x8000000000000001ULL, 0x8000000000000001ULL);
	/* float: only the low half is defined. */
	{
		kl_ret[0] = 0xDEADBEEF00000000ULL | f2w(1.5f);
		float c = ((float (*)(void))kl_capture)(), f = 0;
		CHECK("ret-float-clang", c == 1.5f);
		CHECK("ret-float", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 0, &ffi_type_float, NULL) == FFI_OK);
		ffi_call(&cif, FFI_FN(kl_capture), &f, NULL);
		CHECK("ret-float", f == 1.5f);
	}
	/* Aggregates in R12/R11. */
	{
		kl_ret[0] = 0xFFFFFFFE00000001ULL; kl_ret[1] = 0xABCDEF0000000003ULL;
		struct s12 c = ((struct s12 (*)(void))kl_capture)(), f;
		CHECK("ret-s12-clang", c.a == 1 && c.b == -2 && c.c == 3);
		CHECK("ret-s12", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 0, &t_s12, NULL) == FFI_OK);
		memset(&f, 0, sizeof f);
		ffi_call(&cif, FFI_FN(kl_capture), &f, NULL);
		CHECK("ret-s12", f.a == 1 && f.b == -2 && f.c == 3);

		struct sfd cs, fs;
		uint64_t d;
		double dv = -3.75;
		memcpy(&d, &dv, 8);
		kl_ret[0] = 0x12345678ULL << 32 | f2w(0.25f); kl_ret[1] = d;
		cs = ((struct sfd (*)(void))kl_capture)();
		CHECK("ret-sfd-clang", cs.f == 0.25f && cs.d == -3.75);
		CHECK("ret-sfd", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 0, &t_sfd, NULL) == FFI_OK);
		ffi_call(&cif, FFI_FN(kl_capture), &fs, NULL);
		CHECK("ret-sfd", fs.f == 0.25f && fs.d == -3.75);

		kl_ret[0] = 0x332211ULL | 0xFFFFFF0000000000ULL;
		struct s3 c3 = ((struct s3 (*)(void))kl_capture)(), f3[2];
		CHECK("ret-s3-clang", c3.a == 0x11 && c3.b == 0x22 && c3.c == 0x33);
		CHECK("ret-s3", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 0, &t_s3, NULL) == FFI_OK);
		memset(f3, 0x5A, sizeof f3);
		ffi_call(&cif, FFI_FN(kl_capture), &f3[0], NULL);
		CHECK("ret-s3", f3[0].a == 0x11 && f3[0].b == 0x22 && f3[0].c == 0x33);
		CHECK("ret-s3-no-overrun", f3[1].a == 0x5A);	/* only 3 bytes written */

		double _Complex cc, fc;
		double re = 1.5, im = -2.5;
		memcpy(&kl_ret[0], &re, 8); memcpy(&kl_ret[1], &im, 8);
		cc = ((double _Complex (*)(void))kl_capture)();
		CHECK("ret-cdouble-clang", creal(cc) == 1.5 && cimag(cc) == -2.5);
		CHECK("ret-cdouble", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 0, &ffi_type_complex_double, NULL) == FFI_OK);
		ffi_call(&cif, FFI_FN(kl_capture), &fc, NULL);
		CHECK("ret-cdouble", creal(fc) == 1.5 && cimag(fc) == -2.5);
	}
}

/* ================= calls into compiled functions ================= */
__attribute__((noinline)) int is_u32_top(unsigned x) { return x == 0x80000000u; }
__attribute__((noinline)) unsigned ret_u32_top(void) { return 0x80000000u; }
__attribute__((noinline)) long take24(struct s24 s)
{
	long r = s.a * 100 + s.b * 10 + s.c;
	s.a = s.b = s.c = -1;	/* the callee's copy */
	return r;
}
__attribute__((noinline)) long take24_at5(long a, long b, long c, long d, struct s24 s)
{
	return a + b + c + d + s.a * 1000 + s.b * 100 + s.c * 10;
}
__attribute__((noinline)) double mix(char a, short b, int c, long d, float e, double f, struct s3 g,
				     struct s12 h, struct s24 i, long double j, unsigned char k, float l,
				     struct sfd m)
{
	return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g.a + g.b * 7 + g.c * 8 + h.a * 9 + h.b * 10 +
	       h.c * 11 + i.a * 12 + i.b * 13 + i.c * 14 + (double)j * 15 + k * 16 + l * 17 + m.f * 18 +
	       m.d * 19;
}
__attribute__((noinline)) double vsum(int n, ...)
{
	va_list ap;
	double s = 0;
	va_start(ap, n);
	for (int i = 0; i < n; i++) {
		switch (va_arg(ap, int)) {
		case 'l': s += va_arg(ap, long); break;
		case 'd': s += va_arg(ap, double); break;
		case 'q': { struct s16 x = va_arg(ap, struct s16); s += x.a * 3 + x.b * 5; break; }
		case 'w': { struct s24 x = va_arg(ap, struct s24); s += x.a * 7 + x.b * 11 + x.c * 13; break; }
		case 't': { struct s3 x = va_arg(ap, struct s3); s += x.a + x.b * 2 + x.c * 4; break; }
		}
	}
	va_end(ap);
	return s;
}

/* Python's configure test for complex support in libffi. */
__attribute__((noinline)) int z_is_expected(double _Complex z)
{
	const double _Complex expected = 1.25 - 0.5 * I;
	return z == expected;
}

static void call_cases(void)
{
	ffi_cif cif;
	ffi_type *at[16];
	void *av[16];
	ffi_arg r;

	unsigned top = 0x80000000u;
	at[0] = &ffi_type_uint;
	av[0] = &top;
	CHECK("is_u32_top", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_sint, at) == FFI_OK);
	ffi_call(&cif, FFI_FN(is_u32_top), &r, av);
	CHECKX("is_u32_top", r, 1);
	CHECK("ret_u32_top", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 0, &ffi_type_uint, NULL) == FFI_OK);
	ffi_call(&cif, FFI_FN(ret_u32_top), &r, NULL);
	CHECKX("ret_u32_top", r, 0x80000000u);

	struct s24 s = { 1, 2, 3 };
	long lr;
	at[0] = &t_s24; av[0] = &s;
	CHECK("take24", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_slong, at) == FFI_OK);
	ffi_call(&cif, FFI_FN(take24), &lr, av);
	CHECKX("take24", lr, 123);
	CHECK("take24-copy", s.a == 1 && s.b == 2 && s.c == 3);
	long a1 = 1, a2 = 2, a3 = 3, a4 = 4;
	at[0] = at[1] = at[2] = at[3] = &ffi_type_slong; at[4] = &t_s24;
	av[0] = &a1; av[1] = &a2; av[2] = &a3; av[3] = &a4; av[4] = &s;
	CHECK("take24_at5", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 5, &ffi_type_slong, at) == FFI_OK);
	ffi_call(&cif, FFI_FN(take24_at5), &lr, av);
	CHECKX("take24_at5", lr, take24_at5(1, 2, 3, 4, s));

	char a = -5; short b = -1234; int c = 100000; long d = -7; float e = 0.5f; double f = 1e10;
	struct s3 g = { 1, 2, 3 }; struct s12 h = { -1, 2, -3 }; struct s24 i = { 4, 5, 6 };
	long double j = -0.125L; unsigned char k = 200; float l = 3.25f; struct sfd m = { 1.5f, 2.5 };
	double want = mix(a, b, c, d, e, f, g, h, i, j, k, l, m), got = 0;
	at[0] = &ffi_type_schar; at[1] = &ffi_type_sshort; at[2] = &ffi_type_sint; at[3] = &ffi_type_slong;
	at[4] = &ffi_type_float; at[5] = &ffi_type_double; at[6] = &t_s3; at[7] = &t_s12; at[8] = &t_s24;
	at[9] = &ffi_type_longdouble; at[10] = &ffi_type_uchar; at[11] = &ffi_type_float; at[12] = &t_sfd;
	av[0] = &a; av[1] = &b; av[2] = &c; av[3] = &d; av[4] = &e; av[5] = &f; av[6] = &g; av[7] = &h;
	av[8] = &i; av[9] = &j; av[10] = &k; av[11] = &l; av[12] = &m;
	CHECK("mix", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 13, &ffi_type_double, at) == FFI_OK);
	ffi_call(&cif, FFI_FN(mix), &got, av);
	CHECK("mix", got == want);

	int n = 6, tl = 'l', td = 'd', tq = 'q', tw = 'w', tt = 't';
	long vl = 1000; double vd = 0.5; struct s16 vq = { 3, 4 }; struct s24 vw = { 5, 6, 7 }; struct s3 vt = { 8, 9, 10 };
	want = vsum(n, 'q', vq, 'l', vl, 'w', vw, 'd', vd, 't', vt, 'q', vq);
	at[0] = &ffi_type_sint;
	at[1] = &ffi_type_sint; at[2] = &t_s16; at[3] = &ffi_type_sint; at[4] = &ffi_type_slong;
	at[5] = &ffi_type_sint; at[6] = &t_s24; at[7] = &ffi_type_sint; at[8] = &ffi_type_double;
	at[9] = &ffi_type_sint; at[10] = &t_s3; at[11] = &ffi_type_sint; at[12] = &t_s16;
	av[0] = &n; av[1] = &tq; av[2] = &vq; av[3] = &tl; av[4] = &vl; av[5] = &tw; av[6] = &vw;
	av[7] = &td; av[8] = &vd; av[9] = &tt; av[10] = &vt; av[11] = &tq; av[12] = &vq;
	CHECK("vsum", ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, 1, 13, &ffi_type_double, at) == FFI_OK);
	got = 0;
	ffi_call(&cif, FFI_FN(vsum), &got, av);
	CHECK("vsum", got == want);
	/* a variadic call with n = 1: the home area alone */
	n = 1;
	want = vsum(n, 'd', vd);
	at[1] = &ffi_type_sint; at[2] = &ffi_type_double; av[1] = &td; av[2] = &vd;
	CHECK("vsum1", ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, 1, 3, &ffi_type_double, at) == FFI_OK);
	ffi_call(&cif, FFI_FN(vsum), &got, av);
	CHECK("vsum1", got == want);
	/* The same through a non-variadic cif (ctypes without argtypes):
	   undefined in C, but with every argument in R0-R3 it works as for
	   clang's calls without a prototype, the callee's home area falling
	   in the 32 bytes libffi reserves. */
	CHECK("vsum1-nonvar", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 3, &ffi_type_double, at) == FFI_OK);
	got = 0;
	ffi_call(&cif, FFI_FN(vsum), &got, av);
	CHECK("vsum1-nonvar", got == want);

	double _Complex z = 1.25 - 0.5 * I;
	at[0] = &ffi_type_complex_double;
	av[0] = &z;
	CHECK("z_is_expected", ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_sint, at) == FFI_OK);
	r = 0;
	ffi_call(&cif, FFI_FN(z_is_expected), &r, av);
	CHECKX("z_is_expected", r, 1);
}

/* ================= closures called by compiled code ================= */
static void id_handler(ffi_cif *cif, void *ret, void **args, void *ud)
{
	/* Copies the single argument to the result as an ffi_arg (or the
	   value itself for non-integers), as libffi's API says. */
	ffi_type *t = cif->arg_types[0];
	switch (t->type) {
	case FFI_TYPE_UINT8: *(ffi_arg *)ret = *(uint8_t *)args[0]; break;
	case FFI_TYPE_SINT8: *(ffi_sarg *)ret = *(int8_t *)args[0]; break;
	case FFI_TYPE_UINT16: *(ffi_arg *)ret = *(uint16_t *)args[0]; break;
	case FFI_TYPE_SINT16: *(ffi_sarg *)ret = *(int16_t *)args[0]; break;
	case FFI_TYPE_UINT32: *(ffi_arg *)ret = *(uint32_t *)args[0]; break;	/* zero-extended */
	case FFI_TYPE_SINT32: case FFI_TYPE_INT: *(ffi_sarg *)ret = *(int32_t *)args[0]; break;
	default: memcpy(ret, args[0], t->size); break;
	}
	(void)ud;
}
__attribute__((noinline)) uint8_t ref_u8(uint8_t x) { return x; }
__attribute__((noinline)) int8_t ref_s8(int8_t x) { return x; }
__attribute__((noinline)) uint16_t ref_u16(uint16_t x) { return x; }
__attribute__((noinline)) int16_t ref_s16(int16_t x) { return x; }
__attribute__((noinline)) uint32_t ref_u32(uint32_t x) { return x; }
__attribute__((noinline)) int32_t ref_s32(int32_t x) { return x; }

static void *make_closure(ffi_cif *cif, void (*fun)(ffi_cif *, void *, void **, void *), void *ud,
			  ffi_closure **pc)
{
	void *code = NULL;
	ffi_closure *c = ffi_closure_alloc(sizeof(ffi_closure), &code);
	if (!c || !code) {
		printf("FAIL ffi_closure_alloc\n");
		exit(1);
	}
	if (ffi_prep_closure_loc(c, cif, fun, ud, code) != FFI_OK) {
		printf("FAIL ffi_prep_closure_loc\n");
		exit(1);
	}
	*pc = c;
	return code;
}

/* closure (long, long, long, struct s16, long, struct s24, struct s12, float) -> long */
static void split_handler(ffi_cif *cif, void *ret, void **args, void *ud)
{
	struct s16 q = *(struct s16 *)args[3];
	struct s24 *w = args[5];
	struct s12 h = *(struct s12 *)args[6];
	long r = *(long *)args[0] + *(long *)args[1] * 2 + *(long *)args[2] * 3 + q.a * 4 + q.b * 5 +
		 *(long *)args[4] * 6 + w->a * 7 + w->b * 8 + w->c * 9 + h.a * 10 + h.b * 11 + h.c * 12 +
		 (long)(*(float *)args[7] * 4);
	w->a = -99;	/* the callee's copy: the caller's must not change */
	*(ffi_sarg *)ret = r;
	(void)cif; (void)ud;
}
/* closure (struct s24) -> struct s24 (sret), and -> struct s12, sfd, complex double */
static void agg_handler(ffi_cif *cif, void *ret, void **args, void *ud)
{
	struct s24 in = *(struct s24 *)args[0];
	switch ((int)(intptr_t)ud) {
	case 0: { struct s24 o = { in.c, in.b, in.a }; memcpy(ret, &o, sizeof o); break; }
	case 1: { struct s12 o = { (int)in.a, (int)in.b, -(int)in.c }; memcpy(ret, &o, sizeof o); break; }
	case 2: { struct sfd o = { (float)in.a, (double)in.b }; memcpy(ret, &o, sizeof o); break; }
	case 3: { double _Complex o = in.a + in.b * I; memcpy(ret, &o, sizeof o); break; }
	}
	(void)cif;
}
/* variadic closure long (int n, ...) over (int, long, double, struct s16, long, long, struct s24) */
static void var_handler(ffi_cif *cif, void *ret, void **args, void *ud)
{
	struct s16 q = *(struct s16 *)args[3];
	struct s24 w = *(struct s24 *)args[6];
	*(ffi_sarg *)ret = *(int *)args[0] + *(long *)args[1] * 2 + (long)(*(double *)args[2] * 4) +
			   q.a * 5 + q.b * 6 + *(long *)args[4] * 7 + *(long *)args[5] * 8 + w.a * 9 +
			   w.b * 10 + w.c * 11;
	CHECK("var-closure-nargs", cif->nargs == 7);
	(void)ud;
}
/* many arguments: (13 of mix's types) -> double, computed like mix */
static void mix_handler(ffi_cif *cif, void *ret, void **a, void *ud)
{
	*(double *)ret = mix(*(char *)a[0], *(short *)a[1], *(int *)a[2], *(long *)a[3], *(float *)a[4],
			     *(double *)a[5], *(struct s3 *)a[6], *(struct s12 *)a[7], *(struct s24 *)a[8],
			     *(long double *)a[9], *(unsigned char *)a[10], *(float *)a[11], *(struct sfd *)a[12]);
	(void)cif; (void)ud;
}
static void float_handler(ffi_cif *cif, void *ret, void **a, void *ud)
{
	*(float *)ret = *(float *)a[0] * *(float *)a[1] + (float)*(double *)a[2];
	(void)cif; (void)ud;
}
static void index_handler(ffi_cif *cif, void *ret, void **a, void *ud)
{
	*(ffi_sarg *)ret = (intptr_t)ud * 3 + *(int *)a[0];
	(void)cif;
}
static void other_handler(ffi_cif *cif, void *ret, void **a, void *ud)
{
	*(ffi_sarg *)ret = -1000 - *(int *)a[0];
	(void)cif; (void)ud;
}

static void closure_cases(void)
{
	ffi_cif cif;
	ffi_type *at[16];
	ffi_closure *cl;
	void *code;

	/* Each narrow integer type: the closure's raw R12 must be what clang's
	   identity function returns for the same raw R0. */
	struct {
		const char *name;
		ffi_type *t;
		void *ref;
		uint64_t in;
	} narrow[] = {
		{ "cl-u8", &ffi_type_uint8, ref_u8, 0xF0 },
		{ "cl-s8", &ffi_type_sint8, ref_s8, (uint64_t)-16 },
		{ "cl-u16", &ffi_type_uint16, ref_u16, 0xF00F },
		{ "cl-s16", &ffi_type_sint16, ref_s16, (uint64_t)-300 },
		{ "cl-u32", &ffi_type_uint32, ref_u32, 0xFFFFFFFF80000000ULL },
		{ "cl-s32", &ffi_type_sint32, ref_s32, 0xFFFFFFFF80000001ULL },
		{ "cl-u32-small", &ffi_type_uint32, ref_u32, 0x7FFFFFFF },
	};
	for (unsigned i = 0; i < sizeof narrow / sizeof narrow[0]; i++) {
		struct kl_raw rr = { narrow[i].ref, { narrow[i].in, 0, 0, 0 }, { 0, 0 } };
		struct kl_raw rc;
		at[0] = narrow[i].t;
		CHECK(narrow[i].name, ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, narrow[i].t, at) == FFI_OK);
		code = make_closure(&cif, id_handler, NULL, &cl);
		kl_call_raw(&rr);
		rc = rr;
		rc.fn = code;
		rc.out[0] = 0xA5A5A5A5A5A5A5A5ULL;
		kl_call_raw(&rc);
		CHECKX(narrow[i].name, rc.out[0], rr.out[0]);
		CHECKX(narrow[i].name, rr.out[0], narrow[i].in);	/* clang: already in ABI form */
		ffi_closure_free(cl);
	}
	/* unsigned int through compiled code: compare against a constant. */
	at[0] = &ffi_type_uint32;
	ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_uint32, at);
	{
		unsigned (*fn)(unsigned) = (unsigned (*)(unsigned))make_closure(&cif, id_handler, NULL, &cl);
		volatile unsigned x = 0x80000000u;
		CHECK("cl-u32-compiled", fn(x) == 0x80000000u);
		CHECK("cl-u32-compiled", is_u32_top(fn(x)));
		CHECK("cl-u32-compiled", (unsigned long)fn(x) == 0x80000000ul);
		ffi_closure_free(cl);
	}
	/* float */
	at[0] = at[1] = &ffi_type_float; at[2] = &ffi_type_double;
	ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 3, &ffi_type_float, at);
	{
		float (*fn)(float, float, double) = (float (*)(float, float, double))make_closure(&cif, float_handler, NULL, &cl);
		volatile float a = 1.5f, b = -2.0f;
		CHECK("cl-float", fn(a, b, 0.25) == -2.75f);
		ffi_closure_free(cl);
	}
	/* splits, by-reference, a float on the stack */
	at[0] = at[1] = at[2] = at[4] = &ffi_type_slong; at[3] = &t_s16; at[5] = &t_s24; at[6] = &t_s12;
	at[7] = &ffi_type_float;
	ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 8, &ffi_type_slong, at);
	{
		long (*fn)(long, long, long, struct s16, long, struct s24, struct s12, float) =
			(long (*)(long, long, long, struct s16, long, struct s24, struct s12, float))make_closure(
				&cif, split_handler, NULL, &cl);
		struct s16 q = { 10, 20 };
		struct s24 w = { 30, 40, 50 };
		struct s12 h = { 60, -70, 80 };
		long want = 1 + 2 * 2 + 3 * 3 + 10 * 4 + 20 * 5 + 6 * 6 + 30 * 7 + 40 * 8 + 50 * 9 + 60 * 10 -
			    70 * 11 + 80 * 12 + 10;
		CHECKX("cl-split", fn(1, 2, 3, q, 6, w, h, 2.5f), want);
		CHECK("cl-split-copy", w.a == 30);
		ffi_closure_free(cl);
	}
	/* aggregate results: sret and R12/R11 */
	at[0] = &t_s24;
	{
		struct s24 in = { 1, 2, 3 };
		ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &t_s24, at);
		struct s24 (*f0)(struct s24) = (struct s24 (*)(struct s24))make_closure(&cif, agg_handler, (void *)0, &cl);
		struct s24 o0 = f0(in);
		CHECK("cl-ret-s24", o0.a == 3 && o0.b == 2 && o0.c == 1);
		ffi_closure_free(cl);
		ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &t_s12, at);
		struct s12 (*f1)(struct s24) = (struct s12 (*)(struct s24))make_closure(&cif, agg_handler, (void *)1, &cl);
		struct s12 o1 = f1(in);
		CHECK("cl-ret-s12", o1.a == 1 && o1.b == 2 && o1.c == -3);
		ffi_closure_free(cl);
		ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &t_sfd, at);
		struct sfd (*f2)(struct s24) = (struct sfd (*)(struct s24))make_closure(&cif, agg_handler, (void *)2, &cl);
		struct sfd o2 = f2(in);
		CHECK("cl-ret-sfd", o2.f == 1.0f && o2.d == 2.0);
		ffi_closure_free(cl);
		ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_complex_double, at);
		double _Complex (*f3)(struct s24) = (double _Complex (*)(struct s24))make_closure(&cif, agg_handler, (void *)3, &cl);
		double _Complex o3 = f3(in);
		CHECK("cl-ret-cdouble", creal(o3) == 1.0 && cimag(o3) == 2.0);
		ffi_closure_free(cl);
	}
	/* a variadic closure called by compiled code (home area) */
	at[0] = &ffi_type_sint; at[1] = &ffi_type_slong; at[2] = &ffi_type_double; at[3] = &t_s16;
	at[4] = at[5] = &ffi_type_slong; at[6] = &t_s24;
	CHECK("cl-var", ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, 1, 7, &ffi_type_slong, at) == FFI_OK);
	{
		long (*fn)(int, ...) = (long (*)(int, ...))make_closure(&cif, var_handler, NULL, &cl);
		struct s16 q = { 3, 4 };
		struct s24 w = { 5, 6, 7 };
		long want = 1 + 2 * 2 + 2 + 3 * 5 + 4 * 6 + 8 * 7 + 9 * 8 + 5 * 9 + 6 * 10 + 7 * 11;
		CHECKX("cl-var", fn(1, 2L, 0.5, q, 8L, 9L, w), want);
		ffi_closure_free(cl);
	}
	/* many arguments */
	at[0] = &ffi_type_schar; at[1] = &ffi_type_sshort; at[2] = &ffi_type_sint; at[3] = &ffi_type_slong;
	at[4] = &ffi_type_float; at[5] = &ffi_type_double; at[6] = &t_s3; at[7] = &t_s12; at[8] = &t_s24;
	at[9] = &ffi_type_longdouble; at[10] = &ffi_type_uchar; at[11] = &ffi_type_float; at[12] = &t_sfd;
	ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 13, &ffi_type_double, at);
	{
		double (*fn)(char, short, int, long, float, double, struct s3, struct s12, struct s24, long double,
			     unsigned char, float, struct sfd) =
			(double (*)(char, short, int, long, float, double, struct s3, struct s12, struct s24,
				    long double, unsigned char, float, struct sfd))make_closure(&cif, mix_handler, NULL, &cl);
		struct s3 g = { 1, 2, 3 }; struct s12 h = { -1, 2, -3 }; struct s24 i = { 4, 5, 6 };
		struct sfd m = { 1.5f, 2.5 };
		double want = mix(-5, -1234, 100000, -7, 0.5f, 1e10, g, h, i, -0.125L, 200, 3.25f, m);
		CHECK("cl-mix", fn(-5, -1234, 100000, -7, 0.5f, 1e10, g, h, i, -0.125L, 200, 3.25f, m) == want);
		ffi_closure_free(cl);
	}
	/* Many closures over several 64 KiB pages, freed and allocated again;
	   and one closure prepared again with another function. */
	at[0] = &ffi_type_sint;
	ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 1, &ffi_type_slong, at);
	enum { N = 3000 };
	static ffi_closure *cls[N];
	static long (*fns[N])(int);
	for (int round = 0; round < 2; round++) {
		int bad = 0;
		for (int i = 0; i < N; i++)
			fns[i] = (long (*)(int))make_closure(&cif, index_handler, (void *)(intptr_t)(i + round), &cls[i]);
		for (int i = 0; i < N; i++)
			bad += fns[i](round) != (long)(i + round) * 3 + round;
		CHECK(round ? "cl-many-again" : "cl-many", bad == 0);
		if (round == 0) {
			for (int i = 0; i < N; i++)
				ffi_closure_free(cls[i]);
		}
	}
	CHECK("cl-reprep", ffi_prep_closure_loc(cls[5], &cif, other_handler, NULL, (void *)fns[5]) == FFI_OK);
	CHECKX("cl-reprep", fns[5](7), -1007);
	CHECKX("cl-reprep-neighbour", fns[6](1), (6 + 1) * 3 + 1);
	for (int i = 0; i < N; i++)
		ffi_closure_free(cls[i]);
}

int main(void)
{
	args_cases();
	ret_cases();
	call_cases();
	closure_cases();
	printf("DONE checks=%d failures=%d\n", checks, failures);
	return failures != 0;
}
