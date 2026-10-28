#!/usr/bin/env python3
"""cfi_check.py -- check an ELF's call frame information against a run.

    cfi_check.py [options] <prog.elf>

Runs <prog.elf> on the klausscc emulator with the per-instruction trace
(--emulate --trace -) and replays the trace against a shadow call stack.
At a call, the shadow stack records what an unwinder must recover for the
new frame: the caller's SP (the CFA), the return address, and the caller's
R15 and callee-saved R4-R7.  Then, before every instruction that runs in a
function with an FDE in .debug_frame, the row that covers its PC
(llvm-dwarfdump --debug-frame) is evaluated on the machine state at that
point and a shadow copy of memory (every store in the trace):

  CFA          must be the caller's SP
  RA           must be the return address
  R15, R4-R7   must be the caller's values: "same" (or no rule) means the
               register still holds it, [CFA+n] that the slot does
  [CFA+n]      a slot below SP is an error too: an interrupt could
               overwrite it before the unwinder reads it

A tail call keeps the frame it replaces (the callee returns to the same
caller), so it needs nothing.  A frame is popped when SP reaches its CFA
(a return or a longjmp).  An interrupt (the next PC is a vector written to
INT_VECn and not where the instruction goes) pushes a frame that is not
checked (the handlers are hand-written, .cfi_signal_frame) until IRET;
calls made in it are.  A SETSP of anything but R15 (a context switch,
longjmp, crt0) leaves the shadow stack untrusted until the next call.

Prints per-function counts of the PCs checked and every disagreement
(function, PC, what, expected, got; the first --show of each), and exits
1 if there is any.  Exits 2 if the run could not be checked: the emulator
stopped other than by halting or reaching --max-instructions (a trap, a
crash, no trace), or not one PC was checked (no FDE parsed, or nothing ran).  Code without an FDE (a library built without -g) is
not checked, but calls through it are followed.

Options:
  --llvm-bin DIR     llvm-dwarfdump / llvm-objdump / llvm-nm (default
                     $KLAUSSCPU_LLVM_BIN)
  --klausscc PATH    the emulator (default $KLAUSSCC, else klausscc)
  --max-instructions N   emulator cap (default 50000000)
  --trace FILE       read a saved trace instead of running the emulator
  --show N           examples printed per distinct disagreement (default 1)
  --quiet            print only the summary line and the disagreements
"""

import argparse
import bisect
import os
import re
import subprocess
import sys
from collections import defaultdict

RAM = 0x8000000  # 128 MiB; MMIO and anything above are not shadowed
INT_VEC_BASE = 0xF00F0010  # INT_VEC0..3 at 8-byte steps (klausscc emu/mmio.rs)
STACK_STEP = 0x10000  # a SETSP this far below SP is an alloca, not a switch
CSR = ("R4", "R5", "R6", "R7", "R15")
TRACE_REG = {f"R{i}": i for i in range(16)}


def run(cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


# ---- static information from the ELF -------------------------------------

ROW_RE = re.compile(r"^\s+0x([0-9a-f]+): CFA=([^:]+)(?:: (.*))?$")
FDE_RE = re.compile(r" FDE cie=([0-9a-f]+) pc=([0-9a-f]+)\.\.\.([0-9a-f]+)")
CIE_RE = re.compile(r"^([0-9a-f]+) [0-9a-f]+ ffffffff CIE")


def parse_cfa(text):
    m = re.fullmatch(r"([A-Z0-9]+)([+-]\d+)?", text.strip())
    if not m:
        return ("unsupported", text)
    return ("reg", m.group(1), int(m.group(2) or 0))


def parse_rules(text):
    rules = {}
    if not text:
        return rules
    for item in text.split(", "):
        reg, _, val = item.partition("=")
        m = re.fullmatch(r"\[CFA([+-]\d+)\]", val)
        if m:
            rules[reg] = ("at", int(m.group(1)))
        elif val == "same":
            rules[reg] = ("same",)
        elif val == "undefined":
            rules[reg] = ("undefined",)
        else:
            rules[reg] = ("unsupported", val)
    return rules


def load_frames(llvm_bin, elf):
    """[(lo, hi, signal, [(pc, cfa, rules)])] sorted by lo."""
    out = run([os.path.join(llvm_bin, "llvm-dwarfdump"), "--debug-frame", elf])
    fdes = []
    cie_aug = {}
    cur_cie = None
    cur = None
    for line in out.splitlines():
        m = CIE_RE.match(line)
        if m:
            cur_cie = m.group(1)
            cie_aug[cur_cie] = ""
            cur = None
            continue
        if cur_cie is not None and cur is None and "Augmentation:" in line:
            cie_aug[cur_cie] = line.split('"')[1] if '"' in line else ""
            continue
        m = FDE_RE.search(line)
        if m:
            cur = [int(m.group(2), 16), int(m.group(3), 16),
                   "S" in cie_aug.get(m.group(1), ""), []]
            fdes.append(cur)
            continue
        m = ROW_RE.match(line)
        if m and cur is not None:
            cur[3].append((int(m.group(1), 16), parse_cfa(m.group(2)),
                           parse_rules(m.group(3))))
    fdes.sort(key=lambda f: f[0])
    return fdes


INSN_RE = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} )+\s*([a-z0-9.]+)\s*(.*)$")


