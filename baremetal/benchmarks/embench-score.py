#!/usr/bin/env python3
"""embench-score.py — Embench-IoT 1.0 speed and size scores from a run.

Usage: embench-score.py <bench-results/<OPT>/embench.csv> [--cpu-mhz N] [--markdown]

Reads the CSV that run-benchmarks.sh writes (name,cycles,instr,verify,status,
text_bytes) and the reference data in embench-iot/baseline-data/, and applies
the Embench 1.0 method (embench-iot/doc/README.md):

  speed: normalised time = cycles / CPU_MHZ (us at 1 MHz); relative speed =
         reference ms * 1000 / normalised time.  With CPU_MHZ = 1 (the default
         build) this is the speed per MHz relative to the reference Cortex-M4.
  size : relative size = .text bytes / reference .text bytes (smaller is better).

Both are summarised by the geometric mean and the geometric standard
deviation, as benchmark_speed.py / benchmark_size.py do.
"""
import argparse
import csv
import json
import math
import os
import sys


def geo(values):
    logs = [math.log(v) for v in values]
    mean = sum(logs) / len(logs)
    sd = math.sqrt(sum((x - mean) ** 2 for x in logs) / len(logs))
    return math.exp(mean), math.exp(sd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('csv')
    ap.add_argument('--cpu-mhz', type=float, default=1.0,
                    help='EMBENCH_CPU_MHZ the run was built with (default 1)')
    ap.add_argument('--baselinedir', default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), 'embench-iot', 'baseline-data'))
    ap.add_argument('--markdown', action='store_true')
    args = ap.parse_args()

    with open(os.path.join(args.baselinedir, 'speed.json')) as f:
        base_speed = json.load(f)
    with open(os.path.join(args.baselinedir, 'size.json')) as f:
        base_size = {k: v['text'] for k, v in json.load(f).items()}

    rows = []
    with open(args.csv) as f:
        for r in csv.DictReader(f):
            name = r['name']
            cyc = int(r['cycles']) if r['cycles'] else 0
            text = int(r['text_bytes']) if r['text_bytes'] else 0
            spd = (base_speed[name] * 1000.0 / (cyc / args.cpu_mhz)) if cyc else None
            sz = (text / base_size[name]) if text else None
            rows.append((name, cyc, r['instr'], r['status'], text, spd, sz))

    speeds = [r[5] for r in rows if r[5] and r[3] == 'PASS']
    sizes = [r[6] for r in rows if r[6]]
    sg, ssd = geo(speeds) if speeds else (0, 0)
    zg, zsd = geo(sizes) if sizes else (0, 0)

    if args.markdown:
        print('| benchmark | cycles | instr | CPI | verify | .text (B) | rel. speed | rel. size |')
        print('|---|---:|---:|---:|---|---:|---:|---:|')
        for name, cyc, instr, st, text, spd, sz in rows:
            cpi = '%.2f' % (cyc / int(instr)) if cyc and instr else '-'
            print('| %s | %d | %s | %s | %s | %d | %s | %s |' % (
                name, cyc, instr, cpi, st, text,
                '%.3f' % spd if spd else '-', '%.3f' % sz if sz else '-'))
        print('| **geomean** | | | | | | **%.3f** | **%.3f** |' % (sg, zg))
        print('| geo. SD | | | | | | %.3f | %.3f |' % (ssd, zsd))
    else:
        print('%-16s %10s %10s %-8s %7s %8s %8s' % (
            'benchmark', 'cycles', 'instr', 'verify', '.text', 'rel_spd', 'rel_size'))
        for name, cyc, instr, st, text, spd, sz in rows:
            print('%-16s %10d %10s %-8s %7d %8s %8s' % (
                name, cyc, instr, st, text,
                '%.3f' % spd if spd else '-', '%.3f' % sz if sz else '-'))
        print('speed geomean %.3f (geo SD %.3f); size geomean %.3f (geo SD %.3f)'
              % (sg, ssd, zg, zsd))
    return 0 if len(speeds) == len(rows) else 1


if __name__ == '__main__':
    sys.exit(main())
