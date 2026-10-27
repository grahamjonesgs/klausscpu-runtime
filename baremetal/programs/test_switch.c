/* test_switch.c — exercises switch/case → jump table dispatch.
 *
 * Compile via Makefile:  make test_switch.elf  &&  make test_switch.bin
 *
 * KlaussCPU's LLVM backend lowers BR_JT to a 4-byte EK_Custom32 table
 * (each entry is an absolute target address with R_KLAUSSCPU_ABS32),
 * loads the entry with MEMGET32/LDIDX32, and dispatches via JMPR_R.
 * This file pokes that whole pipeline:
 *
 *   - dense-key switch (consecutive small ints, the obvious jump-table case)
 *   - char-key switch (offset key, still dense)
 *   - sparse switch with default branch (LLVM may keep this as if/else, but
 *     the harness still verifies behaviour)
 *   - a 16-opcode interpreter loop and a 12-case switch over 23 keys: the
 *     backend builds a jump table from 12 cases at 40% density, so these
 *     two are the ones that take the table dispatch (T1-T4 are below that)
 *
 * Run output should print "PASS" for every test then a results summary.
 */

#include <stdio.h>

static int g_pass;
static int g_fail;

static void check(char *name, int ok) {
    printf("%s%s\n", name, ok ? "PASS" : "FAIL");
    if (ok) g_pass++; else g_fail++;
}

/* ── T1: dense int switch ─────────────────────────────────────────────────── */

static int classify(int n) {
    switch (n) {
    case 0:  return 100;
    case 1:  return 101;
    case 2:  return 102;
    case 3:  return 103;
    case 4:  return 104;
    case 5:  return 105;
    case 6:  return 106;
    case 7:  return 107;
    case 8:  return 108;
    case 9:  return 109;
    default: return -1;
    }
}

/* ── T2: char-key switch with side effect ─────────────────────────────────── */

static int op_apply(char op, int a, int b) {
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return (b == 0) ? 0 : a / b;
    case '%': return (b == 0) ? 0 : a % b;
    case '&': return a & b;
    case '|': return a | b;
    case '^': return a ^ b;
    case '<': return a << (b & 31);
    case '>': return a >> (b & 31);
    default:  return -1;
    }
}

/* ── T3: switch inside a loop, with fallthrough ───────────────────────────── */

static int score(char *s) {
    int total = 0;
    for (int i = 0; s[i] != '\0'; ++i) {
        switch (s[i]) {
        case 'a': case 'e': case 'i': case 'o': case 'u':
            total += 1;
            break;
        case 'A': case 'E': case 'I': case 'O': case 'U':
            total += 2;
            break;
        case ' ':
        case '\t':
            break;
        default:
            total += 3;
            break;
        }
    }
    return total;
}

/* ── T4: sparse switch (LLVM may keep as if/else; verify behaviour) ───────── */

static int sparse(int k) {
    switch (k) {
    case  1:    return 11;
    case 17:    return 217;
    case 99:    return 9999;
    case 1000:  return 100000;
    case -5:    return -55;
    default:    return 0;
    }
}

/* ── T5: 16-opcode interpreter loop (a jump table from 12 cases) ─────────── */

enum { OP_ADD, OP_SUB, OP_XOR, OP_SHL, OP_SHR, OP_INC, OP_DEC, OP_DUP,
       OP_SWAP, OP_MUL3, OP_NOT, OP_ROT, OP_AND, OP_OR, OP_NEG, OP_HALT };

static unsigned long interp(const unsigned char *p, unsigned long a,
                            unsigned long b) {
    for (;;) {
        switch (*p++) {
        case OP_ADD:  a = a + b;                       break;
        case OP_SUB:  a = a - b;                       break;
        case OP_XOR:  a = a ^ b;                       break;
        case OP_SHL:  a = a << 1;                      break;
        case OP_SHR:  a = a >> 1;                      break;
        case OP_INC:  a++;                             break;
        case OP_DEC:  b--;                             break;
        case OP_DUP:  b = a;                           break;
        case OP_SWAP: { unsigned long t = a; a = b; b = t; } break;
        case OP_MUL3: a = a * 3;                       break;
        case OP_NOT:  a = ~a;                          break;
        case OP_ROT:  a = (a << 7) | (a >> 57);        break;
        case OP_AND:  a = a & (b | 0xFF);              break;
        case OP_OR:   a = a | (b & 0xF0F0);            break;
        case OP_NEG:  a = 0 - a;                       break;
        case OP_HALT: return a ^ (b << 1);
        default:      a += 0x5A5A;                     break;  /* bad opcode */
        }
    }
}

