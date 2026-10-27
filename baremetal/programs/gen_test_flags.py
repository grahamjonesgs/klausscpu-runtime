#!/usr/bin/env python3
"""Generate test_flags_cases.S / test_flags_cases.h for test_flags.c.

test_flags checks, on the emulator or the board, which flags every KlaussCPU
instruction writes, against the table below, and this script checks the
table against the LLVM backend's claims: an instruction has `Defs = [FLAGS]`
in KlaussCPUInstrInfo.td exactly when the table says it writes a flag (calls
also clobber FLAGS for the ABI), `Uses = [FLAGS]` exactly when it reads one,
and every opcode the FlagReuse pass trusts to set Z = (result == 0)
(setsZeroFlagAsResult in KlaussCPUFlagReuse.cpp) writes Z, which test_flags
then checks against the result.  So a .td change that drops a FLAGS def
fails here (table mismatch) or on the target (a flag the table says is kept
changes), and an over-approximated def fails here.

The table is the RTL decoder (klaussCPU pipeline_core.sv f_decode_raw: each
instruction's fmask / fread, and the EX overrides: DIV/MOD by zero write only
V, IRET restores all four).

  gen_test_flags.py <klausscpu-llvm checkout> [--check | --force]

writes test_flags_cases.S and test_flags_cases.h next to this script, after
checking that every non-pseudo instruction is covered or listed as skipped
and that each test assembles to the instruction it names (word-0 fixed bits
and size, from llvm-objdump of the assembled cases).  --check compares with
the files already there instead of writing them; --force writes them in
spite of .td mismatches (to run the target against the table alone).  It
needs <checkout>/build/bin/{llvm-tblgen,llvm-mc,llvm-objdump}.
"""
import json, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# name -> (flags written, flags read, test body).  The body runs with
# r1 = A, r2 = B, r3 = 0x5A (the destination), r9 free, the flags set to the
# state under test; `9:` follows the body and `8:` may label its start.
# fc_buf is an 8 KiB scratch buffer, fc_stub_ret a `ret`.
RRR = '{mn}\tr3, r1, r2'
T = {}


def t(names, flags, body, reads=''):
    for n in names.split():
        T[n] = (flags, reads, body)


# Class 1 ALU reg-reg: ADD/SUB/ADC/SBC write all four (F [21] = 1); AND/OR/
# XOR/MIN/MAX(U) none (pipeline_core.sv f_decode_raw class 1/2).
t('ADDR', 'ZSCV', 'addr\tr3, r1, r2')
t('SUBR', 'ZSCV', 'subr\tr3, r1, r2')
t('ADDC', 'ZSCV', 'addc\tr3, r1, r2', reads='C')
t('SUBC', 'ZSCV', 'subc\tr3, r1, r2', reads='C')
t('ADDW', 'ZSCV', 'addw\tr3, r1, r2')
t('SUBW', 'ZSCV', 'subw\tr3, r1, r2')
for mn in 'andr orr xorr minr maxr minur maxur'.split():
    t(mn.upper(), '', RRR.format(mn=mn))
# Class 2 ALU immediate.
t('ADDI', 'ZSCV', 'addi\tr3, r1, 100000')
t('ADDI_SH', 'ZSCV', 'addi\tr3, r1, -5')
t('ADDIW', 'ZSCV', 'addiw\tr3, r1, -1')
t('ADDV', 'ZSCV', 'addv\tr3, r1, 100000')
t('ADDV_SH', 'ZSCV', 'addv\tr3, r1, 5')
t('MINUSV', 'ZSCV', 'minusv\tr3, r1, 100000')
t('MINUSV_SH', 'ZSCV', 'minusv\tr3, r1, 5')
for mn in 'andv orv xorv'.split():
    t(mn.upper(), '', mn + '\tr3, r1, 100000')
    t(mn.upper() + '_SH', '', mn + '\tr3, r1, 5')
for mn in 'andi ori xori'.split():
    t(mn.upper(), '', mn + '\tr3, r1, -100000')
    t(mn.upper() + '_SH', '', mn + '\tr3, r1, -8')
