#!/usr/bin/env python3
"""Generate test_hwcg_cases.h for test_hwcg.c.

test_hwcg checks, on the emulator and the board, the code the compiler emits
for the klaussCPU 3508f87 instructions (ISA_ENCODING_V2_MAP.md section 8):

  - fused compare-and-branch with the W predicates (EQW/LTW/ULTW and their
    inverses) and with an imm32 right-hand side (2-word FBR), every
    predicate, taken and not taken, with constants on both sides of the
    simm4 / simm8 / imm32 boundaries and operands whose upper 32 bits differ
    (a W compare must ignore them, a 64-bit compare must not);
  - forward and backward fused branches whose distance is at the largest the
    compiler still fuses (FuseCmpBr's worst-case range check) and just
    beyond (JMPcc instead);
  - the 1-word CMPRVW.S, MULV.S / MULUV.S and LEAPC.S.

Every expected value is computed here, not by the compiler, so a miscompile
shows as a difference.  The range cases are found by compiling probe
functions with the toolchain (the boundary moves with code generation), so
this script needs <checkout>/build/bin/{clang,llvm-objdump}:

  gen_test_hwcg.py <klausscpu-llvm checkout> [--check]

--check compares with the header already there.
"""
import os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
M64 = (1 << 64) - 1
M32 = (1 << 32) - 1


def s32(x):
    x &= M32
    return x - (1 << 32) if x >> 31 else x


def s64(x):
    x &= M64
    return x - (1 << 64) if x >> 63 else x


PREDS = ["eq", "ne", "lt", "le", "gt", "ge", "ult", "ule", "ugt", "uge"]
CX = {"eq": "==", "ne": "!=", "lt": "<", "le": "<=", "gt": ">", "ge": ">=",
      "ult": "<", "ule": "<=", "ugt": ">", "uge": ">="}


def pred(p, a, b):
    """a, b: python ints already reduced to the compare's width/signedness."""
    return {"eq": a == b, "ne": a != b, "lt": a < b, "le": a <= b,
            "gt": a > b, "ge": a >= b, "ult": a < b, "ule": a <= b,
            "ugt": a > b, "uge": a >= b}[p]


def isuns(p):
    return p.startswith("u")


# ---- fused compare-and-branch --------------------------------------------

COMMON = [0, 1, 2, 7, 8, 9, 127, 128, 255, 256, 0x7fff, 0x8000, 0xffff,
          0x10000, 0x7ffffffe, 0x7fffffff, 0x80000000, 0x80000001,
          0xfffffffe, 0xffffffff, 0x100000000, 0x100000001, 0x1ffffffff,
          0x7fffffff00000000, 0x7fffffffffffffff, 0x8000000000000000,
          0x8000000080000000, 0xffffffff00000000, 0xffffffff00000001,
          0xfffffffffffffffe, 0xffffffffffffffff, 0xdead0000beef0000,
          0xdeadbeef12345678, 0xcafef00d00000007]
COMMON += [(-x) & M64 for x in (1, 2, 7, 8, 9, 127, 128, 129, 0x7fffffff,
                                0x80000000, 0x80000001, 0x100000000)]

# W: signed constants (int32) and unsigned (uint32), 64-bit: imm32 range and
# beyond it.
WS = [0, 1, -1, 7, -8, 8, -9, 100, -100, 127, -128, 128, 0x7fff, 0x8000,
      0xffff, 0x7ffffffe, 0x7fffffff, -0x80000000, -0x7fffffff,
      0x12345678, -0x12345678]
WU = [0, 1, 7, 8, 100, 255, 0x7fffffff, 0x80000000, 0x80000001, 0xfffffffe,
      0xffffffff, 0x12345678, 0xdeadbeef]
QS = [0, 1, -1, 7, -8, 8, -9, 100, -100, 0x7fff, 0x7fffffff, -0x80000000,
      -0x7fffffff, 0x12345678, -0x12345678, 0x80000000, 0xffffffff,
      0x100000000, -0x80000001, 0x123456789a]
QU = [0, 1, 7, 8, 100, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000,
      0xfffffffffffffff0, 0xffffffffffffffff]


def around(c):
    out = []
    for d in (-1, 0, 1):
        v = (c + d) & M64
        out += [v, v ^ (0xdead << 32), (v & M32) | (1 << 32),
                (v & M32) | (0xffffffff << 32)]
    return out


