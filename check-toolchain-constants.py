#!/usr/bin/env python3
"""Check that the toolchain constants copied outside klausscpu-llvm agree.

klausscpu-llvm owns the KlaussCPU ELF machine number and relocation types
(llvm/include/llvm/BinaryFormat/ELF.h, ELFRelocs/KlaussCPU.def) and the ABI
and ISA facts in llvm/include/llvm/TargetParser/KlaussCPUABI.h (register
names and DWARF numbers, argument/return/callee-saved registers, NOP/HALT/
TRAP/JMPREL words; KlaussCPU unittests check that header against the
backend's TableGen files).  This script compares the copies in this
repository and in klausscc with them:

  klausscpu_elf.h                          EM_KLAUSSCPU, R_KLAUSSCPU_*, PCREL32 bias
  zephyr-ws/.../arch/klausscpu/core/elf.c  R_KLAUSSCPU_* (all handled), bias
  debug/klausscpu_rtos.py                  register names, DWARF and generic numbers
  klausscpu.ld                             == klausscpu-llvm's rust/klausscpu.ld
  src/setjmp.S                             saves and restores exactly the
                                             callee-saved GPRs and R15
  freertos portmacro.h, Zephyr arch.h and  the RTOS stack alignment (a multiple
    arch/klausscpu/Kconfig                   of StackAlign)
  klausscc src/debug/arch.rs               TRAP_WORD, the gdbserver target XML
  klausscc src/isa/table.rs                NOP, HALT, TRAP, JMPREL templates

Usage: check-toolchain-constants.py [--llvm DIR] [--klausscc DIR]
(defaults: the klausscpu-llvm and klausscc checkouts next to this one).
Exits 1 on any mismatch.
"""

import argparse
import ast
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
errors = []


def fail(msg):
    errors.append(msg)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def c_int(text):
    """A C/C++/Rust integer literal (hex or decimal, digit separators, suffixes)."""
    text = text.replace("_", "").replace("'", "").rstrip("uUlL")
    return int(text, 0)


def defines(text, prefix):
    """#define NAME VALUE for every NAME starting with prefix."""
    return {m.group(1): c_int(m.group(2))
            for m in re.finditer(r"^\s*#\s*define\s+(%s\w*)\s+([0-9xXa-fA-F_']+[uUlL]*)"
                                 % prefix, text, re.M)}


def expect(what, got, want):
    if got != want:
        fail("%s: %r, but klausscpu-llvm says %r" % (what, got, want))


# ---- klausscpu-llvm: the sources of truth ---------------------------------


def llvm_facts(llvm):
    inc = os.path.join(llvm, "llvm", "include", "llvm")
    elf_h = read(os.path.join(inc, "BinaryFormat", "ELF.h"))
    m = re.search(r"EM_KLAUSSCPU\s*=\s*(0x[0-9A-Fa-f]+)", elf_h)
    em = int(m.group(1), 16)
    relocs = {m.group(1): int(m.group(2)) for m in re.finditer(
        r"ELF_RELOC\((R_KLAUSSCPU_\w+),\s*(\d+)\)",
        read(os.path.join(inc, "BinaryFormat", "ELFRelocs", "KlaussCPU.def")))}
    abi_text = read(os.path.join(inc, "TargetParser", "KlaussCPUABI.h"))
    abi = {}
    for m in re.finditer(r"inline constexpr \w+ (\w+) = (0x[0-9A-Fa-f]+|\d+);",
                         abi_text):
        abi[m.group(1)] = int(m.group(2), 0)
    for m in re.finditer(r"inline constexpr unsigned (\w+)\[\] = \{([^}]*)\};",
                         abi_text):
        abi[m.group(1)] = [int(x) for x in m.group(2).split(",")]
    m = re.search(r"GPRNames\[NumGPRs\] = \{([^}]*)\};", abi_text)
    abi["GPRNames"] = re.findall(r'"(\w+)"', m.group(1))
    for key in ("NopWord", "HaltWord", "TrapWord", "JmpRelWord", "Word1Offset",
                "DwarfSP", "DwarfRA", "FPReg", "AddressBits", "ArgGPRs",
                "CalleeSavedGPRs", "NumGPRs", "StackAlign"):
        if key not in abi:
            sys.exit("check-toolchain-constants: KlaussCPUABI.h has no %s" % key)
    if len(abi["GPRNames"]) != abi["NumGPRs"]:
        sys.exit("check-toolchain-constants: cannot parse KlaussCPUABI.h GPRNames")
    return em, relocs, abi