t('SETR', '', 'setr\tr3, 100000')
t('SETR_SH', '', 'setr\tr3, -5')
t('SETRU', '', 'setru\tr3, 4026597376')
t('SETRU_SH', '', 'setru\tr3, 200')
t('SETR64', '', 'setr64\tr3, 1, 2')
t('LEAPC', '', 'leapc\tr3, 64')
# Class 3: the flag-setting CMP writes all four; boolean compares none.
t('CMPRR_I', 'ZSCV', 'cmprr\tr1, r2')
t('CMPRRW_I', 'ZSCV', 'cmprrw\tr1, r2')
t('CMPRV_I', 'ZSCV', 'cmprv\tr1, 100000')
t('CMPRV_SH', 'ZSCV', 'cmprv\tr1, -5')
t('CMPRVU_I', 'ZSCV', 'cmprvu\tr1, 4294967295')
t('CMPRVW_I', 'ZSCV', 'cmprvw\tr1, -3')
for p in 'eq ne lt ge le gt ult uge ule ugt'.split():
    c = 'CMP' + p.upper()
    t(c + 'R', '', 'cmp%sr\tr3, r1, r2' % p)
    t(c + 'RW', '', 'cmp%srw\tr3, r1, r2' % p)
    t(c + 'V', '', 'cmp%sv\tr3, r1, 100000' % p)
    t(c + 'VW', '', 'cmp%svw\tr3, r1, -7' % p)
# Class 4 shift / rotate / bit: Z when F [14] is set (every shift and
# rotate template), Z and C for a rotate by an embedded 1 with F (ROT1);
# the bit set/clear/toggle/test-to-register templates have F = 0; BTST by
# position writes Z only; BEXTR writes Z, BDEP nothing.
for mn, n in [('shlr', 'SHLR'), ('shrr', 'SHRR'), ('sarr', 'SARR'),
              ('rolr', 'ROLR'), ('rorr', 'RORR_R')]:
    t(n, 'Z', RRR.format(mn=mn))
for mn in 'shlv shrv shrav'.split():
    t(mn.upper(), 'Z', mn + '\tr3, r1, 7')
t('ROLV', 'ZC', 'rolv\tr3, r1, 7\n\trolv\tr3, r3, 1')
t('RORV', 'ZC', 'rorv\tr3, r1, 7\n\trorv\tr3, r3, 1')
t('ROLR1', 'ZC', 'rolr1\tr3, r1')
t('RORR1', 'ZC', 'rorr1\tr3, r1')
t('ROLCR', 'ZC', 'rolcr\tr3, r1', reads='C')
t('RORCR', 'ZC', 'rorcr\tr3, r1', reads='C')
t('SHLR1', '', 'shlr1\tr3, r1')
t('SHLAR', '', 'shlar\tr3, r1')
t('SHRR1', '', 'shrr1\tr3, r1')
t('SHRAR', '', 'shrar\tr3, r1')
for mn in 'bsetrr bclrrr btglrr btstrr'.split():
    t(mn.upper(), '', RRR.format(mn=mn))
for mn in 'bset bclr btgl'.split():
    t(mn.upper(), '', mn + '\tr3, r1, 5')
t('BTST', 'Z', 'btst\tr1, 5')
t('BEXTR', 'Z', 'bextr\tr3, r1, 2052')
t('BDEP', '', 'bdep\tr3, r1, r2, 2052')
# Class 5 unary: Z with F [19] (NEG/NOT/ZEXTB/ZEXTH/POPCNT templates),
# Z and S for SEXT with F, Z and V for ABS (F required), all four for
# INC/DEC (F required); COPY/BSWAP/BITREV/CLZ/CTZ/SEXTW/ZEXTW and GETF
# (SETFR, which reads all four) have F = 0.
t('NEGR', 'Z', 'negr\tr3, r1')
t('NOTR', 'Z', 'notr\tr3, r1')
t('ABSR', 'ZV', 'absr\tr3, r1')
t('SEXTB', 'ZS', 'sextb\tr3, r1')
t('SEXTH', 'ZS', 'sexth\tr3, r1')
t('ZEXTB', 'Z', 'zextb\tr3, r1')
t('ZEXTH', 'Z', 'zexth\tr3, r1')
t('POPCNT', 'Z', 'popcnt\tr3, r1')
t('INCR', 'ZSCV', 'incr\tr3, r1')
t('DECR', 'ZSCV', 'decr\tr3, r1')
for mn, n in [('copy', 'COPY_R'), ('bswap', 'BSWAP_R'), ('bitrev', 'BITREV'),
              ('clz', 'CLZ'), ('ctz', 'CTZ'), ('sextw', 'SEXTW'),
              ('zextw', 'ZEXTW')]:
    t(n, '', mn + '\tr3, r1')
