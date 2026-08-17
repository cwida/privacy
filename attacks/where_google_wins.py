import sys; sys.path.insert(0,'/home/ila/Code/privacy/attacks')
import numpy as np, itertools
import fineness_sweep as F
EPS=1.0; T=3
print("Where does Google DP as published BEAT the l1 clip? Controlled construction.")
print("Most PUs touch one group (so C_v=1 is nearly lossless for Google), plus a fraction f of")
print("'whales' spread over W groups. Whales inflate our B (max per-PU norm) while barely moving")
print("Google's U (max per-cell), and Google truncates them away almost for free.\n")
K=200; NBASE=40000
print(f"{'whale frac':>11}{'W':>5}{'B/U':>7}{'google':>9}{'ours':>8}{'ratio':>8}  who")
print("-"*56)
for f in (0.0002,0.001,0.005,0.02):
    for W in (20,100):
        rr=np.random.default_rng(3)
        pi=[];gi=[];val=[]
        for u in range(NBASE):                       # ordinary PUs: one group, unit-ish value
            pi.append(u); gi.append(rr.integers(K)); val.append(abs(rr.normal(100,20)))
        nw=max(int(NBASE*f),1)
        for w in range(nw):                          # whales: W groups, same per-cell magnitude
            gs=rr.choice(K,size=min(W,K),replace=False)
            for g in gs:
                pi.append(NBASE+w); gi.append(g); val.append(abs(rr.normal(100,20)))
        c=F.Cells.__new__(F.Cells)
        c._init_from(np.array(pi),np.array(gi),np.array(val,dtype=float))
        mk=int(c.k_u.max()); cands=sorted(set([1,2,3,5,8,13,21,34,55,89])|{mk})
        ranks=[c.rank_random(rr) for _ in range(T)]
        vt={(t,ce):F.votes(c,ranks[t],ce) for t in range(T) for ce in cands}
        ca={}
        for eb in sorted({s[0] for s in F.SPLITS}):
            for t in range(T):
                U=F.approx_bounds(c.val,EPS*eb,rr); B=F.approx_bounds(c.norms[c.norms>0],EPS*eb,rr)
                cl=np.clip(c.val,-B,B); nu=np.bincount(c.pi,weights=np.abs(cl),minlength=c.P)
                ca[(eb,t)]=(U,B,{cv:F.google_values(c,cv,U,ranks[t],False) for cv in cands},
                    np.bincount(c.gi,weights=cl*np.minimum(1.0,B/np.maximum(nu,1e-30))[c.pi],minlength=c.K))
        def ev(kind):
            best=None
            for ce,(eb,ee,evv) in itertools.product(cands,F.SPLITS):
                thr=F.tau(EPS*ee,F.DELTA,ce)
                for cv in (cands if kind=="g" else [None]):
                    es=[]
                    for t in range(T):
                        U,B,g,o=ca[(eb,t)]; v=vt[(t,ce)]
                        rel=(v+rr.laplace(0,ce/(EPS*ee),size=c.K)>=thr)&(v>0)
                        tot,sc=(g[cv],cv*U) if kind=="g" else (o,B)
                        es.append(c.score(np.where(rel,tot+rr.laplace(0,sc/(EPS*evv),size=c.K),0.0)))
                    e=float(np.mean(es)); best=e if best is None or e<best else best
            return best
        U0,B0=ca[(0.002,0)][0],ca[(0.002,0)][1]
        gg,oo=ev("g"),ev("o")
        who="GOOGLE" if gg<oo*0.98 else ("ours" if oo<gg*0.98 else "tie")
        print(f"{f:>11.4f}{W:>5}{B0/max(U0,1):>7.1f}{100*gg:>8.2f}%{100*oo:>7.2f}%"
              f"{gg/oo:>7.2f}x  {who}", flush=True)
        del c,ca