# ---- this repository ------------------------------------------------------


def check_runtime(em, relocs, abi, llvm):
    hdr = read(os.path.join(HERE, "klausscpu_elf.h"))
    d = defines(hdr, "EM_KLAUSSCPU")
    expect("klausscpu_elf.h EM_KLAUSSCPU", d.get("EM_KLAUSSCPU"), em)
    expect("klausscpu_elf.h relocation types", defines(hdr, "R_KLAUSSCPU_"),
           relocs)
    expect("klausscpu_elf.h KLAUSSCPU_PCREL32_BIAS",
           defines(hdr, "KLAUSSCPU_PCREL32_BIAS").get("KLAUSSCPU_PCREL32_BIAS"),
           abi["Word1Offset"])

    # Zephyr's LLEXT relocates ET_REL extensions, which may hold any type.
    elf_c = read(os.path.join(HERE, "zephyr-ws", "klausscpu-zephyr", "arch",
                              "klausscpu", "core", "elf.c"))
    zr = defines(elf_c, "R_KLAUSSCPU_")
    expect("Zephyr elf.c relocation types", zr, relocs)
    for name in relocs:
        if not re.search(r"case\s+%s\s*:" % name, elf_c):
            fail("Zephyr elf.c arch_elf_relocate() does not handle %s" % name)
    expect("Zephyr elf.c KLAUSSCPU_PCREL_INSN_BIAS",
           defines(elf_c, "KLAUSSCPU_PCREL_INSN_BIAS").get(
               "KLAUSSCPU_PCREL_INSN_BIAS"), abi["Word1Offset"])

    # The lldb OS plugin's register layout (it mirrors klausscc's target XML).
    rtos = read(os.path.join(HERE, "debug", "klausscpu_rtos.py"))
    m = re.search(r"^_REGS = \(\n(.*?)\n\)", rtos, re.M | re.S)
    body = m.group(1) if m else ""
    dwarf = {}
    m = re.search(r'\[\("r%d" % n, 64, n\) for n in range\((\d+)\)\]', body)
    for n in range(int(m.group(1)) if m else 0):
        dwarf["r%d" % n] = n
    for name, _, num in re.findall(r'\("(\w+)", (\d+), (\d+|None)\)', body):
        if num != "None":
            dwarf[name] = int(num)
    expect("debug/klausscpu_rtos.py DWARF numbers", dwarf, want_dwarf(abi))
    m = re.search(r"^_GENERIC = (\{.*?\})", rtos, re.M | re.S)
    expect("debug/klausscpu_rtos.py generic registers",
           {k: v for k, v in ast.literal_eval(m.group(1)).items()
            if v.startswith("arg") or v == "fp"}, want_generic(abi))

    # The RTOS ports keep task stacks at least as aligned as the ABI's SP.
    zephyr = os.path.join(HERE, "zephyr-ws", "klausscpu-zephyr")
    aligns = (
        ("freertos portmacro.h portBYTE_ALIGNMENT",
         defines(read(os.path.join(HERE, "freertos", "portable", "KlaussCPU",
                                   "portmacro.h")), "portBYTE_ALIGNMENT"
                 ).get("portBYTE_ALIGNMENT")),
        ("Zephyr arch.h ARCH_STACK_PTR_ALIGN",
         defines(read(os.path.join(zephyr, "include", "zephyr", "arch",
                                   "klausscpu", "arch.h")),
                 "ARCH_STACK_PTR_ALIGN").get("ARCH_STACK_PTR_ALIGN")),
        ("Zephyr Kconfig STACK_ALIGN_SIZE",
         kconfig_default(read(os.path.join(zephyr, "arch", "klausscpu",
                                           "Kconfig")), "STACK_ALIGN_SIZE")),
    )
    for what, got in aligns:
        if got is None or got % abi["StackAlign"]:
            fail("%s: %r, but klausscpu-llvm's StackAlign is %d (it must be a "
                 "multiple)" % (what, got, abi["StackAlign"]))

    # setjmp/longjmp save and restore the callee-saved GPRs and the frame
    # pointer: no fewer (a longjmp would corrupt the caller's registers), no
    # more (jmp_buf is sized for them).
    sj = read(os.path.join(HERE, "src", "setjmp.S"))
    want = sorted(abi["CalleeSavedGPRs"] + [abi["FPReg"]])
    for fn, op in (("setjmp", "stidx64"), ("longjmp", "ldidx64")):
        m = re.search(r"^%s:\n(.*?)\.size" % fn, sj, re.M | re.S)
        body = m.group(1) if m else ""
        # R12-R14 carry the saved SP and return address (scratch registers).
        got = sorted({int(r) for r in re.findall(
            r"^\s*%s\s+r(\d+),\s*r0,\s*\d+" % op, body, re.M)} - {12, 13, 14})
        expect("src/setjmp.S %s registers" % fn, got, want)

    # The Rust bare-metal linker script is a copy of klausscpu.ld.
    rust_ld = os.path.join(llvm, "llvm", "lib", "Target", "KlaussCPU", "rust",
                           "klausscpu.ld")
    if os.path.exists(rust_ld):
        strip = lambda t: re.sub(r"/\*.*?\*/", "", t, flags=re.S).strip()
        if strip(read(rust_ld)) != strip(read(os.path.join(HERE, "klausscpu.ld"))):
            fail("klausscpu.ld and %s differ (outside comments)" % rust_ld)


