/*
 * dhrystone.c — Dhrystone 2.1 integer CPU benchmark for KlaussCPU (llext).
 *
 * Dhrystone is the classic, public-domain, integer benchmark (Reinhold P.
 * Weicker, 1984; C translation by Rick Richardson).  It reports a standard
 * figure of merit: Dhrystones/second, VAX-relative DMIPS (Dhrystones/s ÷ 1757),
 * and DMIPS/MHz — the number most often quoted for embedded cores.
 *
 * This is the unmodified Dhrystone 2.1 workload (Proc_0..8, Func_1..3, the
 * documented record/array/string mix), with three KlaussCPU adaptations:
 *   - Timing uses the hardware cycle counter (REG_PERF_CYCLES, 0xF00D) instead
 *     of POSIX times()/clock(); also reads REG_PERF_INSTR for cycles- and
 *     instructions-per-Dhrystone.  Most accurate bare-metal; over `run` it also
 *     counts the timer ISR + any SSH/network activity, so run an idle board.
 *   - Run count is auto-calibrated to a ~3 s window so the result is stable
 *     regardless of clock speed (no interactive prompt — `run` passes no argv).
 *   - Output is integer fixed-point (no float printf); 64-bit values print with
 *     %lu (KlaussCPU `long` is 64-bit; picolibc has no `ll` modifier).
 *
 * Build:  cd baremetal && make dhrystone.llext   (copy to SD, `run dhrystone.llext`)
 * Symbols resolved against the kernel: printf, malloc, strcpy, strcmp, memcpy.
 */

#include "../../mmio.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLOCK_HZ        100000000ull   /* KlaussCPU core clock (100 MHz)      */
#define VAX_DHRY_PER_S  1757ull        /* 1 VAX-11/780 DMIPS = 1757 Dhry/s    */
#define CAL_RUNS        100000         /* calibration pass iterations         */
#define TARGET_CYCLES   300000000ull   /* aim the measured run at ~3 s        */
#define MAX_RUNS        100000000      /* clamp                               */

#define U(x) ((unsigned long)(x))      /* cast for %lu (64-bit)               */

/* ── Dhrystone types (dhry.h) ────────────────────────────────────────────── */

#define Null 0
#define true  1
#define false 0

typedef enum { Ident_1, Ident_2, Ident_3, Ident_4, Ident_5 } Enumeration;
typedef int  One_Thirty;
typedef int  One_Fifty;
typedef char Capital_Letter;
typedef int  Boolean;
typedef char Str_30[31];
typedef int  Arr_1_Dim[50];
typedef int  Arr_2_Dim[50][50];

typedef struct record {
	struct record *Ptr_Comp;
	Enumeration    Discr;
	union {
		struct {
			Enumeration Enum_Comp;
			int         Int_Comp;
			char        Str_Comp[31];
		} var_1;
		struct {
			Enumeration E_Comp_2;
			char        Str_2_Comp[31];
		} var_2;
		struct {
			char Ch_1_Comp;
			char Ch_2_Comp;
		} var_3;
	} variant;
} Rec_Type, *Rec_Pointer;

/* ── globals (dhry_1.c) ──────────────────────────────────────────────────── */

static Rec_Pointer Ptr_Glob, Next_Ptr_Glob;
static int         Int_Glob;
static Boolean     Bool_Glob;
static char        Ch_1_Glob, Ch_2_Glob;
static int         Arr_1_Glob[50];
static int         Arr_2_Glob[50][50];

/* post-loop locals captured for the verification print */
static struct {
	int Int_1_Loc, Int_2_Loc, Int_3_Loc;
	Enumeration Enum_Loc;
	char Str_1_Loc[31], Str_2_Loc[31];
} Chk;

/* ── procedures / functions (dhry_2.c) ───────────────────────────────────── */

static Boolean Func_3(Enumeration Enum_Par_Val)
{
	Enumeration Enum_Loc = Enum_Par_Val;

	return Enum_Loc == Ident_3 ? true : false;
}

static void Proc_7(One_Fifty Int_1_Par_Val, One_Fifty Int_2_Par_Val,
		   One_Fifty *Int_Par_Ref)
{
	One_Fifty Int_Loc = Int_1_Par_Val + 2;

	*Int_Par_Ref = Int_2_Par_Val + Int_Loc;
}

static void Proc_6(Enumeration Enum_Val_Par, Enumeration *Enum_Ref_Par)
{
	*Enum_Ref_Par = Enum_Val_Par;
	if (!Func_3(Enum_Val_Par)) {
		*Enum_Ref_Par = Ident_4;
	}
	switch (Enum_Val_Par) {
	case Ident_1: *Enum_Ref_Par = Ident_1; break;
	case Ident_2: *Enum_Ref_Par = (Int_Glob > 100) ? Ident_1 : Ident_4; break;
	case Ident_3: *Enum_Ref_Par = Ident_2; break;
	case Ident_4: break;
	case Ident_5: *Enum_Ref_Par = Ident_3; break;
	}
}

