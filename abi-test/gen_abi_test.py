#!/usr/bin/env python3
"""Generate an ABI conformance test: caller.c and callee.c (separate TUs).

The callee TU defines functions with random parameter and return types:
signed and unsigned 8/16/32/64-bit integers, pointers, float/double (soft
float on KlaussCPU), structs and unions of 1 to 24 bytes with mixed fields,
and variadic functions (including C23 `f(...)` with no named parameter).
Each callee folds every argument into a 64-bit hash with operations that
depend on the full register value (widening to 64 bits, signed compares,
shifts), checks that a struct passed by value is a private copy (it writes to
its parameter), and returns a value of its return type derived from the hash.

The caller TU computes each argument from volatile data, so values reach the
call in registers with arbitrary upper bits unless the producer extends them,
calls every function, and prints one line per test: the hash of the result and
of the caller's by-value objects after the call.  Host gcc gives the expected
output; the target build mixes optimisation levels between the TUs.

Usage: gen_abi_test.py SEED OUTDIR [NFUNCS]
"""
import os
import random
import sys
import zlib

INT_TYPES = [
    ('signed char', 1, True), ('unsigned char', 1, False),
    ('short', 2, True), ('unsigned short', 2, False),
    ('int', 4, True), ('unsigned', 4, False),
    ('long', 8, True), ('unsigned long', 8, False),
    ('_Bool', 1, False),
]
FIELD_TYPES = ['signed char', 'unsigned char', 'short', 'unsigned short',
               'int', 'unsigned', 'long', 'unsigned long', 'float', 'double']
SIZES = {'signed char': 1, 'unsigned char': 1, 'short': 2,
         'unsigned short': 2, 'int': 4, 'unsigned': 4, 'long': 8,
         'unsigned long': 8, 'float': 4, 'double': 8, '_Bool': 1}


def layout_size(fields):
    off, align = 0, 1
    for t, n in fields:
        s = SIZES[t]
        a = s
        off = (off + a - 1) // a * a + s * n
        align = max(align, a)
    return (off + align - 1) // align * align


