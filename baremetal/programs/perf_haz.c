// perf_haz.c — perf_baseline.c + M6/M7 pipeline HAZARD-counter attribution.
//
// Identical micro-kernels and measurement method to perf_baseline.c (see that
// file's header), but the per-kernel snapshot ALSO reads the pipeline hazard
// counters at 0xF00D_00B0..00E8 (added in pipeline M6c; documented in
// MMIO_MAP.md) and prints them, so hazard attribution is measured on the REAL
// compiled kernels with the REAL 256 KiB working set — not the hand-assembled
// micro-kernel proxy in fpga/KlaussCPU/perf/m7/haz_probe_*.kla.
//
// WHY a separate program (not an edit to perf_baseline.c): perf_baseline.elf is
// the PINNED CPI A/B reference (perf/METHOD.md "stale-ELF trap") — recompiling it
// would shift code/heap layout and change the memory kernels' miss pattern,
// breaking the M6-vs-M7 CPI comparison.  perf_haz is its own ELF; pin it once and
// run it on both bitstreams for a true hazard A/B.
//
// The hazard counters are free-running and cleared by the same PERF_CTRL_CLEAR
// pulse bench() already issues, and bench() already snapshots every counter
// BEFORE printing — so reading 8 more in the snapshot is non-perturbing.
//
// Each kernel emits, in addition to perf_baseline's "CSV," line, a machine-
// readable "HAZ," line:
//   HAZ,<name>,<cycles>,<instr>,<data>,<loaduse>,<flags>,<sp>,<muldiv>,
//       <brflush>,<ifmiss>,<memwait>
// (raw cycle counts; percentages are printed in the human "haz:" line).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../mmio.h"

// ── M6/M7 pipeline hazard counters (not yet in mmio.h) ─────────────────────────
// 48-bit counters in 64-bit MMIO slots; PERF_CTRL_CLEAR zeroes them too.
#define REG_PERF_STALL_DATA    (*(volatile uint64_t *)(PERF_BASE + 0x00B0u)) /* GPR RAW  */
#define REG_PERF_STALL_LOADUSE (*(volatile uint64_t *)(PERF_BASE + 0x00B8u)) /* load-use */
#define REG_PERF_STALL_FLAGS   (*(volatile uint64_t *)(PERF_BASE + 0x00C0u)) /* flag rd  */
#define REG_PERF_STALL_SP      (*(volatile uint64_t *)(PERF_BASE + 0x00C8u)) /* SP ser.  */
#define REG_PERF_STALL_MULDIV  (*(volatile uint64_t *)(PERF_BASE + 0x00D0u)) /* EX busy  */
#define REG_PERF_BRANCH_FLUSH  (*(volatile uint64_t *)(PERF_BASE + 0x00D8u)) /* redirects*/
#define REG_PERF_IF_MISS       (*(volatile uint64_t *)(PERF_BASE + 0x00E0u)) /* fetch idle*/
#define REG_PERF_MEM_WAIT      (*(volatile uint64_t *)(PERF_BASE + 0x00E8u)) /* MEM port */

typedef uint64_t u64;
typedef uint32_t u32;

#define U(x) ((unsigned long)(x))   /* cast for %lu */

/* ── tunables — identical to perf_baseline.c ─────────────────────────────────── */
#define ALU_ITERS     500000u
#define MEM_WORDS     32768u        /* 256 KiB working set (> 64 KiB L1)      */
#define MEM_REPS      4u
#define CHASE_STEPS   200000u
#define BR_ITERS      500000u
#define FIB_N         28u
#define MD_ITERS      50000u

static volatile u64 g_sink;
static u64 *g_buf;
static u32  g_words;

typedef struct {
    u64 ms, cycles, instr;
    u64 fetch, exec, mul, div, intc, idle;
    u64 mul_ops, div_ops, int_ops;
    u64 alu, load, store, branch, taken, jump, call, indirect, other;
    u64 rh, rm, wh, wm, wb, stall;
    u64 fastpath;
    /* M6/M7 pipeline hazard attribution */
    u64 h_data, h_loaduse, h_flags, h_sp, h_muldiv, h_brflush, h_ifmiss, h_memwait;
} snap_t;

/* ── fixed-point printers ────────────────────────────────────────────────────── */

static void pct(u64 num, u64 den) {
    u64 p = den ? (num * 10000ull) / den : 0;
    printf("%lu.%02lu%%", U(p / 100), U(p % 100));
}

static void cyc2(u64 num, u64 den) {
    u64 v = den ? (num * 100ull) / den : 0;
    printf("%lu.%02luc", U(v / 100), U(v % 100));
}