static void Proc_3(Rec_Pointer *Ptr_Ref_Par)
{
	if (Ptr_Glob != Null) {
		*Ptr_Ref_Par = Ptr_Glob->Ptr_Comp;
	}
	Proc_7(10, Int_Glob, &Ptr_Glob->variant.var_1.Int_Comp);
}

static void Proc_1(Rec_Pointer Ptr_Val_Par)
{
	Rec_Pointer Next_Record = Ptr_Val_Par->Ptr_Comp;

	*Ptr_Val_Par->Ptr_Comp = *Ptr_Glob;
	Ptr_Val_Par->variant.var_1.Int_Comp = 5;
	Next_Record->variant.var_1.Int_Comp = Ptr_Val_Par->variant.var_1.Int_Comp;
	Next_Record->Ptr_Comp = Ptr_Val_Par->Ptr_Comp;
	Proc_3(&Next_Record->Ptr_Comp);
	if (Next_Record->Discr == Ident_1) {
		Next_Record->variant.var_1.Int_Comp = 6;
		Proc_6(Ptr_Val_Par->variant.var_1.Enum_Comp,
		       &Next_Record->variant.var_1.Enum_Comp);
		Next_Record->Ptr_Comp = Ptr_Glob->Ptr_Comp;
		Proc_7(Next_Record->variant.var_1.Int_Comp, 10,
		       &Next_Record->variant.var_1.Int_Comp);
	} else {
		*Ptr_Val_Par = *Ptr_Val_Par->Ptr_Comp;
	}
}

static void Proc_2(One_Fifty *Int_Par_Ref)
{
	One_Fifty   Int_Loc = *Int_Par_Ref + 10;
	Enumeration Enum_Loc = Ident_2;

	do {
		if (Ch_1_Glob == 'A') {
			Int_Loc -= 1;
			*Int_Par_Ref = Int_Loc - Int_Glob;
			Enum_Loc = Ident_1;
		}
	} while (Enum_Loc != Ident_1);
}

static void Proc_4(void)
{
	Boolean Bool_Loc = (Ch_1_Glob == 'A');

	Bool_Glob = Bool_Loc | Bool_Glob;
	Ch_2_Glob = 'B';
}

static void Proc_5(void)
{
	Ch_1_Glob = 'A';
	Bool_Glob = false;
}

static void Proc_8(Arr_1_Dim Arr_1_Par_Ref, Arr_2_Dim Arr_2_Par_Ref,
		   int Int_1_Par_Val, int Int_2_Par_Val)
{
	One_Fifty Int_Index;
	One_Fifty Int_Loc = Int_1_Par_Val + 5;

	Arr_1_Par_Ref[Int_Loc] = Int_2_Par_Val;
	Arr_1_Par_Ref[Int_Loc + 1] = Arr_1_Par_Ref[Int_Loc];
	Arr_1_Par_Ref[Int_Loc + 30] = Int_Loc;
	for (Int_Index = Int_Loc; Int_Index <= Int_Loc + 1; ++Int_Index) {
		Arr_2_Par_Ref[Int_Loc][Int_Index] = Int_Loc;
	}
	Arr_2_Par_Ref[Int_Loc][Int_Loc - 1] += 1;
	Arr_2_Par_Ref[Int_Loc + 20][Int_Loc] = Arr_1_Par_Ref[Int_Loc];
	Int_Glob = 5;
}

static Enumeration Func_1(Capital_Letter Ch_1_Par_Val, Capital_Letter Ch_2_Par_Val)
{
	Capital_Letter Ch_1_Loc = Ch_1_Par_Val;
	Capital_Letter Ch_2_Loc = Ch_1_Loc;

	if (Ch_2_Loc != Ch_2_Par_Val) {
		return Ident_1;
	}
	Ch_1_Glob = Ch_1_Loc;
	return Ident_2;
}

static Boolean Func_2(Str_30 Str_1_Par_Ref, Str_30 Str_2_Par_Ref)
{
	One_Thirty     Int_Loc = 2;
	Capital_Letter Ch_Loc = 'A';

	while (Int_Loc <= 2) {
		if (Func_1(Str_1_Par_Ref[Int_Loc], Str_2_Par_Ref[Int_Loc + 1]) ==
		    Ident_1) {
			Ch_Loc = 'A';
			Int_Loc += 1;
		}
	}
	if (Ch_Loc >= 'W' && Ch_Loc < 'Z') {
		Int_Loc = 7;
	}
	if (Ch_Loc == 'R') {
		return true;
	}
	if (strcmp(Str_1_Par_Ref, Str_2_Par_Ref) > 0) {
		Int_Loc += 7;
		Int_Glob = Int_Loc;
		return true;
	}
	return false;
}

