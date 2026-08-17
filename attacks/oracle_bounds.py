import sys; sys.path.insert(0,'/home/ila/Code/privacy/attacks')
import numpy as np, duckdb, itertools
import fineness_sweep as F
from broad_benchmark import Q, load
con=duckdb.connect(config={'threads':2}); con.execute("SET enable_progress_bar=false")
for al,p in (("tpch","tpch_sass_sf10.db"),("so","stackoverflow_dba_sqlstorm.db"),
             ("cb","clickbench_micro.db")):
    con.execute(f"ATTACH '/home/ila/Code/privacy/{p}' AS {al} (READ_ONLY)")
EPS=1.0; T=2; r=np.random.default_rng(5)
print("Bounds are NOT post-processing: U and B come from ApproxBounds, a DP release costing eps_b.")
print("Does the comparison depend on them? Give BOTH arms ORACLE bounds (best U / best B on a")
print("grid, eps_b -> 0 so the saved budget goes to the values) and see if the gap moves.\n")
print(f"{'query':<26}{'--- DP bounds ---':>24}{'--- ORACLE bounds ---':>26}")
print(f"{'':26}{'g-best':>8}{'ours':>8}{'ratio':>8}{'g-best':>9}{'ours':>8}{'ratio':>9}")
print("-"*76)
CV=[1,3,8,21,55]; NG=17
for name in ("tpch SUM price / mo|nation","so COUNT posts / month","cb COUNT hits / region"):
    body,where=Q[name]; c,_=load(con,body,where)
    if len(c.val)>6_000_000: print(f"{name:<26} skipped"); continue
    mk=int(c.k_u.max()); cands=sorted(set(CV)|{mk})
    ranks=[c.rank_random(r) for _ in range(T)]
    vt={(t,ce):F.votes(c,ranks[t],ce) for t in range(T) for ce in cands}
    Ug=[float(np.abs(c.val).max())*2.0**k for k in np.linspace(-4,0,NG)]
    Bg=[float(c.norms.max())*2.0**k for k in np.linspace(-4,0,NG)]
    def otot(B):
        cl=np.clip(c.val,-B,B); nu=np.bincount(c.pi,weights=np.abs(cl),minlength=c.P)
        return np.bincount(c.gi,weights=cl*np.minimum(1.0,B/np.maximum(nu,1e-30))[c.pi],minlength=c.K)
    GT={(cv,i,t):F.google_values(c,cv,U,ranks[t],False)
        for cv in cands for i,U in enumerate(Ug) for t in range(T)}
    OT={i:otot(B) for i,B in enumerate(Bg)}
    ab={}
    for eb in sorted({s[0] for s in F.SPLITS}):
        for t in range(T):
            U=F.approx_bounds(c.val,EPS*eb,r); B=F.approx_bounds(c.norms[c.norms>0],EPS*eb,r)
            ab[(eb,t)]=(U,{cv:F.google_values(c,cv,U,ranks[t],False) for cv in cands},B,otot(B))
    def ev(kind,oracle):
        best=None
        for ce,(eb,ee,evv) in itertools.product(cands,F.SPLITS):
            thr=F.tau(EPS*ee,F.DELTA,ce); evs=evv+eb if oracle else evv
            opts=list(range(NG)) if oracle else [None]
            for cv in (cands if kind=="g" else [None]):
                for idx in opts:
                    es=[]
                    for t in range(T):
                        v=vt[(t,ce)]; rel=(v+r.laplace(0,ce/(EPS*ee),size=c.K)>=thr)&(v>0)
                        if kind=="g":
                            tot,sc=((GT[(cv,idx,t)],cv*Ug[idx]) if oracle
                                    else (ab[(eb,t)][1][cv],cv*ab[(eb,t)][0]))
                        else:
                            tot,sc=((OT[idx],Bg[idx]) if oracle
                                    else (ab[(eb,t)][3],ab[(eb,t)][2]))
                        es.append(c.score(np.where(rel,tot+r.laplace(0,sc/(EPS*evs),size=c.K),0.0)))
                    e=float(np.mean(es)); best=e if best is None or e<best else best
        return best
    g1,o1,g2,o2=ev("g",False),ev("o",False),ev("g",True),ev("o",True)
    print(f"{name:<26}{100*g1:>7.2f}%{100*o1:>7.2f}%{g1/o1:>7.2f}x"
          f"{100*g2:>8.2f}%{100*o2:>7.2f}%{g2/o2:>8.2f}x", flush=True)
    del c,GT,OT,ab