static void report(const char *name, const snap_t *s) {
    u64 misses   = s->rm + s->wm;
    u64 accesses = s->rh + s->rm + s->wh + s->wm;
    u64 mix      = s->alu + s->load + s->store + s->branch +
                   s->jump + s->call + s->indirect + s->other;
    u64 cpi_m    = s->instr ? (s->cycles * 1000ull) / s->instr : 0;

    printf("\n--- %s ---\n", name);
    printf("time=%lums cycles=%lu instr=%lu CPI=%lu.%03lu\n",
           U(s->ms), U(s->cycles), U(s->instr), U(cpi_m / 1000), U(cpi_m % 1000));
    printf("FASTPATH=%lu  (instrs that skipped FETCH2)\n", U(s->fastpath));

    printf("mix:  ALU=");  pct(s->alu, s->instr);
    printf(" LD=");        pct(s->load, s->instr);
    printf(" ST=");        pct(s->store, s->instr);
    printf(" BR=");        pct(s->branch, s->instr);
    printf(" JMP=");       pct(s->jump, s->instr);
    printf(" CALL=");      pct(s->call, s->instr);
    printf(" IND=");       pct(s->indirect, s->instr);
    printf(" OTH=");       pct(s->other, s->instr);
    printf("\n");
    printf("      (mix sum=%lu instr=%lu %s)\n",
           U(mix), U(s->instr), mix == s->instr ? "ok" : "MISMATCH");

    printf("branch: n=%lu taken=%lu rate=", U(s->branch), U(s->taken));
    pct(s->taken, s->branch);
    printf("\n");

    printf("cyc:  fetch="); pct(s->fetch, s->cycles);
    printf(" exec=");       pct(s->exec, s->cycles);
    printf(" mul=");        pct(s->mul, s->cycles);
    printf(" div=");        pct(s->div, s->cycles);
    printf(" int=");        pct(s->intc, s->cycles);
    printf(" idle=");       pct(s->idle, s->cycles);
    printf("\n");

    printf("cache: acc=%lu rdH=%lu rdM=%lu wrH=%lu wrM=%lu wb=%lu miss=",
           U(accesses), U(s->rh), U(s->rm), U(s->wh), U(s->wm), U(s->wb));
    pct(misses, accesses);
    printf(" stall=%lu", U(s->stall));
    if (misses) { printf(" avgpen="); cyc2(s->stall, misses); }
    printf("\n");

    /* M6/M7 pipeline hazard attribution — each as % of cycles */
    printf("haz:  DATA=");    pct(s->h_data,    s->cycles);
    printf(" LOADUSE=");      pct(s->h_loaduse, s->cycles);
    printf(" FLAGS=");        pct(s->h_flags,   s->cycles);
    printf(" SP=");           pct(s->h_sp,      s->cycles);
    printf(" MULDIV=");       pct(s->h_muldiv,  s->cycles);
    printf(" BRFLUSH=");      pct(s->h_brflush, s->cycles);
    printf(" IFMISS=");       pct(s->h_ifmiss,  s->cycles);
    printf(" MEMWAIT=");      pct(s->h_memwait, s->cycles);
    printf("\n");

    if (s->mul_ops) { printf("mul: ops=%lu avglat=", U(s->mul_ops)); cyc2(s->mul, s->mul_ops); printf("\n"); }
    if (s->div_ops) { printf("div: ops=%lu avglat=", U(s->div_ops)); cyc2(s->div, s->div_ops); printf("\n"); }

    u64 missrate_bp = accesses ? (misses * 10000ull) / accesses : 0;
    u64 taken_bp    = s->branch ? (s->taken * 10000ull) / s->branch : 0;
    printf("CSV,%s,%lu,%lu,%lu,%lu,%lu,%lu\n",
           name, U(s->ms), U(s->cycles), U(s->instr), U(cpi_m), U(missrate_bp), U(taken_bp));
    /* machine-readable hazard line (raw cycle counts) */
    printf("HAZ,%s,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu\n",
           name, U(s->cycles), U(s->instr),
           U(s->h_data), U(s->h_loaduse), U(s->h_flags), U(s->h_sp),
           U(s->h_muldiv), U(s->h_brflush), U(s->h_ifmiss), U(s->h_memwait));
}

/* ── micro-kernels (identical to perf_baseline.c) ─────────────────────────────── */

__attribute__((noinline))
static void k_alu(void) {
    u64 a = 0x0123456789abcdefull, b = 0xfedcba9876543210ull;
    for (u32 i = 0; i < ALU_ITERS; i++) {
        a += b; a ^= (a << 7); a -= (b >> 3);
        b += a; b ^= (b << 11); b += i;
    }
    g_sink = a + b;
}

__attribute__((noinline))
static void k_mem(void) {
    volatile u64 *b = g_buf;
    u32 n = g_words;
    u64 sum = 0;
    for (u32 r = 0; r < MEM_REPS; r++) {
        for (u32 i = 0; i < n; i++) b[i] = b[i] + i + r;
        for (u32 i = 0; i < n; i++) sum += b[i];
    }
    g_sink = sum;
}

static void build_perm(void) {
    u64 *b = g_buf;
    u32 n = g_words;
    for (u32 i = 0; i < n; i++) b[i] = i;
    u32 rng = 0x2545f491u;
    for (u32 i = n - 1; i > 0; i--) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        u32 j = rng % i;
        u64 t = b[i]; b[i] = b[j]; b[j] = t;
    }
}

