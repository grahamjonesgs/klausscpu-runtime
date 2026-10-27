/* test_flags.c — which flags does each instruction write?
 *
 * Runs every KlaussCPU instruction (test_flags_cases.S, generated with the
 * table in test_flags_cases.h by gen_test_flags.py, which also checks that
 * table against the LLVM backend's Defs/Uses = [FLAGS]) with the flags all
 * clear and all set and a set of operand pairs, and checks:
 *
 *   - no flag outside the instruction's table entry changes.  The compiler
 *     keeps a value in the flags across an instruction whose entry is
 *     empty, so a failure here is a miscompile waiting to happen;
 *   - every flag in the entry changes for some state and operands (else the
 *     entry, and the compiler's FLAGS def, may be wider than needed:
 *     reported as WIDE, not a failure);
 *   - for the instructions the FlagReuse pass takes the Z flag from
 *     instead of a `cmprv rd, 0`, Z == (rd == 0).
 *
 * NOP's entry is empty, so it checks the harness: both flag states must
 * read back unchanged.  Regenerate the cases after a .td change with
 *   python3 programs/gen_test_flags.py <klausscpu-llvm checkout>
 * (it fails when the .td and the table disagree).
 *
 * Prints one line per failure and a summary; exit status = failures.
 */

#include <stdio.h>

struct fc_case {
    const char *name;
    void (*fn)(unsigned long *out, unsigned long state, unsigned long a,
               unsigned long b);
    unsigned char writes;   /* Z 8, S 4, C 2, V 1 */
    unsigned char zres;     /* check Z == (rd == 0) */
};

#include "test_flags_cases.h"

#define FZ 8
#define FS 4
#define FC 2
#define FV 1

static const unsigned long ops[][2] = {
    {0, 0},
    {7, 3},
    {3, 7},
    {7, 7},
    {0x8000000000000000ul, 0x8000000000000000ul},
    {0xFFFFFFFFFFFFFFFFul, 1},
    {0x100000000ul, 0x7FFFFFFFFFFFFFFFul},
    {0x100, 0xFFFFFFFF},
    {0x10000, 0x8000000000000000ul},
    {(unsigned long)-100000, 5},
    {100000, (unsigned long)-5},
    {5, 1},
    {1, 0},
};

/* GETF puts Z at [63], C at [61], V at [60]; the test adds S at [0]. */
static unsigned flags_of(unsigned long w) {
    return ((w >> 63) & 1 ? FZ : 0) | (w & 1 ? FS : 0) |
           ((w >> 61) & 1 ? FC : 0) | ((w >> 60) & 1 ? FV : 0);
}

int main(void) {
    unsigned n = sizeof fc_cases / sizeof fc_cases[0];
    unsigned nops = sizeof ops / sizeof ops[0];
    unsigned runs = 0, fails = 0, wide = 0;

    for (unsigned i = 0; i < n; i++) {
        const struct fc_case *c = &fc_cases[i];
        unsigned seen = 0, zero_rd = 0, nonzero_rd = 0;
        for (unsigned st = 0; st < 2; st++) {
            unsigned before = st ? (FZ | FS | FC | FV) : 0;
            for (unsigned k = 0; k < nops; k++) {
                unsigned long out[2] = {0, 0};
                c->fn(out, st, ops[k][0], ops[k][1]);
                unsigned after = flags_of(out[1]);
                unsigned changed = before ^ after;
                runs++;
                seen |= changed;
                if (changed & ~c->writes) {
                    printf("FAIL %s: state %u a=%lx b=%lx changed %x outside %x\n",
                           c->name, st, ops[k][0], ops[k][1], changed,
                           c->writes);
                    fails++;
                }
                if (c->zres) {
                    if (out[0] == 0) zero_rd++; else nonzero_rd++;
                    if (!!(after & FZ) != (out[0] == 0)) {
                        printf("FAIL %s: state %u a=%lx b=%lx rd=%lx Z=%u\n",
                               c->name, st, ops[k][0], ops[k][1], out[0],
                               !!(after & FZ));
                        fails++;
                    }
                }
            }
        }
        if (c->writes & ~seen) {
            printf("WIDE %s: never changed %x of %x\n", c->name,
                   c->writes & ~seen, c->writes);
            wide++;
        }
        if (c->zres && (!zero_rd || !nonzero_rd)) {
            printf("WIDE %s: no %s result\n", c->name,
                   zero_rd ? "nonzero" : "zero");
            wide++;
        }
    }
    printf("test_flags: %u instructions, %u runs, %u failures, %u wide\n", n,
           runs, fails, wide);
    printf(fails ? "test_flags: FAIL\n" : "test_flags: PASS\n");
    return (int)fails;
}