def load_insns(llvm_bin, elf):
    """pc -> (mnemonic, branch target or None)."""
    out = run([os.path.join(llvm_bin, "llvm-objdump"), "-d", elf])
    insns = {}
    for line in out.splitlines():
        m = INSN_RE.match(line)
        if not m:
            continue
        ops = m.group(3)
        t = re.search(r"0x([0-9a-f]+) <", ops)
        insns[int(m.group(1), 16)] = (m.group(2), int(t.group(1), 16) if t else None)
    return insns


def load_funcs(llvm_bin, elf):
    out = run([os.path.join(llvm_bin, "llvm-nm"), "-n", "--defined-only", elf])
    addrs, names = [], []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "tT":
            addrs.append(int(p[0], 16))
            names.append(p[2])
    return addrs, names


# ---- the replay ------------------------------------------------------------

class Frame:
    __slots__ = ("cfa", "ra", "regs", "kind")

    def __init__(self, cfa, ra, regs, kind):
        self.cfa, self.ra, self.regs, self.kind = cfa, ra, regs, kind


def reg_of(line, k, n):
    """Register n (0-15) of a trace line whose ' pc=' is at k."""
    s = k + 28 + 20 * n if n < 10 else k + 229 + 21 * (n - 10)
    return int(line[s:s + 16], 16)


