#!/usr/bin/env python3
"""perf.py -- board performance numbers: extract them, compare with a baseline.

  perf.py extract board  <board_suite dir> <prefix>    Dhrystone, perf_baseline, perf_haz
  perf.py extract bench  <bench-results/<OPT> dir> <prefix>   CoreMark, Embench-IoT
  perf.py extract zephyr <zephyr board dir> <prefix>   mandel, doom, irq_soak, gui_lvgl
  perf.py compare <baseline.tsv> <metrics.tsv> [--tol PCT]
  perf.py merge <baseline.tsv> <metrics.tsv> <toolchain>   update the baseline in place

`extract` prints one metric per line: key, value, which direction is better
(lower/higher/info) and the default tolerance in percent.  The baseline file
(baselines/perf.tsv) has the same columns, plus comments.  `compare` prints
every metric with its change, marks REGRESSION when a metric got worse by more
than its tolerance (or --tol, if given), IMPROVED when it got better by more,
and exits 1 if anything regressed.  Metrics missing from either side are
listed but do not fail.  'info' metrics are reported, never judged.

For the board suite, a second run (run2/) is also read: a metric whose two
runs differ by more than its tolerance is reported as NOISY.
"""
import math
import os
import re
import sys


def read(path):
    try:
        with open(path, 'rb') as f:
            return f.read().decode('utf-8', 'replace').replace('\r', '')
    except OSError:
        return None


def board_metrics(d):
    out = []
    for prog in ('dhrystone', 'dhrystone_nobuiltin'):
        t = read(os.path.join(d, prog + '.out'))
        if t is None:
            continue
        m = re.search(r'Cycles/Dhrystone\s*:\s*([0-9.]+)', t)
        if m:
            out.append((f'{prog}.cycles_per_run', float(m.group(1)), 'lower', 0.2))
        if 'Self-check           : PASS' not in t and 'Self-check' in t:
            out.append((f'{prog}.selfcheck_fail', 1, 'info', 0))
    for prog in ('perf_baseline', 'perf_haz'):
        t = read(os.path.join(d, prog + '.out'))
        if t is None:
            continue
        for m in re.finditer(r'^CSV,([a-z_0-9]+),\d+,(\d+),(\d+),', t, re.M):
            if prog == 'perf_baseline':
                out.append((f'{prog}.{m.group(1)}.cycles', int(m.group(2)), 'lower', 0.5))
                out.append((f'{prog}.{m.group(1)}.instr', int(m.group(3)), 'info', 0))
        # HAZ,<kernel>,cycles,instr,data,loaduse,flags,sp,muldiv,brflush,ifmiss,memwait
        for m in re.finditer(r'^HAZ,([a-z_0-9]+),(.*)$', t, re.M):
            f = m.group(2).split(',')
            names = ['cycles', 'instr', 'data', 'loaduse', 'flags', 'sp', 'muldiv',
                     'brflush', 'ifmiss', 'memwait']
            for n, v in zip(names, f):
                if v.strip().isdigit():
                    out.append((f'perf_haz.{m.group(1)}.{n}', int(v), 'info', 0))
    return out


def bench_metrics(d):
    out = []
    t = read(os.path.join(d, 'coremark.csv'))
    if t:
        for line in t.splitlines():
            f = line.split(',')
            # coremark,iterations,cycles,instr,CoreMark*1000,CoreMark/MHz*1000,status
            if len(f) >= 7 and f[2].isdigit():
                out.append((f'{f[0]}.cycles', int(f[2]), 'lower', 0.5))
                out.append((f'{f[0]}.score', int(f[4]) / 1000.0, 'higher', 0.5))
                out.append((f'{f[0]}.pass', 1 if f[6] == 'PASS' else 0, 'higher', 0))
    t = read(os.path.join(d, 'embench.csv'))
    if t:
        npass = nrun = 0
        for line in t.splitlines()[1:]:
            f = line.split(',')
            # name,cycles,instr,verify,status,text_bytes
            if len(f) >= 6 and f[1].isdigit():
                nrun += 1
                npass += f[4] == 'PASS'
                out.append((f'embench.{f[0]}.cycles', int(f[1]), 'lower', 0.5))
                if f[5].isdigit():
                    out.append((f'embench.{f[0]}.text', int(f[5]), 'lower', 1.0))
        out.append(('embench.pass', npass, 'higher', 0))
        out.append(('embench.run', nrun, 'info', 0))
    t = read(os.path.join(d, 'summary.txt'))
    if t:
        m = re.search(r'speed geomean ([0-9.]+).*size geomean ([0-9.]+)', t)
        if m:
            out.append(('embench.speed', float(m.group(1)), 'higher', 0.5))
            out.append(('embench.size', float(m.group(2)), 'lower', 0.5))
    return out