t('SETFR', '', 'setfr\tr3', reads='ZCV')
# Class A: MUL writes Z S V; DIV/MOD Z V (only V, set, for a zero divisor).
for mn in 'mulr mulur mulhr mulhur mulw'.split():
    t(mn.upper(), 'ZSV', RRR.format(mn=mn))
for mn in 'divr divur modr modur'.split():
    t(mn.upper(), 'ZV', RRR.format(mn=mn))
t('MULV', 'ZSV', 'mulv\tr3, r1, -3')
t('DIVV', 'ZV', 'divv\tr3, r1, 7')
t('MODV', 'ZV', 'modv\tr3, r1, 100000')
# Classes 6/7: loads and stores write no flags.
BUF = 'setru\tr9, fc_buf\n\t'
for w in '8 16 32 64'.split():
    if w != '64':
        t('MEMGET' + w, '', BUF + 'memget%s\tr3, r9' % w)
    t('MEMSET' + w, '', BUF + 'memset%s\tr2, r9' % w)
    t('LDIDX%sR' % w, '',
      'setru\tr3, fc_buf\n\tsetr\tr9, 16\n\tldidx%sr\tr3, r3, r9' % w)
    t('STIDX%sR' % w, '', BUF + 'setr\tr3, 16\n\tstidx%sr\tr2, r9, r3' % w)
    t('LDIDX' + w, '', BUF + 'ldidx%s\tr3, r9, 4096' % w)
    t('LDIDX%s_SH' % w, '', BUF + 'ldidx%s\tr3, r9, 8' % w)
    t('STIDX' + w, '', BUF + 'stidx%s\tr2, r9, 4096' % w)
    t('STIDX%s_SH' % w, '', BUF + 'stidx%s\tr2, r9, 8' % w)
    if w != '64':
        t('LDIDX%s_S' % w, '', BUF + 'ldidx%ss\tr3, r9, 4096' % w)
        t('LDIDX%s_S_SH' % w, '', BUF + 'ldidx%ss\tr3, r9, 8' % w)
        t('LDIDX%s_SR' % w, '',
          'setru\tr3, fc_buf\n\tsetr\tr9, 16\n\tldidx%ssr\tr3, r3, r9' % w)
        t('LDABS' + w, '', 'ldabs%s\tr3, fc_buf' % w)
        t('LDABS%s_S' % w, '', 'ldabs%ss\tr3, fc_buf' % w)
        t('STABS' + w, '', 'stabs%s\tr2, fc_buf' % w)
t('MEMGET64', '', BUF + 'memget64\tr3, r9')
t('MEMREADRR', '', BUF + 'memreadrr\tr3, r9')
t('MEMSET64RR', '', BUF + 'memset64rr\tr2, r9')
t('LDIDX64U', '', BUF + 'ldidx64u\tr3, r9, 4096')
t('LDIDX64U_SH', '', BUF + 'ldidx64u\tr3, r9, 8')
t('STIDX64U', '', BUF + 'stidx64u\tr2, r9, 4096')
t('STIDX64U_SH', '', BUF + 'stidx64u\tr2, r9, 8')
t('MEMREADR', '', 'memreadr\tr3, fc_buf')
t('MEMSETR', '', 'memsetr\tr2, fc_buf')
# Class 8: jumps and calls write no flags; a conditional one reads them.
# The absolute and 2-word PC-relative forms are spelled as data, since the
# assembler would pick the 1-word short form for a local target.
CONDS = {'Z': '08', 'NZ': '0C', 'C': '10', 'NC': '14', 'O': '18', 'NO': '1C',
         'S': '20', 'NS': '24', 'LT': '28', 'GE': '2C', 'LE': '30', 'GT': '34',
         'ULT': '38', 'UGE': '3C', 'ULE': '40', 'UGT': '44', 'E': '48',
         'NE': '4C'}