def uniq(l):
    seen, out = set(), []
    for x in l:
        x &= M64
        if x not in seen:
            seen.add(x)
            out.append(x)
    return out


VALS = list(COMMON)
for c in WS + WU + QS + QU:
    VALS += around(c)
VALS = uniq(VALS)
NW = (len(VALS) + 63) // 64

PAIRV = uniq([0, 1, 2, 7, 0x7fffffff, 0x80000000, 0xffffffff, 0x100000000,
              0x100000001, 0x17fffffff, 0x180000000, 0x1ffffffff,
              0xffffffff00000000, 0xffffffff00000001, 0xffffffffffffffff,
              0xfffffffffffffffe, 0x8000000000000000, 0x7fffffffffffffff,
              0xdead00000000, 0xdead00000001, 0xdead80000000, 5,
              (-5) & M64, (-0x80000000) & M64])
NP = len(PAIRV)
NPW = (NP * NP + 63) // 64

funcs = []   # (name, kind, cdecl, nwords, bits)


def mask(bits):
    words = []
    for w in range((len(bits) + 63) // 64):
        v = 0
        for i, b in enumerate(bits[w * 64:(w + 1) * 64]):
            if b:
                v |= 1 << i
        words.append(v)
    return words


def cname(c):
    return ("m" if c < 0 else "") + "%x" % abs(c)


# (cond for a W compare is on the low 32 bits.)
for p in PREDS:
    u = isuns(p)
    T32 = "uint32_t" if u else "int32_t"
    T64 = "uint64_t" if u else "int64_t"
    # register-register
    funcs.append(("rrw_" + p, 2,
                  f"{T32} x = ({T32})a, y = ({T32})b; if (x {CX[p]} y)",
                  mask([pred(p, (x & M32) if u else s32(x), (y & M32) if u else s32(y))
                        for x in PAIRV for y in PAIRV])))
    funcs.append(("rr64_" + p, 2,
                  f"if (({T64})a {CX[p]} ({T64})b)",
                  mask([pred(p, x if u else s64(x), y if u else s64(y))
                        for x in PAIRV for y in PAIRV])))
    # register-constant
    for c in (WU if u else WS):
        cc = c & M32
        lit = ("0x%xu" % cc) if u else ("(%d)" % c if c != -0x80000000 else "(-2147483647 - 1)")
        funcs.append((f"riw_{p}_{cname(c)}", 1,
                      f"if (({T32})a {CX[p]} ({T32}){lit})",
                      mask([pred(p, (v & M32) if u else s32(v), cc if u else s32(c)) for v in VALS])))
    for c in (QU if u else QS):
        if u:
            lit = "0x%xull" % (c & M64)
            cv = c & M64
        else:
            lit = "(%dll)" % c if c != -(1 << 63) else "(-9223372036854775807ll - 1)"
            cv = s64(c)
        funcs.append((f"ri64_{p}_{cname(c)}", 1,
                      f"if (({T64})a {CX[p]} {lit})",
                      mask([pred(p, v if u else s64(v), cv) for v in VALS])))

# The chain a backward loop reads its exit value from: a loop counter would
# be fused only in some shapes (the compare of the old counter, then the
# update, then the branch stays a compare and a jump), a loaded value always.
CHAIN = [5, 0x12345677, 0xdead000000000001, 0x100000000, 0x12345678,
         0xffffffff00000000, 0x7fffffff, 0x12345679]

BOUND = {"riw": "0x12345678", "rsw": "100", "ri64": "0x12345678"}

# ---- ranges (probed) -------------------------------------------------------

BIN = None
G = "static volatile int g[256]; static volatile int seedv = 7; static volatile int64_t chainq[8] = {%s};" % ", ".join("%dll" % s64(v) for v in CHAIN)


def body(n):
    return "\n".join(f"  p[{k % 200}] = s + {k};" for k in range(n))


def fwd_src(form, n):
    cond = {"rrw": "(int32_t)a < (int32_t)b",
            "riw": "(int32_t)a < 0x12345678",
            "rsw": "(int32_t)a < 100",
            "ri64": "(int64_t)a < 0x12345678"}[form]
    return f"""#include <stdint.h>
{G}
__attribute__((noinline, used)) static int f(uint64_t a, uint64_t b) {{
  volatile int *p = g; int s = seedv;
  if ({cond}) goto skip;
{body(n)}
skip:
  return 1;
}}
"""


def bwd_src(form, n):
    if form == "rrw":
        loop = "int i = 0; do {\n%s\n i++; } while (i < (int32_t)b);" % body(n)
        ret = "i"
    elif form in ("riw", "rsw"):
        loop = ("int i = (int)b; int64_t x; do {\n%s\n x = chainq[i++ & 7]; }"
                " while ((int32_t)x < %s);" % (body(n), BOUND[form]))
        ret = "i - (int)b"
    else:
        loop = ("int i = (int)b; int64_t x; do {\n%s\n x = chainq[i++ & 7]; }"
                " while (x < 0x12345678);" % body(n))
        ret = "i - (int)b"
    return f"""#include <stdint.h>
{G}
__attribute__((noinline, used)) static int f(uint64_t a, uint64_t b) {{
  volatile int *p = g; int s = seedv;
  {loop}
  return {ret};
}}
"""


def first_fbr(src, direction, clang, objdump):
    with tempfile.TemporaryDirectory() as d:
        open(d + "/p.c", "w").write(src)
        subprocess.check_call([clang, "-target", "klausscpu-unknown-elf", "-O2", "-fPIC", "-ffunction-sections", "-c",
                               d + "/p.c", "-o", d + "/p.o"])
        o = subprocess.check_output([objdump, "-d", "--no-show-raw-insn", d + "/p.o"], text=True)
    return any(re.match(r"\s+[0-9a-f]+:\s+fbr", l) for l in o.splitlines())


def probe(direction, form, clang, objdump):
    mk = fwd_src if direction == "fwd" else bwd_src
    lo, hi = 50, 2200
    assert first_fbr(mk(form, lo), direction, clang, objdump), (direction, form, "no fusion at all")
    assert not first_fbr(mk(form, hi), direction, clang, objdump), (direction, form, "no limit")
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if first_fbr(mk(form, mid), direction, clang, objdump):
            lo = mid
        else:
            hi = mid
    # The threshold is not exactly monotonic (the scheduler places the
    # instructions around the compare differently from size to size): take the
    # last size that fuses before a run of 24 that do not.
    n = lo
    while True:
        later = [m for m in range(n + 1, n + 25) if first_fbr(mk(form, m), direction, clang, objdump)]
        if not later:
            return n
        n = later[-1]


def stores_checksum(n):
    g = [0] * 200
    for k in range(n):
        g[k % 200] = k + 7
    return sum(g) & 0xffffffff


def gen_ranges(root):
    clang = root + "/build/bin/clang"
    objdump = root + "/build/bin/llvm-objdump"
    out = []
    rng = []   # (name, kind, n)
    for direction in ("fwd", "bwd"):
        for form in ("rrw", "riw", "rsw", "ri64"):
            n0 = probe(direction, form, clang, objdump)
            for n in (n0 - 1, n0, n0 + 1):
                nm = f"rng_{direction}_{form}_{n}"
                if direction == "fwd":
                    out.append(fwd_src(form, n).replace("#include <stdint.h>\n" + G + "\n", "")
                               .replace("static int f(", f"static int {nm}(").replace("skip:", f"skip_{n}:")
                               .replace("goto skip;", f"goto skip_{n};"))
                else:
                    out.append(bwd_src(form, n).replace("#include <stdint.h>\n" + G + "\n", "")
                               .replace("static int f(", f"static int {nm}("))
                rng.append((nm, direction, form, n, n0))
    return out, rng


def range_cases(rng):
    """(function, a, b, expected return, expected checksum of g) per call."""
    cases = []
    for nm, d, form, n, n0 in rng:
        ck = stores_checksum(n)
        if d == "fwd":
            if form == "rrw":
                pairs = [(0xdead00000001, 2), (2, 0xbeef00000001), (5, 5), (0x100000000, 0),
                         (0xffffffff, 0), (0x7fffffff, 0x80000000), (0x80000000, 0x7fffffff)]
            else:
                bd = 100 if form == "rsw" else 0x12345678
                pairs = [(bd - 1, 0), (bd, 0), (0xdead000000000000 | (bd - 1), 0),
                         (0x100000000 | (bd + 1), 0), (M64, 0), (0x80000000, 0), (5, 0)]
            res = []
            for a, b in pairs:
                if form == "rrw":
                    taken = s32(a) < s32(b)
                elif form == "riw":
                    taken = s32(a) < 0x12345678
                elif form == "rsw":
                    taken = s32(a) < 100
                else:
                    taken = s64(a) < 0x12345678
                res.append((a, b, 1, 0 if taken else ck))
            cases.append((nm, res))
        else:
            res = []
            if form == "rrw":
                for b in (1, 2, 3, 0, M64, 0xdead000000000003, 0x100000002, 0x80000000):
                    i = 0
                    while True:
                        i += 1
                        if not (i < s32(b)):
                            break
                    res.append((0, b, i, ck))
            else:
                for st in range(8):
                    i = st
                    while True:
                        x = CHAIN[i & 7]
                        i += 1
                        xv = s64(x) if form == "ri64" else s32(x)
                        if not (xv < (0x12345678 if form != "rsw" else 100)):
                            break
                    res.append((0, st, i - st, ck))
            cases.append((nm, res))
    return cases


# ---- short forms -----------------------------------------------------------

CMPC = [-128, -127, -9, -8, -1, 0, 1, 7, 8, 9, 100, 126, 127, 128, 129, -129]
CMP_PRED = ["eq", "ne", "lt", "le", "gt", "ge", "ult", "ule", "ugt", "uge"]


def gen_short():
    src = []
    tab = []
    # CMPRVW.S: a W compare whose result is a value, not a branch (a select
    # of constants: no fused branch, so the compare stays a compare).
    for p in CMP_PRED:
        u = isuns(p)
        T32 = "uint32_t" if u else "int32_t"
        for c in CMPC:
            if u and c < 0:
                continue
            nm = f"cw_{p}_{cname(c)}"
            src.append(f"NOINLINE static uint64_t {nm}(uint64_t a, uint64_t b) "
                       f"{{ return (({T32})a {CX[p]} ({T32}){c}) ? b : ~b; }}")
            vals = []
            for v in VALS:
                x = (v & M32) if u else s32(v)
                cv = (c & M32) if u else c
                vals.append(1 if pred(p, x, cv) else 0)
            tab.append((nm, "cmpw", mask(vals)))
    return src, tab


MULC = [-128, -127, -100, -9, -8, -2, -1, 0, 1, 2, 3, 5, 7, 9, 10, 100, 127, 128, 200, 255, 256, 12345]


def gen_mul():
    src, tab = [], []
    for c in MULC:
        nm = f"mulv_{cname(c)}"
        src.append(f"NOINLINE static uint64_t {nm}(uint64_t a) {{ return a * (uint64_t)({c}ll); }}")
        tab.append((nm, c))
    return src, tab


def main():
    root = sys.argv[1]
    check = "--check" in sys.argv
    out = []
    out.append("/* test_hwcg_cases.h -- generated by gen_test_hwcg.py: do not edit. */\n")
    out.append("#define NOINLINE __attribute__((noinline))\n")
    out.append("static volatile int hwcg_sink;\n")
    out.append("static const uint64_t HW_VALS[%d] = {%s};\n" % (
        len(VALS), ",\n    ".join(", ".join("0x%xull" % v for v in VALS[i:i + 4]) for i in range(0, len(VALS), 4))))
    out.append("static const uint64_t HW_PAIRV[%d] = {%s};\n" % (
        NP, ", ".join("0x%xull" % v for v in PAIRV)))
    out.append(f"#define HW_NVALS {len(VALS)}\n#define HW_NW {NW}\n#define HW_NPAIRV {NP}\n#define HW_NPW {NPW}\n")
    for nm, nargs, cond, bits in funcs:
        if nargs == 2:
            out.append(f"NOINLINE static int {nm}(uint64_t a, uint64_t b) {{ {cond} {{ hwcg_sink = 1; return 1; }} hwcg_sink = 2; return 0; }}\n")
        else:
            out.append(f"NOINLINE static int {nm}(uint64_t a) {{ {cond} {{ hwcg_sink = 1; return 1; }} hwcg_sink = 2; return 0; }}\n")
    out.append("struct hw_br1 { const char *name; int (*f)(uint64_t); uint64_t m[HW_NW]; };\n")
    out.append("struct hw_br2 { const char *name; int (*f)(uint64_t, uint64_t); uint64_t m[HW_NPW]; };\n")
    out.append("static const struct hw_br1 HW_BR1[] = {\n")
    for nm, nargs, cond, bits in funcs:
        if nargs == 1:
            out.append("  {\"%s\", %s, {%s}},\n" % (nm, nm, ", ".join("0x%xull" % w for w in bits)))
    out.append("};\n")
    out.append("static const struct hw_br2 HW_BR2[] = {\n")
    for nm, nargs, cond, bits in funcs:
        if nargs == 2:
            out.append("  {\"%s\", %s, {%s}},\n" % (nm, nm, ", ".join("0x%xull" % w for w in bits)))
    out.append("};\n")
    out.append("#define HW_N_BR1 (sizeof HW_BR1 / sizeof HW_BR1[0])\n#define HW_N_BR2 (sizeof HW_BR2 / sizeof HW_BR2[0])\n")

    # ranges
    rsrc, rng = gen_ranges(root)
    out.append("\n/* ---- range: the largest fused branch the compiler emits, and one beyond ---- */\n")
    out.append("static volatile int g[256];\nstatic volatile int seedv = 7;\n")
    out.append("static volatile int64_t chainq[8] = {%s};\n" % ", ".join("%dll" % s64(v) for v in CHAIN))
    for s in rsrc:
        out.append(s.replace("  volatile int *p = g;", "  volatile int *p = g;")
                   .replace("__attribute__((noinline, used)) static int", "NOINLINE static int"))
    out.append("struct hw_rng { const char *name; int (*f)(uint64_t, uint64_t); unsigned ncall; };\n")
    cases = range_cases(rng)
    out.append("struct hw_rcall { uint64_t a, b; int ret; unsigned ck; };\n")
    for nm, res in cases:
        out.append("static const struct hw_rcall %s_calls[] = {%s};\n" % (
            nm, ", ".join("{0x%xull, 0x%xull, %d, %du}" % (a & M64, b & M64, r, c) for a, b, r, c in res)))
    out.append("static const struct { const char *name; int (*f)(uint64_t, uint64_t); const struct hw_rcall *c; unsigned n; } HW_RNG[] = {\n")
    for nm, res in cases:
        out.append("  {\"%s\", %s, %s_calls, %d},\n" % (nm, nm, nm, len(res)))
    out.append("};\n#define HW_N_RNG (sizeof HW_RNG / sizeof HW_RNG[0])\n")

    # short forms
    s, tabc = gen_short()
    out.append("\n/* ---- CMPRVW.S: a W compare against a constant in simm8, used as a value ---- */\n")
    out += [x + "\n" for x in s]
    out.append("static const struct { const char *name; uint64_t (*f)(uint64_t, uint64_t); uint64_t m[HW_NW]; } HW_CW[] = {\n")
    for nm, _, m in tabc:
        out.append("  {\"%s\", %s, {%s}},\n" % (nm, nm, ", ".join("0x%xull" % w for w in m)))
    out.append("};\n#define HW_N_CW (sizeof HW_CW / sizeof HW_CW[0])\n")

    s, tabm = gen_mul()
    out.append("\n/* ---- MULV.S / MULUV.S: multiply by an 8-bit constant ---- */\n")
    out += [x + "\n" for x in s]
    out.append("static const struct { const char *name; uint64_t (*f)(uint64_t); long long c; } HW_MUL[] = {\n")
    for nm, c in tabm:
        out.append("  {\"%s\", %s, %dll},\n" % (nm, nm, c))
    out.append("};\n#define HW_N_MUL (sizeof HW_MUL / sizeof HW_MUL[0])\n")

    text = "".join(out)
    path = os.path.join(HERE, "test_hwcg_cases.h")
    if check:
        if open(path).read() != text:
            sys.exit("test_hwcg_cases.h is out of date")
        print("test_hwcg_cases.h is up to date")
    else:
        open(path, "w").write(text)
        print("wrote", path, "%d bytes, %d values, %d range functions" % (len(text), len(VALS), len(rng)))


if __name__ == "__main__":
    main()
