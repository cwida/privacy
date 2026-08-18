import sys, pickle
sys.path.insert(0, '/tmp/claude-1000/-home-ila-Code-privacy/123095d1-595b-46af-a45b-7c4fde73199b/scratchpad')
import numpy as np
import mech
from geom import Segments, verify

SP = '/tmp/claude-1000/-home-ila-Code-privacy/123095d1-595b-46af-a45b-7c4fde73199b/scratchpad/'
SQL = """
SELECT o_custkey AS u, strftime(o_orderdate,'%Y-%m') AS g, SUM(l_extendedprice) AS v
FROM d.lineitem JOIN d.orders ON o_orderkey=l_orderkey
WHERE l_shipmode IN ('AIR','REG AIR')
GROUP BY 1,2
"""
u, g, v, n_pu, n_g, labels = mech.load_cells(SQL)
seg = Segments(u, g, v, n_pu, n_g)
cells = mech.Cells(u, g, v, n_pu, n_g, labels)
print("cells=%d  PUs=%d  groups=%d  total_mass=%.6e" % (len(v), n_pu, n_g, seg.total_mass))
print("k_u  (groups per PU): med=%.0f p90=%.0f p99=%.0f max=%.0f"
      % tuple(np.percentile(seg.counts, [50, 90, 99, 100])))
print("N_u  (per-PU l1 norm): med=%.3e p90=%.3e p99=%.3e max=%.3e"
      % tuple(np.percentile(seg.N_u, [50, 90, 99, 100])))
print("cell v: med=%.3e p99=%.3e max=%.3e" % tuple(np.percentile(v, [50, 99, 100])))
print("group totals: min=%.4e med=%.4e max=%.4e" % (seg.truth.min(), np.median(seg.truth), seg.truth.max()))

JS = list(range(12, 27))
LAD = [2.0 ** j for j in JS]
U_AB = 2.0 ** 20   # representative ApproxBounds U on cells, for the hybrid

GEOMS = {
    'l1_pure':   lambda B: seg.g_l1_pure(B),
    'l1_capB':   lambda B: seg.g_l1_capB(B),
    'waterfill': lambda B: seg.g_waterfill(B),
    'greedy':    lambda B: seg.g_greedy(B, True),
    'greedy_strict': lambda B: seg.g_greedy(B, False),
    'hybrid_U':  lambda B: seg.g_hybrid(B, U_AB),
    'soft':      lambda B: seg.g_soft(B),
    'oracle_big': lambda B: seg.g_oracle_bigfirst(B, seg.truth),
}

print("\n=== L1 CONSTRAINT VERIFICATION (||x_u||_1 <= B for every one of %d PUs) ===" % n_pu)
print("%-15s %-8s %-6s %-12s %-10s %-8s %-8s" %
      ("geometry", "B=2^j", "ok", "violations", "max/B", "neg", "x>t"))
sums = {}
massf = {}
allok = True
for name, fn in GEOMS.items():
    sums[name] = np.zeros((len(JS), n_g))
    massf[name] = np.zeros(len(JS))
    for i, (j, B) in enumerate(zip(JS, LAD)):
        x = fn(B)
        r = verify(seg, x, B, name)
        sums[name][i] = np.bincount(g, weights=x, minlength=n_g)
        massf[name][i] = r['mass_frac']
        allok &= r['ok']
        if j in (16, 20, 24):
            print("%-15s %-8s %-6s %-12d %-10.6f %-8d %-8d" %
                  (name, "2^%d" % j, "PASS" if r['ok'] else "FAIL",
                   r['n_violations'], r['ratio'], r['n_negative'], r['n_exceeds_t']))
print("ALL GEOMETRIES, ALL B:", "PASS" if allok else "FAIL")

print("\n=== FRACTION OF TOTAL MASS PRESERVED (sum_g x_g / sum_g t_g) ===")
print("%-8s " % "B" + "".join("%-12s" % n for n in GEOMS))
for i, j in enumerate(JS):
    print("2^%-6d " % j + "".join("%-12.6f" % massf[n][i] for n in GEOMS))

pickle.dump(dict(JS=JS, LAD=LAD, sums=sums, massf=massf, truth=seg.truth,
                 n_g=n_g, n_pu=n_pu, U_AB=U_AB,
                 med_gt=float(np.median(seg.truth)), total_mass=seg.total_mass),
            open(SP + 'geom_pre.pkl', 'wb'))
pickle.dump(dict(u=u, g=g, v=v, n_pu=n_pu, n_g=n_g), open(SP + 'geom_cells.pkl', 'wb'))
print("\nsaved geom_pre.pkl")