READS = {'Z': 'Z', 'NZ': 'Z', 'E': 'Z', 'NE': 'Z', 'C': 'C', 'NC': 'C',
         'ULT': 'C', 'UGE': 'C', 'ULE': 'ZC', 'UGT': 'ZC', 'O': 'V', 'NO': 'V',
         'S': 'S', 'NS': 'S', 'LT': 'SV', 'GE': 'SV', 'LE': 'ZSV', 'GT': 'ZSV'}
t('JMP', '', '.4byte\t0xA0000000, 9f')
t('JMPREL', '', '8:\t.4byte\t0xA1000000, 9f - 8b')
t('JMP_SH', '', 'jmprel\t9f')
t('JMPR_R', '', 'leapc\tr9, 9f\n\tjmpr\tr9')
for c, cc in CONDS.items():
    t('JMP' + c, '', '.4byte\t0xA0%s0000, 9f' % cc, reads=READS[c])
    if c not in ('O', 'NO'):
        t('JMP%sREL' % c, '', '8:\t.4byte\t0xA1%s0000, 9f - 8b' % cc,
          reads=READS[c])
        t('JMP%s_SH' % c, '', 'jmp%srel\t9f' % c.lower(), reads=READS[c])
    else:
        t('JMP%s_SH' % c, '', 'jmp%s\t9f' % c.lower(), reads=READS[c])
for c in 'Z NZ C NC O NO E NE'.split():
    t('CALL' + c, '', '.4byte\t0xA2%s0000, fc_stub_ret' % CONDS[c],
      reads=READS[c])
t('CALL_I', '', '.4byte\t0xA2000000, fc_stub_ret')
t('CALLREL', '', '8:\t.4byte\t0xA3000000, fc_stub_ret - 8b')
t('CALL_SH', '', 'callrel\tfc_stub_ret')
t('CALLR_R', '', 'leapc\tr9, fc_stub_ret\n\tcallr\tr9')
t('RET_I', '', 'callrel\t7f\n\tjmprel\t9f\n7:\tret')
t('LEAVERET_I', '', 'callrel\t7f\n\tjmprel\t9f\n7:\tenter\t0\n\tleaveret')
t('FBR_RR', '', 'fbr\tr1, r2, 0, 9f')
t('FBR_RI', '', 'fbri\tr1, 3, 3, 9f')
# Class 9 stack / SP.
t('PUSH_R', '', 'push\tr1\n\tpop\tr3')
t('POP_R', '', 'push\tr2\n\tpop\tr3')
t('PUSHV', '', 'pushv\t100000\n\tpop\tr3')
t('PUSHV64', '', 'pushv64\t1, 2\n\tpop\tr3')
t('GETSP_R', '', 'getsp\tr3')
t('SETSP_R', '', 'getsp\tr9\n\tsetsp\tr9')
t('ADDSP_I', '', 'addsp\t-8\n\taddsp\t8')
t('ENTER_I', '', 'enter\t1\n\tleave')
t('LEAVE_I', '', 'enter\t0\n\tleave')
# Class B.
t('NOP_I', '', 'nop')
t('DELAYR', '', 'setr\tr9, 2\n\tdelayr\tr9')
t('DELAYV', '', 'delayv\t2')

# Not run: they stop or park the core, need an interrupt frame, or drive
# the LCD.  IRET restores all four flags (pipeline_core.sv: fmask '1).
SKIP = {'HALT_I': '', 'WAIT_I': '', 'TRAP_I': '', 'IRET_I': 'ZSCV',
        'LCD': '', 'LCDCMDV': '', 'LCDDATAR': '', 'LCDDATAV': ''}
