#!/usr/bin/env python3
# gen.py N > ichase_N.S : N code stubs, each in its own 64 KiB page, at a
# different offset within the page (distinct I-cache index bits [13:4]),
# linked in a fixed shuffled cycle; chain() enters stub order[0], the last
# stub returns.  Each stub: setru r1, next; jmpr r1.
import sys, random
n = int(sys.argv[1])
random.seed(12345)
order = list(range(n)); random.shuffle(order)
nxt = {order[i]: order[i+1] for i in range(n-1)}
print("\t.text\n\t.globl chain\n\t.p2align 3")
print("chain:\n\tjmp\tstub%d" % order[0] if False else "chain:\n\tsetru\tr1, stub%d\n\tjmpr\tr1" % order[0])
for k in range(n):
    off = ((k * 37) % 1024) * 16
    print("\t.balign 65536\n\t.org . + %d\nstub%d:" % (off, k))
    if k in nxt: print("\tsetru\tr1, stub%d\n\tjmpr\tr1" % nxt[k])
    else: print("\tret")