class Gen:
    def __init__(self, seed, nfuncs):
        self.r = random.Random(seed)
        self.nfuncs = nfuncs
        self.records = []   # (name, kind, fields)
        self.funcs = []

    def make_record(self):
        r = self.r
        for _ in range(100):
            nf = r.randint(1, 5)
            fields = []
            for _ in range(nf):
                t = r.choice(FIELD_TYPES)
                n = 1 if r.random() < 0.8 else r.randint(2, 5)
                fields.append((t, n))
            sz = layout_size(fields)
            if 1 <= sz <= 24:
                break
        kind = 'union' if r.random() < 0.12 else 'struct'
        name = 'R%d' % len(self.records)
        self.records.append((name, kind, fields))
        return name

    def rand_type(self):
        r = self.r
        x = r.random()
        if x < 0.45:
            return ('int',) + r.choice(INT_TYPES)
        if x < 0.55:
            return ('ptr',)
        if x < 0.62:
            return ('fp', r.choice(['float', 'double']))
        if x < 0.67:
            return ('i128',)
        if self.records and r.random() < 0.5:
            return ('rec', r.choice(self.records)[0])
        return ('rec', self.make_record())

    def cname(self, t):
        if t[0] == 'int':
            return t[1]
        if t[0] == 'ptr':
            return 'const char *'
        if t[0] == 'fp':
            return t[1]
        if t[0] == 'i128':
            return '__int128'
        rec = [x for x in self.records if x[0] == t[1]][0]
        return '%s %s' % (rec[1], rec[0])

    def gen(self):
        for i in range(self.nfuncs):
            r = self.r
            variadic = r.random() < 0.25
            nparams = r.randint(0, 9)
            if variadic and r.random() < 0.15:
                nparams = 0           # C23 f(...)
            params = [self.rand_type() for _ in range(nparams)]
            if variadic and nparams == 0:
                pass
            nvar = r.randint(0, 7) if variadic else 0
            # Unnamed arguments: default-promoted types only.
            vtypes = []
            for _ in range(nvar):
                t = self.rand_type()
                if t[0] == 'int' and t[2] < 4 or t[0] == 'int' and t[1] == '_Bool':
                    t = ('int', 'int', 4, True)
                if t[0] == 'fp':
                    t = ('fp', 'double')
                vtypes.append(t)
            ret = self.rand_type() if r.random() < 0.85 else ('void',)
            self.funcs.append(('f%d' % i, ret, params, variadic, vtypes))

    # -- emitters ----------------------------------------------------------

    def header(self):
        out = ['#include <stdarg.h>', '#include <string.h>', '#include <stdint.h>', '']
        out.append('typedef unsigned long u64;')
        out.append('static inline u64 mix(u64 h, u64 v) { h ^= v + 0x9e3779b97f4a7c15UL + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdUL; }')
        out.append('static inline u64 mixs(u64 h, long v) { return mix(mix(h, (u64)v), v < 0 ? 0x55 : 0xaa); }')
        out.append('static inline u64 mixbytes(u64 h, const void *p, unsigned n) { const unsigned char *b = p; for (unsigned i = 0; i < n; i++) h = mix(h, b[i]); return h; }')
        for name, kind, fields in self.records:
            fs = ' '.join('%s m%d%s;' % (t, j, '' if n == 1 else '[%d]' % n)
                          for j, (t, n) in enumerate(fields))
            out.append('%s %s { %s };' % (kind, name, fs))
        for name, ret, params, variadic, vtypes in self.funcs:
            out.append(self.proto(name, ret, params, variadic) + ';')
        out.append('')
        return out

    def proto(self, name, ret, params, variadic):
        ps = ['%s a%d' % (self.cname(t), j) for j, t in enumerate(params)]
        if variadic:
            ps.append('...')
        if not ps:
            ps = ['void']
        rt = 'void' if ret[0] == 'void' else self.cname(ret)
        return '%s %s(%s)' % (rt, name, ', '.join(ps))

    def fold(self, expr, t, h='h'):
        """Statements folding value `expr` (evaluated once: it may be a
        va_arg) of type t into hash h."""
        if t[0] == 'int':
            if t[3]:
                return ['%s = mixs(%s, (long)%s);' % (h, h, expr)]
            return ['{ %s v_ = %s; %s = mix(%s, (u64)v_); %s = mix(%s, (u64)(v_ >> 1)); }'
                    % (t[1], expr, h, h, h, h)]
        if t[0] == 'ptr':
            return ['%s = mix(%s, (u64)(%s - g_base));' % (h, h, expr)]
        if t[0] == 'fp':
            return ['{ %s v_ = %s; %s = mixbytes(%s, &v_, sizeof v_); }' % (t[1], expr, h, h)]
        if t[0] == 'i128':
            return ['{ __int128 v_ = %s; %s = mix(%s, (u64)v_); %s = mix(%s, (u64)(v_ >> 64)); %s = mix(%s, v_ < 0); }'
                    % (expr, h, h, h, h, h, h)]
        rec = [x for x in self.records if x[0] == t[1]][0]
        out = ['{ %s r_ = %s;' % (self.cname(t), expr)]
        if rec[1] == 'union':
            out.append('  %s = mixbytes(%s, &r_, sizeof r_); }' % (h, h))
            return out
        for j, (ft, n) in enumerate(rec[2]):
            for k in range(n):
                fe = 'r_.m%d%s' % (j, '' if n == 1 else '[%d]' % k)
                if ft in ('float', 'double'):
                    out.append('  { %s v_ = %s; %s = mixbytes(%s, &v_, sizeof v_); }' % (ft, fe, h, h))
                elif ft in ('signed char', 'short', 'int', 'long'):
                    out.append('  %s = mixs(%s, (long)%s);' % (h, h, fe))
                else:
                    out.append('  %s = mix(%s, (u64)%s);' % (h, h, fe))
        out.append('}')
        return out

    def make_value(self, t, src):
        """An expression of type t computed from u64 expression src."""
        if t[0] == 'int':
            if t[1] == '_Bool':
                return '(_Bool)((%s) & 4)' % src
            return '(%s)(%s)' % (t[1], src)
        if t[0] == 'ptr':
            return '(g_base + ((%s) & 63))' % src
        if t[0] == 'fp':
            return '(%s)(long)(int)(%s)' % (t[1], src)
        if t[0] == 'i128':
            return '(((__int128)(long)(%s) << 64) | (__int128)(u64)((%s) * 3))' % (src, src)
        return None

    def fill_record(self, var, t, src):
        rec = [x for x in self.records if x[0] == t[1]][0]
        out = ['memset(&%s, 0, sizeof %s);' % (var, var)]
        if rec[1] == 'union':
            out.append('{ u64 s_ = %s; for (unsigned i_ = 0; i_ < sizeof %s; i_++) { ((unsigned char *)&%s)[i_] = (unsigned char)(s_ >> (8 * (i_ & 7))); if ((i_ & 7) == 7) s_ = mix(s_, i_); } }' % (src, var, var))
            return out
        idx = 0
        for j, (ft, n) in enumerate(rec[2]):
            for k in range(n):
                fe = '%s.m%d%s' % (var, j, '' if n == 1 else '[%d]' % k)
                v = '(%s) * %d + %d' % (src, 2 * idx + 3, idx)
                if ft in ('float', 'double'):
                    out.append('%s = (%s)(long)(int)(%s);' % (fe, ft, v))
                else:
                    out.append('%s = (%s)(%s);' % (fe, ft, v))
                idx += 1
        return out

    def callee(self):
        out = self.header()
        out.append('extern const char g_base[];')
        out.append('extern volatile u64 g_sink;')
        for name, ret, params, variadic, vtypes in self.funcs:
            out.append(self.proto(name, ret, params, variadic) + ' {')
            out.append('  u64 h = 0x%x;' % zlib.crc32(name.encode()))
            for j, t in enumerate(params):
                out += ['  ' + s for s in self.fold('a%d' % j, t)]
                if t[0] == 'rec':
                    # The parameter is our copy: writing it must not change
                    # the caller's object.
                    out.append('  memset(&a%d, 0x5a, sizeof a%d);' % (j, j))
            if variadic:
                out.append('  va_list ap;')
                if params:
                    out.append('  va_start(ap, a%d);' % (len(params) - 1))
                else:
                    out.append('  va_start(ap);')
                for k, t in enumerate(vtypes):
                    out += ['  ' + s for s in self.fold('va_arg(ap, %s)' % self.cname(t), t)]
                out.append('  va_end(ap);')
            if ret[0] == 'void':
                out.append('  g_sink = h;')
            elif ret[0] == 'rec':
                out.append('  %s r;' % self.cname(ret))
                out += ['  ' + s for s in self.fill_record('r', ret, 'h')]
                out.append('  return r;')
            else:
                out.append('  return %s;' % self.make_value(ret, 'h'))
            out.append('}')
        return '\n'.join(out) + '\n'

    def caller(self):
        out = self.header()
        out.append('#include <stdio.h>')
        out.append('const char g_base[64] = "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ!?";')
        out.append('volatile u64 g_sink;')
        out.append('volatile u64 g_seed = 0x%x;' % self.r.getrandbits(64))
        out.append('static u64 next(void) { u64 s = g_seed; s ^= s << 13; s ^= s >> 7; s ^= s << 17; g_seed = s; return s; }')
        out.append('int main(void) {')
        for i, (name, ret, params, variadic, vtypes) in enumerate(self.funcs):
            out.append('  { /* %s */' % name)
            out.append('    u64 h = 0;')
            args = []
            recs = []
            for j, t in enumerate(params + vtypes):
                if t[0] == 'rec':
                    v = 'v%d' % j
                    out.append('    %s %s;' % (self.cname(t), v))
                    out += ['    ' + s for s in self.fill_record(v, t, 'next()')]
                    args.append(v)
                    recs.append((v, t))
                else:
                    v = 'v%d' % j
                    out.append('    %s %s = %s;' % (self.cname(t), v, self.make_value(t, 'next()')))
                    args.append(v)
            call = '%s(%s)' % (name, ', '.join(args))
            if ret[0] == 'void':
                out.append('    %s;' % call)
                out.append('    h = mix(h, g_sink);')
            else:
                out += ['    ' + s for s in self.fold(call, ret)]
            for v, t in recs:
                out.append('    h = mixbytes(h, &%s, sizeof %s);' % (v, v))
            out.append('    printf("T%d %%016lx\\n", h);' % i)
            out.append('  }')
        out.append('  printf("ABI-DONE\\n");')
        out.append('  return 0;')
        out.append('}')
        return '\n'.join(out) + '\n'


def main():
    seed = int(sys.argv[1])
    outdir = sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    g = Gen(seed, n)
    g.gen()
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, 'callee.c'), 'w') as f:
        f.write(g.callee())
    with open(os.path.join(outdir, 'caller.c'), 'w') as f:
        f.write(g.caller())


if __name__ == '__main__':
    main()
