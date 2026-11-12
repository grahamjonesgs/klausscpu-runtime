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
#
# Every benchmark runs under a time limit (busybox here has no timeout
# applet), so one that hangs is reported and the rest still run.  lmbench's
# tlb gets -M: its default search goes to 16384 pages, which with 64 KiB
# pages is 1 GiB, far beyond the board's 128 MiB (it stalled the board run
# after lat_mem_rd); 8 MiB is 128 pages, 8x the TLB's 16 entries.
L=/usr/libexec/lmbench
limit() { # <seconds> <command...>: run it, killed after that long
	s=$1
	shift
	"$@" &
	p=$!
	# lmbench measures in forked children: they go too (pkill -P), or a
	# child left measuring would load every benchmark after it.
	( sleep "$s"; pkill -P "$p" 2>/dev/null; kill "$p" 2>/dev/null && echo "bench: ${1##*/} stopped after $s s" ) &
	w=$!
	wait "$p"
	r=$?
	kill "$w" 2>/dev/null
	return $r
}
if [ "${1:-}" = quick ]; then
	N="-N 1 -W 0"; CM="0x0 0x0 0x66 20"; MEM=64k; MEMRD=1; CTX="2 4"; MEMRDN=$N
	# -M 4M: the TLB holds 32 x 64 KiB pages (2 MiB) since klaussCPU tlb32;
	# within 1 MiB lmbench's tlb finds no edge and prints nothing.
	TLBM="-M 4M"; TLBA=100000; T=900
else
	N=""; CM=""; MEM=8m; MEMRD=32; CTX="2 4 8 16"; TLBM="-c -M 8M"; TLBA=2000000; T=600
	# lat_mem_rd's ~110 sizes at the default 11 repetitions take far
	# longer than T; one repetition is about 1.3 s a size.
	MEMRDN="-N 1"
fi
echo "== coremark $CM"
limit $T coremark $CM 2>&1
cp "$L/hello" /tmp/hello
echo "== lat_syscall"
for s in null read write; do limit $T "$L/lat_syscall" $N $s 2>&1; done
limit $T "$L/lat_syscall" $N stat /tmp 2>&1
limit $T "$L/lat_syscall" $N open /tmp 2>&1
echo "== lat_proc"
for p in fork exec shell; do limit $T "$L/lat_proc" $N $p 2>&1; done
echo "== lat_ctx"
limit $T "$L/lat_ctx" $N -s 0 $CTX 2>&1
echo "== lat_pagefault"
head -c 4194304 /dev/zero >/tmp/pf && limit $T "$L/lat_pagefault" $N /tmp/pf 2>&1
rm -f /tmp/pf
echo "== bw_mem $MEM"
for w in rd wr cp bcopy; do printf '%s ' $w; limit $T "$L/bw_mem" $N $MEM $w 2>&1; done
echo "== lat_mem_rd ${MEMRD}M stride 64"
limit $T "$L/lat_mem_rd" $MEMRDN $MEMRD 64 2>&1
echo "== tlb $TLBM"
limit $T "$L/tlb" $N $TLBM 2>&1
echo "== tlbmiss"
limit $T tlbmiss 64 $TLBA
echo "bench: done"