# The ABI says a callee may change the flags, so a call clobbers FLAGS
# although the CALL itself writes none.
ABI_CLOBBER = {'CALL_I', 'CALLREL', 'CALL_SH', 'CALLR_R', 'CALLZ', 'CALLNZ',
               'CALLC', 'CALLNC', 'CALLO', 'CALLNO', 'CALLE', 'CALLNE'}

FLAG_BITS = {'Z': 8, 'S': 4, 'C': 2, 'V': 1}


def nib(s):
    return sum(FLAG_BITS[c] for c in s)


def fixed_bits(rec):
    """Word 0's fixed bits (mask, value) from the TableGen Inst."""
    m = v = 0
    for i, x in enumerate(rec['Inst'][:32]):
        if isinstance(x, int):
            m |= 1 << i
            v |= x << i
    return m, v


def zero_flag_reusers(llvm):
    src = open(os.path.join(llvm, 'llvm/lib/Target/KlaussCPU/'
                            'KlaussCPUFlagReuse.cpp')).read()
    body = re.search(r'static bool setsZeroFlagAsResult\(unsigned Opc\) \{(.*?)\n\}',
                     src, re.S).group(1)
    return set(re.findall(r'case KlaussCPU::(\w+):', body))


def emit_asm(names):
    out = ['// Generated by gen_test_flags.py; do not edit.',
           '// One function per instruction: fc_<NAME>(out, state, A, B).',
           '// state 0 clears Z S C V, state 1 sets all four, then the',
           '// instruction runs with r1 = A, r2 = B, r3 = 0x5A;',
           '// out[0] = r3, out[1] = GETF (Z [63], C [61], V [60]) | S [0].',
           '',
           '\t.text',
           '\t.p2align\t2',
           'fc_stub_ret:',
           '\tret',
           '']
    for n in names:
        body = T[n][2]
        out += ['\t.globl\tfc_' + n,
                '\t.p2align\t2',
                'fc_%s:' % n,
                '\tcopy\tr10, r1',
                '\tcopy\tr1, r2',
                '\tcopy\tr2, r3',
                '\tsetr\tr3, 0x5A',
                '\tcmprv\tr10, 0',
                '\tjmpe\t1f',
                '\tsetr\tr8, 0',
                '\tcmprv\tr8, 1',           # 0 - 1: S C, not Z V
                '\tsetr64\tr8, 0, 0x40000000',
                '\tsetr\tr9, 2',
                '\tmulr\tr9, r8, r9',       # 2^62 * 2: S V (Z clear), C kept
                '\tsetr\tr8, 0',
                '\tbtst\tr8, 0',            # Z only
                '\tjmp\t2f',
                '1:',
                '\tsetr\tr8, 1',
                '\tcmprv\tr8, 0',           # 1 - 0: none
                '2:',
                'fc_%s_t:' % n,
                '\t' + body,
                '9:',
                'fc_%s_e:' % n,
                '\tsetfr\tr12',
                '\tjmpns\t3f',
                '\tori\tr12, r12, 1',
                '3:',
                '\tstidx64\tr3, r0, 0',
                '\tstidx64\tr12, r0, 8',
                '\tret',
                '']
    out += ['\t.bss', '\t.p2align\t3', '\t.globl\tfc_buf', 'fc_buf:',
            '\t.zero\t8192', '']
    return '\n'.join(out)


def emit_header(names, zres):
    out = ['/* Generated by gen_test_flags.py; do not edit. */', '',
           '/* name, test, flags written (Z 8, S 4, C 2, V 1), checks',
           '   Z == (rd == 0) (FlagReuse trusts it) */', '']
    for n in names:
        out.append('void fc_%s(unsigned long *out, unsigned long state, '
                   'unsigned long a, unsigned long b);' % n)
    out += ['', 'static const struct fc_case fc_cases[] = {']
    for n in names:
        out.append('    {"%s", fc_%s, 0x%x, %d},' % (n, n, nib(T[n][0]),
                                                    1 if n in zres else 0))
    out += ['};', '']
    return '\n'.join(out)


