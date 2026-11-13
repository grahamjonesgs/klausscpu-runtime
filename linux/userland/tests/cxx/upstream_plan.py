#!/usr/bin/env python3
"""upstream_plan.py LLVM_SRC: evaluate the lit REQUIRES/UNSUPPORTED/ADDITIONAL_COMPILE_FLAGS
lines of the libunwind and libc++abi *.pass.cpp/.c tests for klausscpu-unknown-linux-musl
(C++23, threads, exceptions, rtti, no sanitizers).  Prints, tab separated:
  RUN<TAB>suite/name<TAB>path<TAB>extra flags   or   SKIP<TAB>suite/name<TAB>path<TAB>reason"""
import os, re, sys
src = sys.argv[1]
TRIPLE = "klausscpu-unknown-linux-musl"
FEATURES = {"linux", "c++23", "c++2b", "c++17-or-above"} | {"libunwind-" + x for x in ()}
def evaluate(expr):
    """lit boolean expression; target=<literal and {{regex}} parts> is re.search'ed in the triple."""
    def tgt(m):
        rx = "".join(x[2:-2] if x.startswith("{{") else re.escape(x)
                     for x in re.findall(r"\{\{.*?\}\}|[^{]+", m.group(1)))
        return " True " if re.search(rx, TRIPLE) else " False "
    e = re.sub(r"target=((?:\{\{.*?\}\}|[^\s&|()!,{])+)", tgt, expr)
    e = e.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    def tok(m):
        w = m.group(0)
        if w in ("and", "or", "not", "True", "False"): return w
        return "True" if w in FEATURES else "False"
    return eval(re.sub(r"[A-Za-z0-9_.+-]+", tok, e))
for suite in ("libunwind", "libcxxabi"):
    d = os.path.join(src, suite, "test")
    for root, dirs, files in os.walk(d):
        dirs.sort()
        for f in sorted(files):
            if not re.search(r"\.pass\.(cpp|c)$", f) and not re.search(r"\.(pass|sh)\.(S|s|cpp)$|\.sh\.test$", f):
                continue
            p = os.path.join(root, f)
            name = suite + "/" + os.path.relpath(p, d)
            if not re.search(r"\.pass\.(cpp|c)$", f):
                print("SKIP", name, p, "assembly/shell test (needs lit %{run} machinery)", sep="\t"); continue
            if "/native/" in p or "/vendor/" in p:
                print("SKIP", name, p, "other-target/vendor test", sep="\t"); continue
            lines = [l.rstrip("\n") for l in open(p, errors="replace")]
            hdr = [l for l in lines if l.startswith("//")]
            for std in ("c++23", "c++14"):   # c++14 only for tests REQUIRES-ing an old dialect
                FEATURES.discard("c++23"); FEATURES.discard("c++14"); FEATURES.add(std)
                reason = None; flags = ["-std=" + std]
                for l in hdr:
                    m = re.match(r"//\s*(REQUIRES|UNSUPPORTED|ADDITIONAL_COMPILE_FLAGS(?:\([^)]*\))?|RUN|FILE_DEPENDENCIES):\s*(.*)", l)
                    if not m: continue
                    k, v = m.group(1), m.group(2).strip()
                    if k == "REQUIRES":
                        for part in v.split(","):
                            if not evaluate(part.strip()): reason = reason or "REQUIRES: " + part.strip()
                    elif k == "UNSUPPORTED":
                        for part in v.split(","):
                            if evaluate(part.strip()): reason = reason or "UNSUPPORTED: " + part.strip()
                    elif k.startswith("ADDITIONAL_COMPILE_FLAGS"):
                        cond = re.match(r".*\((.*)\)", k)
                        if cond and not evaluate(cond.group(1)): continue
                        flags += v.split()
                    elif k == "RUN" and "%{build}" not in v and "%{run}" not in v and "%{exec}" not in v:
                        reason = reason or "RUN: custom step (" + v[:40] + ")"
                    elif k == "RUN" and "%{objcopy}" in v:
                        reason = reason or "RUN: custom step"
                    elif k == "FILE_DEPENDENCIES":
                        reason = reason or "FILE_DEPENDENCIES"
                if not reason: break
                if std == "c++23" and not (reason.startswith("REQUIRES: c++03")): break
            print("SKIP" if reason else "RUN", name, p, reason or " ".join(flags), sep="\t")
