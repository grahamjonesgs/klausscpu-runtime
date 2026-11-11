#!/usr/bin/env python3
"""check.py EMU_OUT JOBS TESTSUITE HERE BINDIR UNSUPPORTED

Checks ffirun's output (EMU_OUT) for the test programs of run.sh (JOBS:
"name source flags..." per line) the way libffi's DejaGnu testsuite does:
a test passes when it exits with status 0 and its output matches the
concatenation of its dg-output patterns (a Tcl regular expression, not
anchored, searched in stdout+stderr).  libffi.bhaible's programs print
every result twice (the direct call and the libffi one): they pass when
no line is unpaired, as their Makefile's `uniq -u` check.  The
KlaussCPU-specific abi programs print FAIL lines for failed checks and
exit non-zero.  Prints one line per test and a SUMMARY line.
"""
import os
import re
import sys


def tcl_unescape(s):
    """Tcl backslash substitution inside a double-quoted word."""
    out, i = [], 0
    simple = {'n': '\n', 't': '\t', 'r': '\r', 'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v'}
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            out.append(simple.get(n, n))
            i += 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


DIRECTIVE = re.compile(r'\{[ \t]+dg-([-a-z]+)[ \t][^}]*\}')
OUTPUT = re.compile(r'\{[ \t]+dg-output[ \t]+"((?:[^"\\]|\\.)*)"')


def expected_output(src):
    text = open(src, encoding='latin-1').read()
    pat = ''
    for m in DIRECTIVE.finditer(text):
        if m.group(1) != 'output':
            continue
        o = OUTPUT.match(m.group(0))
        if o:
            pat += tcl_unescape(o.group(1))
    return pat


def tcl_to_python(p):
    # Tcl AREs and Python's re agree on what these patterns use; Tcl's
    # "." also matches a newline.
    return re.compile(p, re.DOTALL)


def main():
    emu_out, jobs, _ts, _here, bindir, unsup = sys.argv[1:7]
    raw = open(emu_out, encoding='latin-1').read().replace('\r\n', '\n').replace('\r', '')
    blocks = {}
    for m in re.finditer(r'^@@BEGIN (\S+)\n(.*?)\n@@END \1 (\S+)\n', raw, re.S | re.M):
        blocks[m.group(1)] = (m.group(2), m.group(3))
    npass = nfail = nunsup = 0
    for line in open(jobs):
        name, src = line.split()[:2]
        exe = os.path.join(bindir, name.replace('/', '__'))
        if os.path.exists(exe + '.compile-error'):
            print(f'FAIL {name}: does not compile: '
                  + open(exe + '.compile-error').read().strip().replace('\n', ' | ')[:300])
            nfail += 1
            continue
        if name not in blocks:
            print(f'FAIL {name}: no output (not run)')
            nfail += 1
            continue
        output, status = blocks[name]
        why = []
        if status != 'exit=0':
            why.append(status)
        if name.startswith('libffi.bhaible/'):
            lines = output.split('\n')
            unpaired = [l for i, l in enumerate(lines)
                        if l and not ((i > 0 and lines[i - 1] == l) or (i + 1 < len(lines) and lines[i + 1] == l))]
            if unpaired:
                why.append(f'{len(unpaired)} unpaired lines, first: {unpaired[0][:120]!r}')
            nres = sum(1 for l in lines if l) // 2
            if nres < 10:
                why.append(f'only {nres} results')
        elif name.startswith('klausscpu/'):
            bad = [l for l in output.split('\n') if l.startswith('FAIL')]
            if bad:
                why.append(f'{len(bad)} FAIL lines, first: {bad[0][:200]}')
            if not re.search(r'^DONE checks=\d+ failures=0$', output, re.M):
                why.append('no clean DONE line')
        else:
            pat = expected_output(src)
            if pat and not tcl_to_python(pat).search(output):
                why.append(f'output does not match dg-output {pat[:120]!r}; got {output[:200]!r}')
        if why:
            print(f'FAIL {name}: ' + '; '.join(why))
            nfail += 1
        else:
            print(f'PASS {name}')
            npass += 1
    if os.path.exists(unsup):
        for line in open(unsup):
            print(f'UNSUPPORTED {line.strip()}')
            nunsup += 1
    print(f'SUMMARY pass={npass} fail={nfail} unsupported={nunsup} ')


if __name__ == '__main__':
    main()
