import sys; sys.path.insert(0,'/home/ila/Code/privacy/attacks')
import numpy as np, duckdb, itertools
import fineness_sweep as F
from em_cu import em_select
con=duckdb.connect(config={'threads':2}); con.execute("SET enable_progress_bar=false")
con.execute("ATTACH '/home/ila/Code/privacy/tpch_sass_sf10.db' AS tpch (READ_ONLY)")
EPS=1.0; T=2; r=np.random.default_rng(5); EM=0.01; P=0.95
BODY="""SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m')||'|'||cast(c_nationkey as varchar) g,
 sum(l_extendedprice) t FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
 JOIN tpch.customer ON c_custkey=o_custkey JOIN tpch.nation ON n_nationkey=c_nationkey
 WHERE {W} GROUP BY 1,2"""
FILTERS={"none (the frozen query)":"true",
         "PU-side: acctbal>=8000":"c_acctbal>=8000",
         "PU-side: acctbal>=9500":"c_acctbal>=9500",
         "PU-side: acctbal>=9900":"c_acctbal>=9900",
         "GRP-side: ship>=1996":"l_shipdate>=DATE '1996-01-01'",
         "GRP-side: ship>=1998":"l_shipdate>=DATE '1998-01-01'",
         "GRP-side: nation<5":"c_nationkey<5"}
def load(w):
    rows=con.execute(f"""WITH c AS ({BODY.format(W=w)})
      SELECT dense_rank() OVER (ORDER BY pu)-1 pid, dense_rank() OVER (ORDER BY g)-1 gid, t
      FROM c""").fetchnumpy()
    c=F.Cells.__new__(F.Cells)
    c._init_from(rows["pid"].astype(np.int64),rows["gid"].astype(np.int64),rows["t"].astype(np.float64))
    return c
print("Q(1): is a single FROZEN C_u as good as choosing C_u per query?")
print("Frozen = the EM run once on the unfiltered query. Per-query = the EM run on each query.")
print("Both at p=0.95; per-query additionally pays eps_N+eps_C=0.01 EVERY time.\n")
base=load("true"); mkb=int(base.k_u.max())
frozen_cu=int(np.median([em_select(base.k_u,mkb,P,EM/5,4*EM/5,0.01,r)[0] for _ in range(9)]))
print(f"frozen C_u (from the unfiltered query, {base.P:,} PUs, max k_u={mkb}) = {frozen_cu}\n")
del base
print(f"{'query':<26}{'PUs':>9}{'k_u p50':>8}{'p95':>6}{'per-q C_u':>11}"
      f"{'frozen':>8}{'per-query':>11}{'best':>7}")
print("-"*88)
for name,w in FILTERS.items():
    c=load(w)
    if len(c.val)>6_500_000: print(f"{name:<26} skipped ({len(c.val):,} cells)"); del c; continue
    mk=int(c.k_u.max()); cands=sorted(set([1,2,3,5,8,13,21,34,55,89])|{mk,frozen_cu})
    ranks=[c.rank_random(r) for _ in range(T)]
    vt={(t,ce):F.votes(c,ranks[t],ce) for t in range(T) for ce in cands}
    ca={}
    for eb in sorted({s[0] for s in F.SPLITS}):
        for t in range(T):
            for eq in (EPS,EPS-EM):
                U=F.approx_bounds(c.val,eq*eb,r)
                ca[(eb,t,eq)]=(U,{cv:F.google_values(c,cv,U,ranks[t],False) for cv in cands})
    def err(cv,eq):
        best=None
        for ce,(eb,ee,ev) in itertools.product(cands,F.SPLITS):
            thr=F.tau(eq*ee,F.DELTA,ce); es=[]
            for t in range(T):
                U,g=ca[(eb,t,eq)]; v=vt[(t,ce)]
                rel=(v+r.laplace(0,ce/(eq*ee),size=c.K)>=thr)&(v>0)
                es.append(c.score(np.where(rel,g[cv]+r.laplace(0,cv*U/(eq*ev),size=c.K),0.0)))
            e=float(np.mean(es)); best=e if best is None or e<best else best
        return best
    pq=int(np.median([em_select(c.k_u,mk,P,EM/5,4*EM/5,0.01,r)[0] for _ in range(9)]))
    nf=min(cands,key=lambda y:abs(y-frozen_cu)); npq=min(cands,key=lambda y:abs(y-pq))
    e_f=err(nf,EPS)                 # frozen: no per-query EM cost
    e_p=err(npq,EPS-EM)             # per-query: pays the EM each time
    e_b=min(err(cv,EPS) for cv in cands)
    print(f"{name:<26}{c.P:>9,}{np.quantile(c.k_u,.5):>8.0f}{np.quantile(c.k_u,.95):>6.0f}"
          f"{pq:>11}{100*e_f:>7.2f}%{100*e_p:>10.2f}%{100*e_b:>6.2f}%")
    del c,ca