def kconfig_default(text, name):
    """The integer default of Kconfig symbol name."""
    m = re.search(r"^config %s\n(?:[ \t].*\n)*?[ \t]+default (\w+)" % name,
                  text, re.M)
    return c_int(m.group(1)) if m else None


def want_dwarf(abi):
    d = {name: n for n, name in enumerate(abi["GPRNames"])}
    d["sp"] = abi["DwarfSP"]
    d["pc"] = abi["DwarfRA"]
    return d


def want_generic(abi):
    g = {abi["GPRNames"][r]: "arg%d" % (i + 1) for i, r in enumerate(abi["ArgGPRs"])}
    g[abi["GPRNames"][abi["FPReg"]]] = "fp"
    return g


# ---- klausscc ----------------------------------------------------------------


def check_klausscc(abi, kcc):
    arch = read(os.path.join(kcc, "src", "debug", "arch.rs"))
    m = re.search(r"const TRAP_WORD: u32 = ([0-9xA-Fa-f_]+);", arch)
    expect("klausscc TRAP_WORD", m and c_int(m.group(1)), abi["TrapWord"])
    regs = {}
    for m in re.finditer(r'<reg name="(\w+)"([^>]*)/>', arch):
        attrs = dict(re.findall(r'(\w+)="([^"]*)"', m.group(2)))
        regs[m.group(1)] = attrs
    dwarf = {n: int(a["dwarf_regnum"]) for n, a in regs.items() if "dwarf_regnum" in a}
    expect("klausscc target XML DWARF numbers", dwarf, want_dwarf(abi))
    generic = {n: a["generic"] for n, a in regs.items()
               if a.get("generic", "").startswith("arg") or a.get("generic") == "fp"}
    expect("klausscc target XML generic registers", generic, want_generic(abi))
    for n in ("sp", "pc"):
        expect("klausscc target XML %s bitsize" % n,
               int(regs.get(n, {}).get("bitsize", 0)), abi["AddressBits"])

    table = read(os.path.join(kcc, "src", "isa", "table.rs"))
    for mnem, key in (("NOP", "NopWord"), ("HALT", "HaltWord"),
                      ("TRAP", "TrapWord"), ("JMPREL", "JmpRelWord")):
        m = re.search(r'op\("%s",\s*([0-9xA-Fa-f_]+)' % mnem, table)
        expect("klausscc isa/table.rs %s" % mnem, m and c_int(m.group(1)), abi[key])


def main():
    src = os.path.dirname(HERE)
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--llvm", default=os.path.join(src, "klausscpu-llvm"))
    ap.add_argument("--klausscc", default=os.path.join(src, "klausscc"))
    args = ap.parse_args()

    em, relocs, abi = llvm_facts(args.llvm)
    check_runtime(em, relocs, abi, args.llvm)
    check_klausscc(abi, args.klausscc)
    for e in errors:
        print("MISMATCH: " + e)
    if errors:
        return 1
    print("toolchain constants agree (%d relocation types, %d registers)"
          % (len(relocs), abi["NumGPRs"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
