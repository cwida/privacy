import duckdb, numpy as np, itertools, sys
EPS, DELTA = 1.0, 1e-6
rng = np.random.default_rng(2026)

con = duckdb.connect(config={'threads': 2})
con.execute("ATTACH '/home/ila/Code/privacy/tpch_sass_sf10.db' AS tpch (READ_ONLY)")
rows = con.execute("""
 SELECT o_custkey AS pu,
        strftime(l_shipdate,'%Y-%m')||'|'||cast(c_nationkey as varchar) AS g,
        sum(l_extendedprice) AS t
 FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
 JOIN tpch.customer ON c_custkey=o_custkey
 WHERE c_acctbal>=8000 GROUP BY 1,2""").fetchall()
pu  = np.array([r[0] for r in rows], dtype=np.int64)
gs  = np.array([r[1] for r in rows])
val = np.array([r[2] for r in rows], dtype=np.float64)
gk, gi = np.unique(gs, return_inverse=True)
_,  pi = np.unique(pu, return_inverse=True)
K, P = len(gk), pi.max()+1
truth = np.bincount(gi, weights=val, minlength=K)            # UNCAPPED truth
npu_g = np.bincount(gi, weights=np.ones_like(val), minlength=K)
order = np.argsort(pi, kind="stable")
pi_s, gi_s, val_s = pi[order], gi[order], val[order]
starts = np.searchsorted(pi_s, np.arange(P+1))
k_u = np.diff(starts)
print(f"query: SUM(price) by month|nation, acctbal>=8000, TPC-H sf10", flush=True)
print(f"  groups={K:,}  PUs={P:,}  cells={len(val):,}  median PUs/group={np.median(npu_g):,.0f}"
      f"  max k_u={k_u.max()}", flush=True)

def tau(ef, cu):
    e, d = EPS*ef, DELTA*ef
    inner = 2.0 - 2.0*(1.0-d)**(1.0/cu)
    return np.inf if inner <= 0 else 1.0 - cu*np.log(inner)/e

def approx_bounds(cells, eps_b, r):
    """Google ApproxBounds over log2 bins of per-cell values."""
    b = np.clip(np.floor(np.log2(np.maximum(cells, 1.0))).astype(int), 0, 40)
    ub, cb = np.unique(b, return_counts=True)
    noisy = cb + r.laplace(0, 1.0/eps_b, size=len(ub))
    thr = 24.88/eps_b
    ok = ub[noisy >= thr]
    return 2.0**((ok.max()+1) if len(ok) else 1)

def run_google(cu, eb, eeta, ev, r):
    U = approx_bounds(val, eb, r)
    keep = np.ones(len(val), bool)
    big = np.where(k_u > cu)[0]
    for u in big:                                  # random truncation to C_u groups
        s, e = starts[u], starts[u+1]
        idx = np.arange(s, e); r.shuffle(idx)
        keep[idx[cu:]] = False
    v = np.minimum(val[keep], U)
    tot = np.bincount(gi[keep], weights=v, minlength=K)
    noised_cnt = npu_g + r.laplace(0, cu/eeta, size=K)
    rel = noised_cnt >= tau(eeta, cu)
    out = np.where(rel, tot + r.laplace(0, cu*U/ev, size=K), 0.0)
    return out, rel

def truncated_votes(cu, r):
    """Per-group DISTINCT-PU counts with each PU voting in at most C_u groups.
    Required for the vote histogram to have L-inf sensitivity C_u."""
    if k_u.max() <= cu:
        return npu_g.copy()
    keep = np.ones(len(val), bool)
    for u in np.where(k_u > cu)[0]:
        idx = np.arange(starts[u], starts[u+1]); r.shuffle(idx)
        keep[idx[cu:]] = False
    return np.bincount(gi[keep], weights=np.ones(keep.sum()), minlength=K)

