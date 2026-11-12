#!/usr/bin/env python3
# analyze.py [DIR]: the tables of run.sh's output in DIR (default
# linux-ws/tlbsim): tlbsim.out (perfstat per workload) and tlbsim.err
# (the TLBSIM windows), as Markdown on stdout.  TLB_ENTRIES (default 32,
# klausscc's default; 16 for a --tlb-entries 16 run) names the configuration
# that models the real TLB (FA<n> RR), compared with the kernel's count.
import os,re,sys
D=sys.argv[1] if len(sys.argv)>1 else os.path.join(os.path.dirname(os.path.abspath(__file__)),'../../../../../linux-ws/tlbsim')
REAL='FA%s RR'%os.environ.get('TLB_ENTRIES','32')
out=open(os.path.join(D,'tlbsim.out'),errors='replace').read()
names=re.findall(r'^@@BEGIN (\S+)',out,re.M)
perf=re.split(r'^@@BEGIN \S+\n',out,flags=re.M)[1:]
pf=[]
for blk in perf:
    blk=blk.split('@@END')[0]
    ins=int(float(re.search(r'instructions\s+([\d.]+)',blk).group(1)))
    ref=int(float(re.search(r'tlb_refills\s+([\d.]+)',blk).group(1)))
    pf.append((ins,ref))
wins=[];cur=None
for l in open(os.path.join(D,'tlbsim.err'),errors='replace'):
    if l.startswith('TLBSIM window'):
        cur={'w':dict(re.findall(r'(\w+)=(\d+)',l)),'c':[],'u':[]};wins.append(cur)
    elif l.startswith('TLBSIM cfg'):
        m=re.match(r'TLBSIM cfg="([^"]+)" (.*)',l); cur['c'].append((m.group(1),{k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',m.group(2))}))
    elif l.startswith('TLBSIM uitlb'):
        m=re.match(r'TLBSIM uitlb="([^"]+)" (.*)',l); cur['u'].append((m.group(1),{k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',m.group(2))}))
print("| workload | perfstat instr | kernel tlb_refills | sim "+REAL+" | diff | sim real_refills(TLBWI) | real hit/sim miss | real miss/sim hit | non-refill misses | refills /1000 instr |")
print("|---|---|---|---|---|---|---|---|---|---|")
for n,(ins,ref),w in zip(names,pf,wins):
    fa=dict(w['c'])[REAL]['misses']
    W=w['w']
    print(f"| {n} | {ins} | {ref} | {fa} | {fa-ref:+d} ({100*(fa-ref)/ref:+.3f}%) | {W['real_refills']} | {W['div_real_hit_sim_miss']} | {W['div_real_miss_sim_hit']} | {W['nonrefill_misses']} | {1000*ref/ins:.2f} |")
for n,(ins,ref),w in zip(names,pf,wins):
    W=w['w']
    print(f"\n### {n}  (instr {ins}, user fetches {W['fetches']}, lookups F/L/S {W['lookups_fetch']}/{W['lookups_load']}/{W['lookups_store']})")
    print("| config | misses | fetch | load | store | kmap | remiss16 | remiss@size | compulsory | misses/1000 instr |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for c,s in w['c']:
        print(f"| {c} | {s['misses']} | {s['fetch']} | {s['load']} | {s['store']} | {s['kmap']} | {s['remiss16']} | {s['remiss_size']} | {s['compulsory']} | {1000*s['misses']/ins:.3f} |")
    for c,s in w['u']:
        print(f"| {c} | {s['misses']} uITLB misses of {s['fetches']} fetches = {1000*s['misses']/max(1,s['fetches']):.1f} per 1000 fetches |")