/* ── T6: 12 cases over 23 keys (a table indexed by c - 'b') ──────────────── */

static unsigned long sparse_dense(int c, unsigned long v) {
    switch (c) {
    case 'b': return v + 1;
    case 'd': return v * 7;
    case 'f': return v - 100;
    case 'h': return v ^ 0x55;
    case 'j': return v << 3;
    case 'l': return v >> 2;
    case 'n': return 0 - v;
    case 'p': return v | 0x1000;
    case 'r': return v & 0xFF;
    case 't': return v * v;
    case 'v': return v + 'v';
    case 'x': return 3 * v + 1;
    default:  return 7;
    }
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void) {
    g_pass = 0;
    g_fail = 0;

    printf("=== Switch / Jump-Table Test ===\n");

    /* T1: dense int switch — exercise every case + the default. */
    {
        int ok = 1;
        for (int i = 0; i < 10; ++i)
            ok = ok && (classify(i) == 100 + i);
        ok = ok && (classify(10)  == -1);
        ok = ok && (classify(-1)  == -1);
        ok = ok && (classify(99)  == -1);
        check("T1 classify(0..9): ", ok);
    }

    /* T2: char-key switch */
    {
        int ok = 1;
        ok = ok && (op_apply('+', 7, 3) == 10);
        ok = ok && (op_apply('-', 7, 3) ==  4);
        ok = ok && (op_apply('*', 7, 3) == 21);
        ok = ok && (op_apply('/', 7, 3) ==  2);
        ok = ok && (op_apply('%', 7, 3) ==  1);
        ok = ok && (op_apply('&', 0xF0, 0x0F) == 0x00);
        ok = ok && (op_apply('|', 0xF0, 0x0F) == 0xFF);
        ok = ok && (op_apply('^', 0xFF, 0x0F) == 0xF0);
        ok = ok && (op_apply('<', 1, 4) == 16);
        ok = ok && (op_apply('>', 64, 2) == 16);
        ok = ok && (op_apply('?', 1, 2) == -1);   /* default */
        check("T2 op_apply chars: ", ok);
    }

    /* T3: switch inside a loop with fallthrough cases */
    {
        int ok = 1;
        ok = ok && (score("hello world") == 3+1+3+3+1+0+3+1+3+3+3);
        ok = ok && (score("AEIOU")       == 2+2+2+2+2);
        ok = ok && (score("aeiou")       == 1+1+1+1+1);
        ok = ok && (score("")            == 0);
        ok = ok && (score("   ")         == 0);
        ok = ok && (score("xyz")         == 9);
        check("T3 score loop sw:  ", ok);
    }

    /* T4: sparse keys */
    {
        int ok = 1;
        ok = ok && (sparse(   1) ==     11);
        ok = ok && (sparse(  17) ==    217);
        ok = ok && (sparse(  99) ==   9999);
        ok = ok && (sparse(1000) == 100000);
        ok = ok && (sparse(  -5) ==    -55);
        ok = ok && (sparse(   0) ==      0);
        ok = ok && (sparse(   2) ==      0);
        check("T4 sparse switch:  ", ok);
    }

    /* T5: interpreter loop over every opcode, an unknown one and HALT.
     * Expected values from the same code built with host gcc. */
    {
        unsigned char prog[256];
        unsigned s = 1;
        for (int i = 0; i < 255; i++) {
            s = s * 1103515245u + 12345u;
            prog[i] = (s >> 16) % 17;           /* 16: an unknown opcode */
            if (prog[i] == OP_HALT)
                prog[i] = 16;
        }
        prog[255] = OP_HALT;
        unsigned long r = 0;
        for (int k = 0; k < 4; k++)
            r += interp(prog, r + k, 12345 * k);
        check("T5 interp loop:    ", r == 9918552UL);
    }

    /* T6: every key in and around 'b'..'x' */
    {
        unsigned long t = 0;
        for (int c = 'a' - 2; c <= 'z' + 2; c++)
            t = t * 31 + sparse_dense(c, c + t % 1000);
        check("T6 sparse table:   ", t == 12686950814770859519UL);
    }

    printf("\nResults: %d pass, %d fail\n", g_pass, g_fail);

    return g_fail;
}
