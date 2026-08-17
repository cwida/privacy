"""Does the automatic C_u selection transfer to the smooth-sensitivity (SASS) path?

The recommendation from the Laplace path is "deliberately overshoot": undershooting C_u discards
real user data and costs up to 35x, overshooting merely adds noise and costs at most 2x, so the
quantile target p is set high (0.95-0.99).

That rule depends on the SHAPE of the error curve, and SASS may not share it. In the Laplace path
C_u MULTIPLIES the value sensitivity. In SASS it does two things at once:

  - rank-caps each PU to C_u (PU, group) partials, so small C_u biases every group by 1 - C_u/k_u
  - divides the per-aggregate budget, eps_cell = eps/((c+1) * C_u), and eps_cell sets
    beta = eps_cell / (2 ln(2/delta_cell)) in the NRS smooth-sensitivity envelope

The second is the worry: as C_u grows, beta shrinks, and the envelope
S* = max_{i<=p<=j} (x_j - x_i) exp(-beta (j-i-1)) stops decaying -- it collapses onto its domain
sentinel term. So SASS may be bad at BOTH ends: bias at small C_u, envelope collapse at large C_u.
If so the curve is a pincer rather than a one-sided cliff, and "overshoot" is the wrong rule.

This measures the curve directly and reports which shape it has.

    python3 attacks/em_cu_sass.py
"""
import argparse
import numpy as np
import duckdb

import fineness_sweep as F
from sass_vote_geometry import smooth_median_release, lane_matrix, M


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--grouping", default="month|nation")
    ap.add_argument("--filter", default="c_acctbal>=8000")
    ap.add_argument("--eps", type=float, default=1.0)
    ap.add_argument("--delta", type=float, default=1e-6)
    ap.add_argument("--c-agg", type=int, default=1, help="number of aggregates c")
    ap.add_argument("--trials", type=int, default=3)
    a = ap.parse_args()

    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    c = F.Cells(con, F.GROUPINGS[a.grouping][0], a.filter)
    r = np.random.default_rng(17)
    lane_of_pu = r.integers(0, M, size=c.P)
    lam = float(np.abs(c.truth).max()) * 2.0          # public output domain
    truth = c.truth
    den = np.sum(np.abs(truth))
    mk = int(c.k_u.max())

    print(f"{a.grouping}, {c.K:,} groups, {c.P:,} PUs, max k_u={mk}, eps={a.eps}, c={a.c_agg}")
    print(f"per-aggregate cell budget eps_cell = eps/((c+1) C_u), so beta shrinks as C_u grows.\n")
    print(f"  {'C_u':>5}{'eps_cell':>10}{'beta':>10}{'rank-cap bias':>15}"
          f"{'median S*':>12}{'total err':>11}")
    print("  " + "-" * 65)
    rows = []
    for cu in (1, 2, 3, 5, 8, 13, 21, 34, 55, mk):
        eps_cell = a.eps / ((a.c_agg + 1) * cu)
        delta_cell = a.delta / (a.c_agg + 1)
        beta = eps_cell / (2.0 * np.log(2.0 / delta_cell))
        keep = c.rank_top < cu                         # rank-cap to C_u partials per PU
        # noiseless bias from the rank cap alone
        biased = np.bincount(c.gi[keep], weights=c.val[keep], minlength=c.K)
        bias = float(np.sum(np.abs(biased - truth)) / den)
        lanes = lane_matrix(c, keep, c.val, lane_of_pu)   # sorts internally
        errs, sstars = [], []
        for _ in range(a.trials):
            out, smooth, _ = smooth_median_release(lanes, lam, eps_cell, delta_cell, r,
                                                   noise=True, return_stats=True)
            errs.append(float(np.sum(np.abs(out - truth)) / den))
            sstars.append(float(np.median(smooth)))
        e = float(np.mean(errs))
        rows.append((cu, e))
        print(f"  {cu:>5}{eps_cell:>10.4f}{beta:>10.2e}{100*bias:>14.1f}%"
              f"{np.mean(sstars):>12.3e}{100*e:>10.1f}%")
    best = min(rows, key=lambda x: x[1])
    lo = [e for cu, e in rows if cu < best[0]]
    hi = [e for cu, e in rows if cu > best[0]]
    print(f"\n  best C_u = {best[0]} at {100*best[1]:.1f}% error")
    if lo and hi:
        print(f"  worst penalty BELOW optimum: {max(lo)/best[1]:.2f}x")
        print(f"  worst penalty ABOVE optimum: {max(hi)/best[1]:.2f}x")
        shape = ("ONE-SIDED (undershoot worse) -- overshoot rule transfers"
                 if max(lo) > 2 * max(hi) else
                 "PINCER (both sides bad) -- overshoot rule does NOT transfer"
                 if max(hi) > 0.5 * max(lo) else "unclear")
        print(f"  -> curve shape: {shape}")
    elif not hi:
        print("  optimum is at the top of the grid: undershooting is the only risk")
    else:
        print("  optimum is at the bottom of the grid: overshooting is the only risk")


if __name__ == "__main__":
    main()