def zephyr_metrics(d):
    out = []
    t = read(os.path.join(d, 'mandel.out'))
    if t:
        v = [int(x) for x in re.findall(r'mandel: kiter/s=(\d+)', t)]
        if v:
            # The rate settles as the run goes on; the best value of the run
            # is the steadiest figure.
            out.append(('mandel.kiter_s_max', max(v), 'higher', 3))
    t = read(os.path.join(d, 'doom.out'))
    if t:
        v = sorted(int(x) for x in re.findall(r'doom: drawfps=(\d+)', t))
        if v:
            out.append(('doom.drawfps_median', v[len(v) // 2], 'higher', 10))
    t = read(os.path.join(d, 'irq_soak.out'))
    if t:
        beats = re.findall(r'BEAT t=(\d+) s loops=(\d+) ticks=\d+ viol=(\d+) lost=(\d+)', t)
        if beats:
            want = [b for b in beats if b[0] == '60']
            b = want[0] if want else beats[-1]
            out.append((f'irq_soak.loops_{b[0]}s', int(b[1]), 'higher', 1))
            out.append(('irq_soak.viol_lost', sum(int(x[2]) + int(x[3]) for x in beats), 'lower', 0))
    t = read(os.path.join(d, 'gui_lvgl.out'))
    if t:
        for m in re.finditer(r'LVGL bench (\w+)\s*: total=(\d+)', t):
            out.append((f'gui_lvgl.{m.group(1)}.ms_frame', int(m.group(2)), 'lower', 5))
    return out


def fmt(v):
    if isinstance(v, float):
        return f'{v:.4f}'.rstrip('0').rstrip('.')
    return str(v)


def extract(kind, d, prefix):
    fn = {'board': board_metrics, 'bench': bench_metrics, 'zephyr': zephyr_metrics}[kind]
    rows = fn(d)
    if kind == 'board' and os.path.isdir(os.path.join(d, 'run2')):
        again = {k: v for k, v, _, _ in fn(os.path.join(d, 'run2'))}
        for k, v, better, tol in rows:
            if better != 'info' and k in again and v and abs(again[k] - v) / abs(v) * 100 > tol:
                print(f'# NOISY {prefix}.{k}: run 1 {fmt(v)}, run 2 {fmt(again[k])}', file=sys.stderr)
    for k, v, better, tol in rows:
        print(f'{prefix}.{k}\t{fmt(v)}\t{better}\t{tol:g}')
    return 0 if rows else 1


def load(path):
    rows = {}
    with open(path) as f:
        for line in f:
            if not line.strip() or line.startswith('#'):
                continue
            p = line.rstrip('\n').split('\t')
            if len(p) < 2:
                continue
            rows[p[0]] = (float(p[1]), p[2] if len(p) > 2 else 'info',
                          float(p[3]) if len(p) > 3 and p[3] else 0.0)
    return rows


def compare(base_path, new_path, tol_override):
    base, new = load(base_path), load(new_path)
    bad = 0
    print(f'{"metric":52s} {"baseline":>14s} {"now":>14s} {"change":>9s}')
    for k in sorted(set(base) | set(new)):
        if k not in new:
            print(f'{k:52s} {fmt(base[k][0]):>14s} {"-":>14s}   not measured')
            continue
        if k not in base:
            print(f'{k:52s} {"-":>14s} {fmt(new[k][0]):>14s}   new metric')
            continue
        b, better, tol = base[k]
        n = new[k][0]
        if better == 'info':
            continue
        if tol_override is not None and tol > 0:
            tol = tol_override
        ch = (n / b - 1) * 100 if b else (0.0 if n == b else math.inf)
        worse = ch if better == 'lower' else -ch
        mark = ''
        if (b == 0 and n != b and better == 'lower') or worse > tol or (tol == 0 and worse > 0):
            mark = 'REGRESSION'
            bad += 1
        elif -worse > max(tol, 0.0) and n != b:
            mark = 'improved'
        if n != b or mark:
            print(f'{k:52s} {fmt(b):>14s} {fmt(n):>14s} {ch:+8.2f}% {mark}')
    print(f'{bad} regression(s) beyond tolerance' if bad else 'no regressions beyond tolerance')
    return 1 if bad else 0


def merge(base_path, new_path, toolchain):
    """Replace the baseline values of every metric in new_path (keeping each
    line's direction and tolerance, which may have been tuned by hand) and
    append metrics the baseline lacks."""
    import datetime
    new = {}
    order = []
    with open(new_path) as f:
        for line in f:
            p = line.rstrip('\n').split('\t')
            if len(p) >= 4 and not line.startswith('#'):
                new[p[0]] = p
                order.append(p[0])
    lines = open(base_path).read().splitlines() if os.path.exists(base_path) else []
    seen = set()
    out = []
    for line in lines:
        p = line.split('\t')
        if not line.startswith('#') and len(p) >= 4 and p[0] in new:
            p[1] = new[p[0]][1]
            seen.add(p[0])
            out.append('\t'.join(p))
        else:
            out.append(line)
    added = [k for k in order if k not in seen]
    if added:
        out.append(f'# added {datetime.date.today()} from toolchain {toolchain}')
        out.extend('\t'.join(new[k][:4]) for k in added)
    prefixes = sorted({k.split('.')[0] for k in new})
    out.append(f'# {datetime.date.today()}: {len(new)} {"/".join(prefixes)} values from toolchain {toolchain}')
    with open(base_path, 'w') as f:
        f.write('\n'.join(out) + '\n')
    return 0


def main(a):
    if len(a) == 4 and a[0] == 'merge':
        return merge(a[1], a[2], a[3])
    if len(a) == 4 and a[0] == 'extract':
        return extract(a[1], a[2], a[3])
    if len(a) >= 3 and a[0] == 'compare':
        tol = float(a[4]) if len(a) == 5 and a[3] == '--tol' else None
        return compare(a[1], a[2], tol)
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