def sp_of(line, k):
    return int(line[k + 354:k + 362], 16)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("--llvm-bin", default=os.environ.get("KLAUSSCPU_LLVM_BIN", ""))
    ap.add_argument("--klausscc", default=os.environ.get("KLAUSSCC", "klausscc"))
    ap.add_argument("--max-instructions", type=int, default=50000000)
    ap.add_argument("--trace")
    ap.add_argument("--show", type=int, default=1)
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    fdes = load_frames(a.llvm_bin, a.elf)
    insns = load_insns(a.llvm_bin, a.elf)
    faddrs, fnames = load_funcs(a.llvm_bin, a.elf)
    fde_lo = [f[0] for f in fdes]

    def func_of(pc):
        i = bisect.bisect_right(faddrs, pc) - 1
        return fnames[i] if i >= 0 else "?"

    row_cache = {}

    def row_of(pc):
        """(signal, cfa, rules) of the row covering pc, or None."""
        r = row_cache.get(pc, False)
        if r is not False:
            return r
        r = None
        i = bisect.bisect_right(fde_lo, pc) - 1
        if i >= 0 and fdes[i][0] <= pc < fdes[i][1]:
            rows = fdes[i][3]
            j = bisect.bisect_right([x[0] for x in rows], pc) - 1
            if j >= 0:
                r = (fdes[i][2], rows[j][1], rows[j][2])
        row_cache[pc] = r
        return r

    mem = bytearray(RAM)
    vectors = set()
    stack = [Frame(1 << 64, None, None, "untrusted")]
    bad = defaultdict(list)
    checked = defaultdict(int)
    nlines = 0
    stop = "?"

    if a.trace:
        src = open(a.trace)
        proc = None
    else:
        proc = subprocess.Popen([a.klausscc, "-i", a.elf, "--emulate", "--trace", "-",
                                 "--max-instructions", str(a.max_instructions)],
                                stdout=subprocess.PIPE, stdin=subprocess.DEVNULL,
                                stderr=subprocess.STDOUT, text=True, bufsize=1 << 20)
        src = proc.stdout

    def report(pc, what, want, got):
        key = (func_of(pc), pc, what)
        if len(bad[key]) < a.show:
            bad[key].append((want, got))
        else:
            bad[key].append(None)

    def check(pc, line, k):
        """Evaluate the row at pc on the state before it (line = previous)."""
        fr = stack[-1]
        if fr.kind != "call":
            return
        row = row_of(pc)
        if row is None:
            return
        signal, cfa_rule, rules = row
        if signal:
            return
        checked[func_of(pc)] += 1
        sp = sp_of(line, k)
        if cfa_rule[0] != "reg" or (cfa_rule[1] != "SP" and cfa_rule[1] not in TRACE_REG):
            report(pc, "CFA", "SP or Rn + offset", repr(cfa_rule))
            return
        base = sp if cfa_rule[1] == "SP" else reg_of(line, k, TRACE_REG[cfa_rule[1]])
        cfa = (base + cfa_rule[2]) & 0xFFFFFFFFFFFFFFFF
        if cfa != fr.cfa:
            report(pc, "CFA", hex(fr.cfa), f"{hex(cfa)} ({cfa_rule[1]}{cfa_rule[2]:+d})")
            return

        def recover(name, cur):
            rule = rules.get(name, ("same",))
            if rule[0] == "same":
                return cur, "same"
            if rule[0] == "at":
                addr = cfa + rule[1]
                if addr < sp:
                    report(pc, f"{name} slot", f">= SP {hex(sp)}", f"[CFA{rule[1]:+d}] = {hex(addr)}")
                if 0 <= addr <= RAM - 8:
                    return int.from_bytes(mem[addr:addr + 8], "little"), f"[CFA{rule[1]:+d}]"
                return None, f"[CFA{rule[1]:+d}] out of RAM"
            return None, rule[0]

        ra, how = recover("RA", None)
        if ra != fr.ra:
            report(pc, "RA", hex(fr.ra), f"{hex(ra) if ra is not None else '-'} ({how})")
        for i, name in enumerate(CSR):
            n = TRACE_REG[name]
            v, how = recover(name, reg_of(line, k, n))
            if v != fr.regs[i]:
                report(pc, name, hex(fr.regs[i]), f"{hex(v) if v is not None else '-'} ({how})")

    prev = None  # previous trace line and its ' pc=' index
    pk = 0
    prev_line, prev_k = None, 0
    for line in src:
        if not line.startswith("i="):
            m = re.search(r"stop = (\w+)", line)
            if m:
                stop = m.group(1)
            continue
        nlines += 1
        k = line.find(" pc=")
        pc = int(line[k + 4:k + 12], 16)
        if prev is not None:
            ppc = int(prev[pk + 4:pk + 12], 16)
            plen = 4 * (int(prev[pk + 16], 16) >> 2) or 4
            mn, tgt = insns.get(ppc, ("?", None))
            # An interrupt entry: not where the previous instruction went.
            if pc in vectors and pc != ppc + plen and pc != tgt:
                psp = sp_of(prev, pk)
                stack.append(Frame(psp, None, None, "irq"))
            else:
                check(pc, prev, pk)
        prev_line, prev_k = prev, pk
        prev, pk = line, k

        # The effects of this instruction (state after it is on this line).
        prev_sp = sp_of(prev_line, prev_k) if prev_line is not None else 0
        sp_after = sp_of(line, k)
        if len(line) > k + 404 and line[k + 372:k + 376] == " wr=":
            addr = int(line[k + 376:k + 384], 16)
            data = int(line[k + 388:k + 404], 16)
            if addr < RAM - 8:
                mem[addr:addr + 8] = data.to_bytes(8, "little")
            elif INT_VEC_BASE <= addr < INT_VEC_BASE + 32 and data:
                vectors.add(data & 0xFFFFFFFF)
        mn = insns.get(pc, ("?", None))[0]
        if mn.startswith("call"):
            plen = 4 * (int(line[k + 16], 16) >> 2) or 4
            # Taken (a conditional call may not be): pushed PC+len at the new SP.
            if (line[k + 372:k + 376] == " wr=" and int(line[k + 376:k + 384], 16) == sp_after
                    and int(line[k + 388:k + 404], 16) == pc + plen):
                # The callee-saved registers as the call leaves them.
                regs = tuple(reg_of(line, k, TRACE_REG[r]) for r in CSR)
                stack.append(Frame(sp_after + 8, pc + plen, regs, "call"))
                continue
        elif (mn == "setsp" and (int(line[k + 16:k + 24], 16) >> 4) & 0xF != 15
              and not (prev_sp - STACK_STEP <= sp_after < stack[-1].cfa)):
            # SETSP rX (rs1, bits 7:4) other than an epilogue's SETSP R15 or
            # a stack restore within the frame (alloca): a context switch,
            # longjmp or crt0.  (A switch that looks like a restore leaves
            # the wrong frame on top: every check then fails, loudly.)
            stack[:] = [Frame(1 << 64, None, None, "untrusted")]
            continue
        while len(stack) > 1 and sp_after >= stack[-1].cfa:
            stack.pop()

    if proc is not None:
        proc.wait()

    nbad = sum(len(v) for v in bad.values())
    total = sum(checked.values())
    print(f"cfi_check {os.path.basename(a.elf)}: {nlines} instructions, {total} checked "
          f"in {len(checked)} functions, {len(bad)} distinct disagreements "
          f"({nbad} instances), stop={stop}")
    if not a.quiet:
        for f in sorted(checked):
            print(f"  checked {checked[f]:10d}  {f}")
    for (f, pc, what), ex in sorted(bad.items(), key=lambda x: x[0][1]):
        shown = [e for e in ex if e]
        print(f"  BAD {f}+{pc - faddrs[bisect.bisect_right(faddrs, pc) - 1]:#x} "
              f"pc={pc:#x} {what}: x{len(ex)}"
              + "".join(f"; want {w} got {g}" for w, g in shown))
    if bad:
        return 1
    if stop not in ("Halt", "InstructionCap"):
        print(f"  ERROR the emulator stopped with {stop}, not Halt or InstructionCap")
        return 2
    if total == 0:
        print("  ERROR no PC was checked")
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