def run_ours(cu, eb, eeta, ev, r):
    """l1 clip on per-PU norms for the VALUES; C_u bounds only the tau VOTES."""
    norms = np.bincount(pi, weights=np.abs(val), minlength=P)
    Bc = approx_bounds(norms[norms > 0], eb, r)
    n_u = np.bincount(pi, weights=np.abs(np.clip(val, -Bc, Bc)), minlength=P)   # FIXED norm
    scale = np.minimum(1.0, Bc/np.maximum(n_u, 1e-30))
    v = np.clip(val, -Bc, Bc) * scale[pi]
    tot = np.bincount(gi, weights=v, minlength=K)
    votes = truncated_votes(cu, r)                      # <-- votes truncated to C_u
    noised_cnt = votes + r.laplace(0, cu/eeta, size=K)
    rel = noised_cnt >= tau(eeta, cu)
    out = np.where(rel, tot + r.laplace(0, Bc/ev, size=K), 0.0)
    return out, rel

def score(out):
    """Relative L1 over the TRUE key set. Suppressed group = released 0 = full error."""
    return float(np.sum(np.abs(out - truth)) / np.sum(np.abs(truth)))

CUS   = [1, 2, 5, 10, 19, 30, 50, 72]
SPLITS= [(1/3,1/3,1/3), (.05,1/3,.6167), (.05,.25,.70), (.05,.15,.80),
         (.05,.10,.85), (.02,.20,.78), (.002,.30,.698), (.05,.05,.90)]
TR = 6
best = {}
for name, fn in (("Google", run_google), ("ours", run_ours)):
    rec = []
    for cu, (eb, ee, ev) in itertools.product(CUS, SPLITS):
        r = np.random.default_rng(99)
        es, rels = [], []
        for _ in range(TR):
            o, rl = fn(cu, EPS*eb, EPS*ee, EPS*ev, r)
            es.append(score(o)); rels.append(rl.sum())
        rec.append((np.mean(es), cu, (eb, ee, ev), np.mean(rels)))
    rec.sort()
    best[name] = rec[0]
    print(f"\n{name}: best of {len(rec)} configs", flush=True)
    for e, cu, sp, nr in rec[:4]:
        print(f"   err={100*e:7.3f}%  C_u={cu:<3} split=({sp[0]:.3f},{sp[1]:.3f},{sp[2]:.3f})"
              f"  released={nr:,.0f}/{K:,}", flush=True)

# decompose Google's error at its optimum: truncation bias vs Laplace noise
_, cu, (eb, ee, ev), _ = best["Google"]
r = np.random.default_rng(5)
U = approx_bounds(val, EPS*eb, r)
keep = np.ones(len(val), bool)
for u in np.where(k_u > cu)[0]:
    idx = np.arange(starts[u], starts[u+1]); r.shuffle(idx); keep[idx[cu:]] = False
tot_noiseless = np.bincount(gi[keep], weights=np.minimum(val[keep], U), minlength=K)
bias = np.sum(np.abs(tot_noiseless - truth))/np.sum(np.abs(truth))
print(f"\nGoogle at its optimum (C_u={cu}, U={U:,.0f}): noiseless error = {100*bias:.2f}% "
      f"(truncation+cell-clip bias)  ->  the other {100*(best['Google'][0]-bias):.2f}pp is noise")
print(f"   C_u*U = {cu*U:,.0f}   vs   our B  = see below")
norms = np.bincount(pi, weights=np.abs(val), minlength=P)
r2 = np.random.default_rng(5); Bc = approx_bounds(norms[norms>0], EPS*0.002, r2)
print(f"   our B = {Bc:,.0f}   sensitivity ratio C_u*U/B = {cu*U/Bc:.2f}x"
      f"   (true max per-PU norm = {norms.max():,.0f})")

eg, eo = best["Google"][0], best["ours"][0]
print(f"\nBOTH FULLY TUNED, tau-binding query, scored on the true key set:")
print(f"   Google {100*eg:.3f}%   ours {100*eo:.3f}%   gap {eg/eo:.2f}x")
