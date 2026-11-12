#!/bin/sh
# wl.sh NAME CMD...: 3 runs of perfstat, one summary line each
name=$1; shift
for r in 1 2 3; do
  perfstat "$@" 2>&1 | awk -v n="$name" '$1=="cycles"{c=$2} $1=="instructions"{i=$2} $1=="idle_cycles"{d=$2} $1=="tlb_refills"{t=$2} $1=="ic_miss"{im=$2} $1=="dc_read_misses"{dm=$2} END{printf "%s cycles %d instr %d idle %d refills %d icmiss %d dcrmiss %d\n",n,c,i,d,t,im,dm}'
done
