#!/bin/sh
# bench.sh [quick]: CoreMark, the lmbench subset (lat_syscall, lat_proc,
# lat_ctx, lat_pagefault, bw_mem, lat_mem_rd, tlb) and tlbmiss.  Installed
# in the system image as /usr/share/klausscpu/tests/bench.sh.
#   bench.sh        the board: CoreMark's own >= 10 s run, lmbench's
#                   default repetitions, tlbmiss up to 64 pages
#   bench.sh quick  the emulator check: everything runs, briefly (the
#                   numbers mean nothing there)
# lmbench prints its results on stderr; they are merged into stdout here.
# CoreMark counts a run shorter than 10 s as an error ("Errors detected"),
# so the quick run is checked by its CRCs instead (list 0xe714, matrix
# 0x1fd7, state 0x8e3a for these seeds).
L=/usr/libexec/lmbench
if [ "${1:-}" = quick ]; then
	N="-N 1 -W 0"; CM="0x0 0x0 0x66 20"; MEM=64k; MEMRD=1; CTX="2 4"
	TLBM="-M 1M"; TLBA=100000
else
	N=""; CM=""; MEM=8m; MEMRD=32; CTX="2 4 8 16"; TLBM=""; TLBA=2000000
fi
echo "== coremark $CM"
coremark $CM 2>&1
cp "$L/hello" /tmp/hello
echo "== lat_syscall"
for s in null read write; do "$L/lat_syscall" $N $s 2>&1; done
"$L/lat_syscall" $N stat /tmp 2>&1
"$L/lat_syscall" $N open /tmp 2>&1
echo "== lat_proc"
for p in fork exec shell; do "$L/lat_proc" $N $p 2>&1; done
echo "== lat_ctx"
"$L/lat_ctx" $N -s 0 $CTX 2>&1
echo "== lat_pagefault"
head -c 4194304 /dev/zero >/tmp/pf && "$L/lat_pagefault" $N /tmp/pf 2>&1
rm -f /tmp/pf
echo "== bw_mem $MEM"
for w in rd wr cp bcopy; do printf '%s ' $w; "$L/bw_mem" $N $MEM $w 2>&1; done
echo "== lat_mem_rd ${MEMRD}M stride 64"
"$L/lat_mem_rd" $N $MEMRD 64 2>&1
echo "== tlb $TLBM"
"$L/tlb" $N $TLBM 2>&1
echo "== tlbmiss"
tlbmiss 64 $TLBA
echo "bench: done"
