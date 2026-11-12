// dchase PAGES STEPS THRASH : data-side TLB-refill microbenchmark with
// in-program counter deltas.  Pointer chase over PAGES pages (one word per
// 64 KiB page, line within page varies, random cycle).  THRASH=1 touches a
// 128 KiB buffer (16-byte stride) before every step to evict the 64 KiB
// D-cache (including the PTE/PGD lines).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
static void rd(unsigned long long *c,unsigned long long *i,unsigned long long *r,unsigned long long *idle){
  FILE*f=fopen("/proc/klausscpu_perf","r");char k[64];unsigned long long v;
  while(fscanf(f,"%63s %llu",k,&v)==2){
    if(!strcmp(k,"cycles"))*c=v; else if(!strcmp(k,"instructions"))*i=v;
    else if(!strcmp(k,"tlb_refills"))*r=v; else if(!strcmp(k,"idle_cycles"))*idle=v;}
  fclose(f);}
int main(int argc,char**argv){
  int n=atoi(argv[1]); long steps=atol(argv[2]); int th=atoi(argv[3]);
  size_t pg=65536; unsigned seed=12345; long i;
  char*mem=mmap(0,n*pg,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  volatile char*buf=mmap(0,2*pg,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  int*o=malloc(n*sizeof*o);
  for(i=0;i<n;i++)o[i]=i;
  for(i=n-1;i>0;i--){int j=(seed=seed*1103515245+12345)%(i+1),x=o[i];o[i]=o[j];o[j]=x;}
  for(i=0;i<n;i++){void**f=(void**)(mem+o[i]*pg+(o[i]%64)*64);
    void**t=(void**)(mem+o[(i+1)%n]*pg+(o[(i+1)%n]%64)*64);*f=t;}
  for(i=0;i<2*pg;i+=4096)buf[i]=1;
  void**p=(void**)(mem+o[0]*pg+(o[0]%64)*64);
  for(i=0;i<n*4;i++)p=*(void*volatile*)p;
  unsigned long long c0,i0,r0,d0,c1,i1,r1,d1; struct timespec a,b; long sum=0;
  rd(&c0,&i0,&r0,&d0);clock_gettime(CLOCK_MONOTONIC,&a);
  for(i=0;i<steps;i++){
    if(th){for(size_t k=0;k<2*pg;k+=16)sum+=buf[k];}
    p=*(void*volatile*)p;
  }
  clock_gettime(CLOCK_MONOTONIC,&b);rd(&c1,&i1,&r1,&d1);
  double ns=(b.tv_sec-a.tv_sec)*1e9+(b.tv_nsec-a.tv_nsec);
  printf("pages %d steps %ld thrash %d cycles %llu instr %llu idle %llu refills %llu ns %.0f %s\n",n,steps,th,c1-c0,i1-i0,d1-d0,r1-r0,ns,(p&&sum>=0)?"":"");
  return 0;}
