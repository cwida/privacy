"""Spend part of the value budget on MARGINALS and reconcile — a constraint the grid cannot see.

Composite group keys (month|nation, date|region) are a 2-D grid, and every headline in this project
is worst on exactly those. A flat release adds Laplace(B/eps_v) to all K cells independently and
throws away the fact that the row and column sums are the SAME data seen another way.

Release three things instead -- the grid, the row marginal, the column marginal -- then solve for
the consistent table closest to all three. The marginals are far more accurate per number (a month
marginal aggregates ~25 cells' worth of signal against one cell's worth of noise), so they pin down
structure the grid alone cannot resolve. Reconciliation is post-processing: it costs nothing beyond
the budget split.

Sensitivity: a PU's l1 contribution is <= B to the grid, <= B to the row marginal, and <= B to the
column marginal, so the three are sequential composition and eps_v splits three ways. That split is
the price; the question is whether reconciliation repays it.

Closed-form 2-D least squares (Hay et al. style) for an m x n grid:
    Y = X + (r - X.rowsum)/n  +  (c - X.colsum)/m  -  (total discrepancy)/(m*n)

    python3 attacks/marginal_reconcile.py [--grouping month|nation]
"""

import argparse
import itertools

import numpy as np

from fineness_sweep import DELTA, SPLITS, Cells, approx_bounds, tau, votes

# groupings expressed as a 2-D grid: (row expr, col expr)
GRIDS = {
    "month|nation": ("strftime(l_shipdate,'%Y-%m')", "cast(c_nationkey as varchar)"),
    "month|priority": ("strftime(l_shipdate,'%Y-%m')", "o_orderpriority"),
    "day|region": ("cast(l_shipdate as varchar)", "cast(n_regionkey as varchar)"),
}


class Grid(Cells):
    """Cells plus the row/column index of every group, so marginals can be formed."""

    def __init__(self, con, rexpr, cexpr, filt):
        rows = con.execute(f"""
            WITH c AS (
              SELECT o_custkey AS pu, {rexpr} AS rk, {cexpr} AS ck, sum(l_extendedprice) AS t
              FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
              JOIN tpch.customer ON c_custkey=o_custkey
              JOIN tpch.nation ON n_nationkey=c_nationkey
              WHERE {filt} GROUP BY 1,2,3)
            SELECT dense_rank() OVER (ORDER BY pu)-1 AS pid,
                   dense_rank() OVER (ORDER BY rk)-1 AS ri,
                   dense_rank() OVER (ORDER BY ck)-1 AS ci, t FROM c""").fetchnumpy()
        ri, ci = rows["ri"].astype(np.int64), rows["ci"].astype(np.int64)
        self.m, self.n = int(ri.max()) + 1, int(ci.max()) + 1
        self._init_from(rows["pid"].astype(np.int64), ri * self.n + ci,
                        rows["t"].astype(np.float64))
        self.K = self.m * self.n                       # full grid, including empty cells
        self.truth = np.bincount(self.gi, weights=self.val, minlength=self.K)
        self.npu_g = np.bincount(self.gi, minlength=self.K).astype(float)


def reconcile(x, r, c, m, n):
    """Closest consistent table to a noisy grid x with noisy row sums r and column sums c."""
    X = x.reshape(m, n)
    dr = (r - X.sum(1)) / n
    dc = (c - X.sum(0)) / m
    tot = (r.sum() + c.sum()) / 2.0
    return (X + dr[:, None] + dc[None, :] - (r.sum() + c.sum() - 2 * tot) / (2 * m * n)).ravel()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--filter", default="c_acctbal>=8000")
    ap.add_argument("--grouping", default="month|nation")
    ap.add_argument("--eps", type=float, default=1.0)
    ap.add_argument("--trials", type=int, default=4)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    rexpr, cexpr = GRIDS[a.grouping]
    c = Grid(con, rexpr, cexpr, a.filter)
    EPS = a.eps
    mk = int(c.k_u.max())
    cus = sorted({1, 2, 3, 5, 8, 13, 21, 34, 55, mk})
    r = np.random.default_rng(99)
    ranks = [c.rank_random(r) for _ in range(a.trials)]
    vt = {(t, ce): votes(c, ranks[t], ce) for t in range(a.trials) for ce in cus}
    print(f"{a.grouping}: {c.m} x {c.n} grid = {c.K:,} cells "
          f"({int((c.truth != 0).sum()):,} non-empty), {c.P:,} PUs; eps={EPS}\n")

    row_t = c.truth.reshape(c.m, c.n).sum(1)
    col_t = c.truth.reshape(c.m, c.n).sum(0)
    cache = {}
    for eb in sorted({s[0] for s in SPLITS}):
        for t in range(a.trials):
            B = approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
            cl = np.clip(c.val, -B, B)
            n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
            v = cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi]
            tot = np.bincount(c.gi, weights=v, minlength=c.K)
            cache[(eb, t)] = (B, tot, tot.reshape(c.m, c.n).sum(1), tot.reshape(c.m, c.n).sum(0))

    # marginal budget share: 0 = flat release (all budget on the grid)
    MSHARE = [0.0, 0.1, 0.2, 0.3, 0.5]

    def run(ce, split, ms, use_rec):
        eb, ee, ev = split
        thr = tau(EPS * ee, DELTA, ce)
        es = []
        for t in range(a.trials):
            rel = (vt[(t, ce)] + r.laplace(0, ce / (EPS * ee), size=c.K) >= thr) & (vt[(t, ce)] > 0)
            B, tot, rt, ct = cache[(eb, t)]
            e_grid = EPS * ev * (1.0 - ms)
            x = tot + r.laplace(0, B / e_grid, size=c.K)
            if use_rec and ms > 0:
                e_marg = EPS * ev * ms / 2.0        # two marginals, sequential
                rn = rt + r.laplace(0, B / e_marg, size=c.m)
                cn = ct + r.laplace(0, B / e_marg, size=c.n)
                x = reconcile(x, rn, cn, c.m, c.n)
            es.append(c.score(np.where(rel, x, 0.0)))
        return float(np.mean(es))

    flat = min((run(ce, s, 0.0, False), ce, s) for ce, s in itertools.product(cus, SPLITS))
    print(f"  {'flat release':<28}{100*flat[0]:>8.3f}%   C_e={flat[1]}")
    best = None
    for ms in MSHARE[1:]:
        v = min((run(ce, s, ms, True), ce, s, ms) for ce, s in itertools.product(cus, SPLITS))
        print(f"  {'+ marginals, share ' + f'{ms:.0%}':<28}{100*v[0]:>8.3f}%   C_e={v[1]}")
        if best is None or v[0] < best[0]:
            best = v
    print(f"\n  best reconciled vs flat : {flat[0]/best[0]:.3f}x  (marginal share {best[3]:.0%})")
    print(f"  grid occupancy          : {100*(c.truth != 0).mean():.1f}% of cells non-empty")
    print("  (reconciliation pays when the grid is sparse or the marginals are much sharper")
    print("   per number than the cells; it costs a 3-way budget split to buy that.)")


if __name__ == "__main__":
    main()
