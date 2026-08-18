import numpy as np, duckdb, sys
sys.path.insert(0,'/home/ila/Code/privacy/attacks')
import fineness_sweep as F
from all_or_frozen_v2 import tau_af, tau_count, group_hist, load, truncated_counts, QUERIES, DELTA
con=duckdb.connect(config={'threads':2}); con.execute("SET enable_progress_bar=false")
con.execute("ATTACH '/home/ila/Code/privacy/tpch_sass_sf10.db' AS tpch (READ_ONLY)")
r=np.random.default_rng(7); CU=37; E=0.4; T=200
print("CHECK 3: at MATCHED C_u=37, eps=0.4, where does All-or-Frozen actually lose?\n")
print("  Both arms: noise C_u/eps = %.1f. Her tau_AF = %.0f, Google tau = %.0f\n"
      % (CU/E, tau_af(CU/E,DELTA), tau_count(E,DELTA,CU)))
print(f"  {'query':<20}{'stat':<12}{'min':>9}{'median':>10}{'per-grp pass':>14}{'AND':>8}")
print("  "+"-"*74)
for name,(sql,filt) in QUERIES.items():
    c,lab,_=load(con, sql.format(W=filt))
    H=group_hist(c); maxbin=H.max(axis=1)
    b=CU/E; t_af=tau_af(b,DELTA); t_gl=tau_count(E,DELTA,CU)
    cnt=truncated_counts(c,CU,r)                       # Google's statistic at C_u=37
    rows=[("her max-bin",maxbin,t_af,b),("Google count",cnt,t_gl,b)]
    for lbl,stat,thr,sc in rows:
        pg=[];al=0
        for _ in range(T):
            p=stat+r.laplace(0,sc,size=c.K)>=thr
            pg.append(p.mean()); al+=int(p.all())
        print(f"  {name if lbl.startswith('her') else '':<20}{lbl:<12}{stat.min():>9,.0f}"
              f"{np.median(stat):>10,.0f}{100*np.mean(pg):>13.1f}%{100*al/T:>7.1f}%", flush=True)
    del c,H
print("\n  'per-grp pass' is what Google's per-group release delivers; 'AND' is what All-or-Frozen")
print("  needs. The gap between those two columns IS the cost of the conjunction.")
