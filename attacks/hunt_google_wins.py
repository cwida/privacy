import sys; sys.path.insert(0,'/home/ila/Code/privacy/attacks')
import numpy as np, duckdb, itertools
import fineness_sweep as F
con=duckdb.connect(config={'threads':2}); con.execute("SET enable_progress_bar=false")
for al,p in (("so","stackoverflow_dba_sqlstorm.db"),("cb","clickbench_micro.db")):
    con.execute(f"ATTACH '/home/ila/Code/privacy/{p}' AS {al} (READ_ONLY)")
EPS=1.0; T=3; r=np.random.default_rng(5)
Q={
 "so SUM ViewCount / month":"""SELECT OwnerUserId pu, strftime(CreationDate,'%Y-%m') g,
   sum(coalesce(ViewCount,0)) t FROM so.Posts WHERE OwnerUserId IS NOT NULL GROUP BY 1,2""",
 "so SUM ViewCount / day":"""SELECT OwnerUserId pu, cast(cast(CreationDate as date) as varchar) g,
   sum(coalesce(ViewCount,0)) t FROM so.Posts WHERE OwnerUserId IS NOT NULL GROUP BY 1,2""",
 "so SUM AnswerCount / month":"""SELECT OwnerUserId pu, strftime(CreationDate,'%Y-%m') g,
   sum(coalesce(AnswerCount,0)) t FROM so.Posts WHERE OwnerUserId IS NOT NULL GROUP BY 1,2""",
 "so SUM CommentCount / mo":"""SELECT OwnerUserId pu, strftime(CreationDate,'%Y-%m') g,
   sum(coalesce(CommentCount,0)) t FROM so.Posts WHERE OwnerUserId IS NOT NULL GROUP BY 1,2""",
 "so COUNT badges / month":"""SELECT UserId pu, strftime(Date,'%Y-%m') g, count(*) t
   FROM so.Badges WHERE UserId IS NOT NULL GROUP BY 1,2""",
 "so COUNT votes / month":"""SELECT UserId pu, strftime(CreationDate,'%Y-%m') g, count(*) t
   FROM so.Votes WHERE UserId IS NOT NULL GROUP BY 1,2""",
 "so SUM Score / day":"""SELECT OwnerUserId pu, cast(cast(CreationDate as date) as varchar) g,
   sum(greatest(Score,0)) t FROM so.Posts WHERE OwnerUserId IS NOT NULL GROUP BY 1,2""",
 "cb SUM width / date":"""SELECT UserID pu, cast(EventDate as varchar) g, sum(ResolutionWidth) t
   FROM cb.hits WHERE UserID IS NOT NULL GROUP BY 1,2""",
 "cb COUNT / counterid":"""SELECT UserID pu, cast(CounterID as varchar) g, count(*) t
   FROM cb.hits WHERE UserID IS NOT NULL GROUP BY 1,2""",
 "cb SUM width / counterid":"""SELECT UserID pu, cast(CounterID as varchar) g,
   sum(ResolutionWidth) t FROM cb.hits WHERE UserID IS NOT NULL GROUP BY 1,2""",
 "cb COUNT / date, searched":"""SELECT UserID pu, cast(EventDate as varchar) g, count(*) t
   FROM cb.hits WHERE UserID IS NOT NULL AND SearchPhrase <> '' GROUP BY 1,2""",
 "cb SUM width / region mob":"""SELECT UserID pu, cast(RegionID as varchar) g,
   sum(ResolutionWidth) t FROM cb.hits WHERE UserID IS NOT NULL AND IsMobile=1 GROUP BY 1,2""",
}
print("Hunting for queries where Google DP as published BEATS ours.")
print("Hypothesis: Google wins when B (our per-PU norm bound) is large relative to U (its per-cell")
print("bound) while C_v=1 is still cheap for it -- heavy-tailed k_u AND heavy-tailed values.\n")
print(f"{'query':<28}{'groups':>8}{'k_u p50':>8}{'B/U':>7}{'google':>9}{'ours':>8}{'ratio':>8}  who")
print("-"*84)
losses=[]
for name,sql in Q.items():
    try:
        rows=con.execute(f"""WITH c AS ({sql})
          SELECT dense_rank() OVER (ORDER BY pu)-1 pid, dense_rank() OVER (ORDER BY g)-1 gid, t
          FROM c""").fetchnumpy()
    except Exception as e:
        print(f"{name:<28}  ERROR {str(e)[:40]}"); continue
    if len(rows["t"])>5_500_000: print(f"{name:<28}  skipped {len(rows['t']):,} cells"); continue
    c=F.Cells.__new__(F.Cells)
    c._init_from(rows["pid"].astype(np.int64),rows["gid"].astype(np.int64),rows["t"].astype(np.float64))
    if c.K<3: print(f"{name:<28}  skipped, {c.K} groups"); del c; continue
    mk=int(c.k_u.max()); cands=sorted(set([1,2,3,5,8,13,21,34,55,89])|{mk})
    ranks=[c.rank_random(r) for _ in range(T)]
    vt={(t,ce):F.votes(c,ranks[t],ce) for t in range(T) for ce in cands}
    ca={}
    for eb in sorted({s[0] for s in F.SPLITS}):
        for t in range(T):
            U=F.approx_bounds(c.val,EPS*eb,r); B=F.approx_bounds(c.norms[c.norms>0],EPS*eb,r)
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
                    rel=(v+r.laplace(0,ce/(EPS*ee),size=c.K)>=thr)&(v>0)
                    tot,sc=(g[cv],cv*U) if kind=="g" else (o,B)
                    es.append(c.score(np.where(rel,tot+r.laplace(0,sc/(EPS*evv),size=c.K),0.0)))
                e=float(np.mean(es)); best=e if best is None or e<best else best
        return best
    U0,B0=ca[(0.002,0)][0],ca[(0.002,0)][1]
    gg,oo=ev("g"),ev("o")
    who="GOOGLE" if gg<oo*0.98 else ("ours" if oo<gg*0.98 else "tie")
    if who=="GOOGLE": losses.append(name)
    print(f"{name:<28}{c.K:>8,}{np.quantile(c.k_u,.5):>8.0f}{B0/max(U0,1):>7.2f}"
          f"{100*gg:>8.2f}%{100*oo:>7.2f}%{gg/oo:>7.2f}x  {who}", flush=True)
    del c,ca
print(f"\nGoogle wins on {len(losses)} of the queries tested: {losses}")
