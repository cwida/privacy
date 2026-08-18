import numpy as np, duckdb, sys
sys.path.insert(0,'/home/ila/Code/privacy/attacks')
import fineness_sweep as F
from all_or_frozen_v2 import tau_af, tau_count, load, QUERIES, DELTA, B_BINS
con=duckdb.connect(config={'threads':2}); con.execute("SET enable_progress_bar=false")
con.execute("ATTACH '/home/ila/Code/privacy/tpch_sass_sf10.db' AS tpch (READ_ONLY)")
r=np.random.default_rng(7); E=0.4; T=200
def hist_trunc(c,ce,r):
    """Her per-group histogram with each PU truncated to ce groups -- her Q4 applied to HER design."""
    rank=c.rank_random(r); keep=rank<ce
    b=np.clip(np.floor(np.log2(np.maximum(np.abs(c.val[keep]),1.0))).astype(int),0,B_BINS-1)
    H=np.zeros((c.K,B_BINS)); np.add.at(H,(c.gi[keep],b),1.0)
    return H.max(axis=1)
print("CHECK 4: HER mechanism at C_u = 1, the value my tau-floor proof says is optimal.")
print("         Truncation applied to the histogram too, per her Q4.\n")
print(f"  {'query':<20}{'C_u':>5}{'tau_AF':>9}{'max-bin min':>13}{'median':>10}"
      f"{'per-grp':>9}{'AND':>7}")
print("  "+"-"*73)
for name,(sql,filt) in QUERIES.items():
    c,lab,_=load(con, sql.format(W=filt))
    for cu in (1,5,37):
        b=cu/E; t=tau_af(b,DELTA)
        mb=hist_trunc(c,cu,r)
        pg=[];al=0
        for _ in range(T):
            p=mb+r.laplace(0,b,size=c.K)>=t
            pg.append(p.mean()); al+=int(p.all())
        print(f"  {name if cu==1 else '':<20}{cu:>5}{t:>9.1f}{mb.min():>13,.0f}"
              f"{np.median(mb):>10,.0f}{100*np.mean(pg):>8.1f}%{100*al/T:>6.1f}%", flush=True)
    del c
print("\n  -> at C_u=1 the threshold collapses to ~43, and her statistic clears it on the coarse")
print("     groupings. The AND is still the binding constraint.")
