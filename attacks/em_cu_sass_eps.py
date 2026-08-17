import sys; sys.path.insert(0,'/home/ila/Code/privacy/attacks')
import numpy as np, duckdb
import fineness_sweep as F
from sass_vote_geometry import smooth_median_release, lane_matrix, M
con=duckdb.connect(config={'threads':2}); con.execute("SET enable_progress_bar=false")
con.execute("ATTACH '/home/ila/Code/privacy/tpch_sass_sf10.db' AS tpch (READ_ONLY)")
c=F.Cells(con, F.GROUPINGS['month|nation'][0], 'c_acctbal>=8000')
r=np.random.default_rng(17); lane=r.integers(0,M,size=c.P)
lam=float(np.abs(c.truth).max())*2.0; den=np.sum(np.abs(c.truth)); mk=int(c.k_u.max())
DELTA=1e-6
print("Is the monotone shape an artifact of eps=1? beta = eps_cell/(2 ln(2/delta_cell)) and")
print("S* only decays once beta*(j-i-1) is order 1, i.e. beta >~ 1/64. Sweeping eps:\n")
print(f"{'eps':>6}{'C_u':>5}{'eps_cell':>10}{'beta':>10}{'beta*64':>9}{'bias':>8}{'error':>11}")
print("-"*60)
for EPS in (1.0, 8.0, 64.0):
    best=None
    for cu in (1,5,21,mk):
        ec=EPS/(2*cu); dc=DELTA/2; beta=ec/(2*np.log(2/dc))
        keep=c.rank_top<cu
        biased=np.bincount(c.gi[keep],weights=c.val[keep],minlength=c.K)
        bias=float(np.sum(np.abs(biased-c.truth))/den)
        lanes=lane_matrix(c,keep,c.val,lane)
        es=[]
        for _ in range(2):
            out,sm,_=smooth_median_release(lanes,lam,ec,dc,r,noise=True,return_stats=True)
            es.append(float(np.sum(np.abs(out-c.truth))/den))
        e=float(np.mean(es))
        if best is None or e<best[1]: best=(cu,e)
        print(f"{EPS:>6.0f}{cu:>5}{ec:>10.4f}{beta:>10.2e}{beta*64:>9.2f}{100*bias:>7.1f}%{100*e:>10.1f}%")
    print(f"       -> best C_u = {best[0]} at {100*best[1]:.1f}%\n")
print("beta*64 << 1 means the NRS envelope never decays across the 64 lanes, so S* saturates at")
print("the domain sentinel and noise is set by Lambda rather than by the data. That is the pincer's")
print("upper jaw, and it only opens when eps_cell is large -- i.e. small C_u or very large eps.")
