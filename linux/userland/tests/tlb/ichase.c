// ichase M : instruction-side TLB-refill microbenchmark.  Links against
// ichase_N.S (gen.py N).  Calls chain() M times; prints deltas of the
// board counters (/proc/klausscpu_perf) around the loop.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
extern void chain(void);
static void rd(unsigned long long *c,unsigned long long *i,unsigned long long *r,unsigned long long *idle){
  FILE*f=fopen("/proc/klausscpu_perf","r");char k[64];unsigned long long v;
  while(fscanf(f,"%63s %llu",k,&v)==2){
    if(!strcmp(k,"cycles"))*c=v; else if(!strcmp(k,"instructions"))*i=v;
    else if(!strcmp(k,"tlb_refills"))*r=v; else if(!strcmp(k,"idle_cycles"))*idle=v;}
  fclose(f);}
int main(int argc,char**argv){
  long m=argc>1?atol(argv[1]):10000;
  for(int i=0;i<20;i++)chain();
  unsigned long long c0,i0,r0,d0,c1,i1,r1,d1;
  struct timespec a,b;
  rd(&c0,&i0,&r0,&d0);clock_gettime(CLOCK_MONOTONIC,&a);
  for(long j=0;j<m;j++)chain();
  clock_gettime(CLOCK_MONOTONIC,&b);rd(&c1,&i1,&r1,&d1);
  double ns=(b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec);
  printf("calls %ld cycles %llu instr %llu idle %llu refills %llu ns %.0f\n",m,c1-c0,i1-i0,d1-d0,r1-r0,ns);
  return 0;}