__attribute__((noinline))
static void k_chase(void) {
    volatile u64 *b = g_buf;
    u32 idx = 0;
    u64 acc = 0;
    for (u32 i = 0; i < CHASE_STEPS; i++) { idx = (u32)b[idx]; acc += idx; }
    g_sink = acc;
}

__attribute__((noinline))
static void k_branch(void) {
    u32 rng = 0x1234567u;
    u64 cnt = 0;
    for (u32 i = 0; i < BR_ITERS; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        if (rng & 0x80000000u) cnt++;
        if ((rng & 0xffu) < 64u) cnt += 2;
        if ((rng & 0x0f00u) == 0) cnt += 3;
    }
    g_sink = cnt;
}

__attribute__((noinline))
static u64 fib(u32 n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
__attribute__((noinline))
static void k_calls(void) { g_sink = fib(FIB_N); }

__attribute__((noinline))
static void k_muldiv(void) {
    u64 a = 0x9e3779b97f4a7c15ull, acc = 0;
    for (u32 i = 1; i <= MD_ITERS; i++) {
        a  = a * (u64)i;
        acc += a / (u64)(i | 1u);
        acc ^= a % (u64)(i | 3u);
    }
    g_sink = acc;
}

/* ── harness ─────────────────────────────────────────────────────────────────── */

static void bench(const char *name, void (*fn)(void)) {
    REG_CACHE_CTRL = CACHE_CTRL_CLEAR;
    REG_PERF_CTRL  = PERF_CTRL_CLEAR;
    u64 t0 = REG_CLOCK_MS;
    fn();
    u64 t1 = REG_CLOCK_MS;

    snap_t s;
    s.cycles   = REG_PERF_CYCLES;   s.instr  = REG_PERF_INSTR;
    s.fetch    = REG_PERF_FETCH_CYCLES; s.exec = REG_PERF_EXEC_CYCLES;
    s.mul      = REG_PERF_MUL_CYCLES;   s.div  = REG_PERF_DIV_CYCLES;
    s.intc     = REG_PERF_INT_CYCLES;   s.idle = REG_PERF_IDLE_CYCLES;
    s.mul_ops  = REG_PERF_MUL_OPS;  s.div_ops = REG_PERF_DIV_OPS;  s.int_ops = REG_PERF_INT_OPS;
    s.alu      = REG_PERF_CNT_ALU;  s.load   = REG_PERF_CNT_LOAD;  s.store = REG_PERF_CNT_STORE;
    s.branch   = REG_PERF_CNT_BRANCH; s.taken = REG_PERF_CNT_BRANCH_TAKEN;
    s.jump     = REG_PERF_CNT_JUMP; s.call   = REG_PERF_CNT_CALL;
    s.indirect = REG_PERF_CNT_INDIRECT; s.other = REG_PERF_CNT_OTHER;
    s.fastpath = REG_PERF_FASTPATH;
    /* M6/M7 hazard counters (read in the snapshot, before any printing) */
    s.h_data    = REG_PERF_STALL_DATA;    s.h_loaduse = REG_PERF_STALL_LOADUSE;
    s.h_flags   = REG_PERF_STALL_FLAGS;   s.h_sp      = REG_PERF_STALL_SP;
    s.h_muldiv  = REG_PERF_STALL_MULDIV;  s.h_brflush = REG_PERF_BRANCH_FLUSH;
    s.h_ifmiss  = REG_PERF_IF_MISS;       s.h_memwait = REG_PERF_MEM_WAIT;
    s.rh = REG_CACHE_RD_HITS;  s.rm = REG_CACHE_RD_MISSES;
    s.wh = REG_CACHE_WR_HITS;  s.wm = REG_CACHE_WR_MISSES;
    s.wb = REG_CACHE_WRITEBACKS; s.stall = REG_CACHE_STALL_CYC;
    s.ms = t1 - t0;

    report(name, &s);
}

int main(void) {
    printf("=== KlaussCPU perf HAZARD baseline ===\n");

    u64 info = REG_CACHE_INFO;
    printf("L1: ways=%u sets=%u line=%uB total=%uB\n",
           CACHE_INFO_WAYS(info), CACHE_INFO_SETS(info),
           CACHE_INFO_LINE_BYTES(info), CACHE_INFO_TOTAL_BYTES(info));

    g_words = MEM_WORDS;
    g_buf = (u64 *)malloc((size_t)g_words * sizeof(u64));
    if (!g_buf) {
        printf("malloc(%u KiB) failed\n", (unsigned)(g_words * 8u / 1024u));
        return 1;
    }
    printf("working set: %u KiB\n\n", (unsigned)(g_words * 8u / 1024u));

    bench("alu",        k_alu);
    bench("mem_stream", k_mem);
    build_perm();
    bench("ptr_chase",  k_chase);
    bench("branchy",    k_branch);
    bench("calls_fib",  k_calls);
    bench("muldiv",     k_muldiv);

    free(g_buf);
    printf("\n=== done (sink=%lu) ===\n", U(g_sink));
    return 0;
}