def main():
    llvm = os.path.abspath(sys.argv[1])
    check = '--check' in sys.argv[2:]
    force = '--force' in sys.argv[2:]
    bindir = os.path.join(llvm, 'build/bin')
    tdir = os.path.join(llvm, 'llvm/lib/Target/KlaussCPU')
    js = subprocess.run([os.path.join(bindir, 'llvm-tblgen'), '--dump-json',
                         '-I', os.path.join(llvm, 'llvm/include'), '-I', tdir,
                         os.path.join(tdir, 'KlaussCPU.td')],
                        check=True, capture_output=True, text=True).stdout
    d = json.loads(js)
    insts = sorted(n for n in d['!instanceof']['Instruction']
                   if d[n].get('Namespace') == 'KlaussCPU'
                   and not d[n]['isPseudo'])
    zres = zero_flag_reusers(llvm)
    errs, tderrs = [], []
    for n in insts:
        if n not in T and n not in SKIP:
            errs.append('%s: no test and not skipped' % n)
            continue
        writes, reads = (T[n][0], T[n][1]) if n in T else (SKIP[n], '')
        defs = 'FLAGS' in [x['def'] for x in d[n]['Defs']]
        uses = 'FLAGS' in [x['def'] for x in d[n]['Uses']]
        if defs != (bool(writes) or n in ABI_CLOBBER):
            tderrs.append('%s: .td %s FLAGS, the CPU writes %s' %
                          (n, 'defines' if defs else 'does not define',
                           writes or 'none'))
        if uses != bool(reads):
            tderrs.append('%s: .td %s FLAGS, the CPU reads %s' %
                          (n, 'uses' if uses else 'does not use',
                           reads or 'none'))
    for n in sorted(zres):
        if n not in T or 'Z' not in T[n][0]:
            tderrs.append('%s: FlagReuse takes its Z, which it does not write'
                          % n)
    for n in T:
        if n not in insts:
            errs.append('%s: tested but not a KlaussCPU instruction' % n)
    names = [n for n in insts if n in T]
    asm, hdr = emit_asm(names), emit_header(names, zres)

    # Each test must assemble to the instruction it names: an instruction
    # of its size with its word-0 fixed bits between fc_<NAME>_t and
    # fc_<NAME>_e.
    with tempfile.TemporaryDirectory() as tmp:
        s, o = os.path.join(tmp, 'c.S'), os.path.join(tmp, 'c.o')
        open(s, 'w').write(asm)
        subprocess.run([os.path.join(bindir, 'llvm-mc'), '-triple=klausscpu',
                        '-filetype=obj', '-o', o, s], check=True)
        dis = subprocess.run([os.path.join(bindir, 'llvm-objdump'), '-d',
                              '--section=.text', o], check=True,
                             capture_output=True, text=True).stdout
        words, cur = {}, None
        for line in dis.splitlines():
            m = re.match(r'^[0-9a-f]+ <fc_(\w+)_([te])>:', line)
            if m:
                cur = m.group(1) if m.group(2) == 't' else None
                if cur:
                    words[cur] = []
                continue
            m = re.match(r'^\s*[0-9a-f]+:\s+((?:[0-9a-f]{2} )+)', line)
            if m and cur:
                words[cur].append(bytes.fromhex(m.group(1).replace(' ', '')))
        for n in names:
            mask, val = fixed_bits(d[n])
            if not any(len(w) == d[n]['Size'] and
                       int.from_bytes(w[:4], 'little') & mask == val
                       for w in words.get(n, [])):
                errs.append('%s: fc_%s does not run it' % (n, n))
    if errs or (tderrs and not force):
        print('\n'.join(tderrs + errs))
        sys.exit(1)
    for fn, text in (('test_flags_cases.S', asm), ('test_flags_cases.h', hdr)):
        p = os.path.join(HERE, fn)
        if check:
            if not os.path.exists(p) or open(p).read() != text:
                print('%s is out of date' % fn)
                sys.exit(1)
        else:
            open(p, 'w').write(text)
    print('%d instructions tested, %d skipped' % (len(names), len(SKIP)))


if __name__ == '__main__':
    main()