/* ── the measured workload (Proc_0 body) ─────────────────────────────────── */

static void dhry_run(int Number_Of_Runs)
{
	One_Fifty   Int_1_Loc, Int_2_Loc, Int_3_Loc;
	char        Ch_Index;
	Enumeration Enum_Loc;
	Str_30      Str_1_Loc, Str_2_Loc;
	int         Run_Index;

	/* Initializations (canonical, before the timed loop). */
	Ptr_Glob->Ptr_Comp = Next_Ptr_Glob;
	Ptr_Glob->Discr = Ident_1;
	Ptr_Glob->variant.var_1.Enum_Comp = Ident_3;
	Ptr_Glob->variant.var_1.Int_Comp = 40;
	strcpy(Ptr_Glob->variant.var_1.Str_Comp,
	       "DHRYSTONE PROGRAM, SOME STRING");
	strcpy(Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING");
	Arr_2_Glob[8][7] = 10;

	for (Run_Index = 1; Run_Index <= Number_Of_Runs; ++Run_Index) {
		Proc_5();
		Proc_4();
		Int_1_Loc = 2;
		Int_2_Loc = 3;
		strcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING");
		Enum_Loc = Ident_2;
		Bool_Glob = !Func_2(Str_1_Loc, Str_2_Loc);
		while (Int_1_Loc < Int_2_Loc) {
			Int_3_Loc = 5 * Int_1_Loc - Int_2_Loc;
			Proc_7(Int_1_Loc, Int_2_Loc, &Int_3_Loc);
			Int_1_Loc += 1;
		}
		Proc_8(Arr_1_Glob, Arr_2_Glob, Int_1_Loc, Int_3_Loc);
		Proc_1(Ptr_Glob);
		for (Ch_Index = 'A'; Ch_Index <= Ch_2_Glob; ++Ch_Index) {
			if (Enum_Loc == Func_1(Ch_Index, 'C')) {
				Proc_6(Ident_1, &Enum_Loc);
				strcpy(Str_2_Loc,
				       "DHRYSTONE PROGRAM, 3'RD STRING");
				Int_2_Loc = Run_Index;
				Int_Glob = Run_Index;
			}
		}
		Int_2_Loc = Int_2_Loc * Int_1_Loc;
		Int_1_Loc = Int_2_Loc / Int_3_Loc;
		Int_2_Loc = 7 * (Int_2_Loc - Int_3_Loc) - Int_1_Loc;
		Proc_2(&Int_1_Loc);
	}

	/* Capture post-loop locals for verification. */
	Chk.Int_1_Loc = Int_1_Loc;
	Chk.Int_2_Loc = Int_2_Loc;
	Chk.Int_3_Loc = Int_3_Loc;
	Chk.Enum_Loc = Enum_Loc;
	strcpy(Chk.Str_1_Loc, Str_1_Loc);
	strcpy(Chk.Str_2_Loc, Str_2_Loc);
}

/* ── reporting ───────────────────────────────────────────────────────────── */

/* print a milli-unit fixed-point value: e.g. milli=3791 -> "3.791" */
static void milli(uint64_t m)
{
	printf("%lu.%03lu", U(m / 1000), U(m % 1000));
}

/* Verify the documented Dhrystone 2.1 end state (the Ch_Index loop only runs
 * 'A'..'B' since Ch_2_Glob=='B', so its body never fires — hence Enum_Loc stays
 * Ident_2 and Str_2_Loc stays the "2'ND STRING").  Returns 1 if all match. */
static int chk_int(const char *name, int got, int want)
{
	int ok = (got == want);

	printf("  %-18s %-12d %s\n", name, got, ok ? "ok" : "MISMATCH");
	return ok;
}

static int verify(int runs)
{
	int ok = 1;

	puts("Verification (expected Dhrystone 2.1 end state):");
	ok &= chk_int("Int_Glob", Int_Glob, 5);
	ok &= chk_int("Bool_Glob", Bool_Glob, 1);
	ok &= chk_int("Ch_1_Glob", Ch_1_Glob, 'A');
	ok &= chk_int("Ch_2_Glob", Ch_2_Glob, 'B');
	ok &= chk_int("Arr_1_Glob[8]", Arr_1_Glob[8], 7);
	ok &= chk_int("Arr_2_Glob[8][7]", Arr_2_Glob[8][7], runs + 10);
	ok &= chk_int("Ptr_Glob.Discr", Ptr_Glob->Discr, Ident_1);
	ok &= chk_int("Ptr_Glob.Int_Comp", Ptr_Glob->variant.var_1.Int_Comp, 17);
	ok &= chk_int("Next.Int_Comp",
		      Next_Ptr_Glob->variant.var_1.Int_Comp, 18);
	ok &= chk_int("Int_1_Loc", Chk.Int_1_Loc, 5);
	ok &= chk_int("Int_2_Loc", Chk.Int_2_Loc, 13);
	ok &= chk_int("Int_3_Loc", Chk.Int_3_Loc, 7);
	ok &= chk_int("Enum_Loc", Chk.Enum_Loc, Ident_2);

	int s1 = (strcmp(Chk.Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING") == 0);
	int s2 = (strcmp(Chk.Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING") == 0);

	printf("  %-18s %-12s %s\n", "Str_1_Loc", Chk.Str_1_Loc,
	       s1 ? "ok" : "MISMATCH");
	printf("  %-18s %-12s %s\n", "Str_2_Loc", Chk.Str_2_Loc,
	       s2 ? "ok" : "MISMATCH");
	return ok && s1 && s2;
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	Next_Ptr_Glob = (Rec_Pointer)malloc(sizeof(Rec_Type));
	Ptr_Glob = (Rec_Pointer)malloc(sizeof(Rec_Type));
	if (Next_Ptr_Glob == Null || Ptr_Glob == Null) {
		puts("dhrystone: out of memory");
		return 1;
	}

	puts("Dhrystone Benchmark, Version 2.1 (Language: C) — KlaussCPU");
	puts("Timing via hardware cycle counter (REG_PERF_CYCLES @ 100 MHz).");

	/* Calibrate the run count to ~TARGET_CYCLES so the result is stable. */
	uint64_t c0 = REG_PERF_CYCLES;

	dhry_run(CAL_RUNS);
	uint64_t cal_cyc = REG_PERF_CYCLES - c0;

	if (cal_cyc == 0) {
		cal_cyc = 1;
	}
	uint64_t runs64 = (TARGET_CYCLES * (uint64_t)CAL_RUNS) / cal_cyc;

	if (runs64 < (uint64_t)CAL_RUNS) {
		runs64 = CAL_RUNS;
	}
	if (runs64 > (uint64_t)MAX_RUNS) {
		runs64 = MAX_RUNS;
	}
	int runs = (int)runs64;

	printf("Calibrated: %lu runs (~%lu cycles/run in warm-up)\n",
	       U(runs), U(cal_cyc / CAL_RUNS));
	printf("Running %lu Dhrystone iterations ...\n", U(runs));

	/* Measured run. */
	uint64_t cyc0 = REG_PERF_CYCLES;
	uint64_t ins0 = REG_PERF_INSTR;

	dhry_run(runs);

	uint64_t cyc = REG_PERF_CYCLES - cyc0;
	uint64_t ins = REG_PERF_INSTR - ins0;

	if (cyc == 0) {
		cyc = 1;
	}

	/* Derived metrics (integer fixed-point, ×1000 where noted). */
	uint64_t dhry_per_s = ((uint64_t)runs * CLOCK_HZ) / cyc;
	uint64_t dmips_milli = (dhry_per_s * 1000ull) / VAX_DHRY_PER_S;
	uint64_t dmips_mhz_milli = dmips_milli / (CLOCK_HZ / 1000000ull);
	uint64_t cyc_per_dhry_milli = (cyc * 1000ull) / (uint64_t)runs;
	uint64_t ins_per_dhry_milli = (ins * 1000ull) / (uint64_t)runs;
	uint64_t us_per_dhry_milli =
		(cyc * 1000ull) / ((uint64_t)runs * (CLOCK_HZ / 1000000ull));

	int ok = verify(runs);

	puts("");
	puts("=== Results ===========================================");
	printf("Runs                 : %lu\n", U(runs));
	printf("Cycles               : %lu\n", U(cyc));
	printf("Instructions         : %lu\n", U(ins));
	printf("Microseconds/Dhry    : "); milli(us_per_dhry_milli); puts("");
	printf("Cycles/Dhrystone     : "); milli(cyc_per_dhry_milli); puts("");
	printf("Instr/Dhrystone      : "); milli(ins_per_dhry_milli); puts("");
	printf("Dhrystones/second    : %lu\n", U(dhry_per_s));
	printf("DMIPS (VAX MIPS)     : "); milli(dmips_milli); puts("");
	printf("DMIPS/MHz            : "); milli(dmips_mhz_milli); puts("");
	printf("Self-check           : %s\n", ok ? "PASS" : "FAIL");
	puts("=======================================================");

	return ok ? 0 : 1;
}
