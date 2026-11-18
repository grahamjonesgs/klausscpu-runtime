#!/usr/bin/env python3
"""symbolize-so.py NM PROF OUTPREFIX: klausscc tools/tlbord/symbolize.py for
a shared library (librustc_driver), whose load address is not known.

NM: llvm-nm --defined-only -n -S of the unstripped library.  PROF: the
KLAUSSCC_PROFILE window file (prefix.N, with prefix.N.calls next to it).

The load base is found from the calls: it is 64 KiB aligned (the page size),
so for each of the most frequent call targets, every function whose start
has the same low 16 bits gives a candidate base (target - start).  The right
base is the one most targets agree on.  Only PCs inside the library count.
Writes OUTPREFIX.funcs (name instr first) and OUTPREFIX.edges (caller callee
count), the formats gen-orders.py reads."""
import sys, bisect, collections

nm, prof, out = sys.argv[1:4]
TH = '__KlaussCPUThunk_'
starts, ends, names = [], [], []
for line in open(nm):
    f = line.split()
    if len(f) == 4 and f[2] in 'tTwW':
        a, sz = int(f[0], 16), int(f[1], 16)
        if sz == 0:
            continue
        starts.append(a); ends.append(a + sz); names.append(f[3])
lo, hi = min(starts), max(ends)

calls = []
for line in open(prof + '.calls'):
    if line.startswith('#'):
        continue
    s, t, c = line.split()
    calls.append((int(s, 16), int(t, 16), int(c)))

by_low = collections.defaultdict(list)
for a in starts:
    by_low[a & 0xffff].append(a)
targets = collections.Counter()
for _, t, c in calls:
    targets[t] += c
votes = collections.Counter()
top = [t for t, _ in targets.most_common(400)]
for t in top:
    for a in by_low.get(t & 0xffff, ()):
        b = t - a
        if b >= 0 and b % 0x10000 == 0:
            votes[b] += 1
if not votes:
    sys.exit(f'{prof}: no load base found')
(base, nvotes), *rest = votes.most_common(2) + [(None, 0)]
second = rest[0][1]
print(f'{prof}: load base {base:#x} ({nvotes} of {len(top)} top targets agree; next best {second})', file=sys.stderr)
if nvotes < len(top) // 4 or nvotes <= 2 * second:
    sys.exit(f'{prof}: load base not clear-cut')


def func(pc):
    v = pc - base
    if v < lo or v >= hi:
        return None
    i = bisect.bisect_right(starts, v) - 1
    if i >= 0 and v < ends[i]:
        n = names[i]
        if n.startswith(TH):
            n = n[len(TH):]
            if n.startswith('.text.') and n.endswith('+0x0'):
                n = n[len('.text.'):-len('+0x0')]
        return n
    return None


instr = collections.Counter(); first = {}
outside = miss = 0
for line in open(prof):
    if line.startswith('#'):
        continue
    f = line.split()
    pc = int(f[0], 16)
    v = pc - base
    if v < lo or v >= hi:
        outside += int(f[1]); continue
    n = func(pc)
    if n is None:
        miss += int(f[1]); continue
    instr[n] += int(f[1])
    fs = int(f[9]) if len(f) > 9 else 0
    if n not in first or fs < first[n]:
        first[n] = fs
edges = collections.Counter(); emiss = 0
for s, t, c in calls:
    a, b = func(s), func(t)
    if a is None or b is None:
        emiss += c; continue
    edges[(a, b)] += c
with open(out + '.funcs', 'w') as w:
    for n, c in instr.most_common():
        w.write(f'{n} {c} {first[n]}\n')
with open(out + '.edges', 'w') as w:
    for (a, b), c in edges.most_common():
        w.write(f'{a} {b} {c}\n')
print(f'{prof}: {len(instr)} driver functions, {sum(instr.values())} driver instr '
      f'({miss} unsymbolized, {outside} outside the driver), {len(edges)} edges, '
      f'{sum(edges.values())} calls ({emiss} not driver-to-driver)', file=sys.stderr)
