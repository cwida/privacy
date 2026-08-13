"""Dandan's proposal: use the filterless U to shrink the RANGE, then spend the saved bins on
resolution — keeping <= 64 bins and the same eps_b.

This is not the experiment in attacks/bound_grid.py. That one made the base finer over a FIXED
range [1, 2^45], so base 2^(1/8) needed 360 bins and paid a higher ApproxBounds threshold
(-ln(2(1-P^(1/2n)))/eps_b grows in n). Dandan's version keeps the bin COUNT at 64 and buys
resolution by shrinking the range using a bound already known from the filterless query:

    base = (U_hi / U_lo)^(1/64)

so with a range of 2^32 the base is sqrt(2) (width 1.41x instead of 2x), and with 2^16 it is
2^0.25 (width 1.19x). Narrower bins mean the returned bound overshoots the true maximum by less.

The question this answers: at the operating point the tuner actually chooses, is there any
overshoot left to remove?

    python3 attacks/adaptive_base.py
"""

import argparse
import itertools

import numpy as np

from fineness_sweep import DELTA, GROUPINGS, SPLITS, Cells, tau, votes

P_SUCCESS = 1.0 - 1e-9


def ab_threshold(n_bins, eps_b):
    return -np.log(2.0 * (1.0 - P_SUCCESS ** (1.0 / (2.0 * n_bins)))) / eps_b


def approx_bounds_range(vals, eps_b, r, lo, hi, n_bins=64):
    """ApproxBounds over n_bins geometric bins spanning [lo, hi]. Returns the upper edge of the
    highest surviving bin -- so the returned bound overshoots the true max by at most the bin
    width (hi/lo)^(1/n_bins)."""
    ratio = np.log(hi / lo) / n_bins
    b = np.clip(np.floor(np.log(np.maximum(vals, lo) / lo) / ratio).astype(int), 0, n_bins - 1)
    ub, cb = np.unique(b, return_counts=True)
    noisy = cb + r.laplace(0, 1.0 / eps_b, size=len(ub))
    ok = ub[noisy >= ab_threshold(n_bins, eps_b)]
    return float(lo * np.exp(ratio * ((ok.max() + 1) if len(ok) else 1)))


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
    c = Cells(con, GROUPINGS[a.grouping][0], a.filter)
    EPS = a.eps
    norms = c.norms[c.norms > 0]
    true_max = float(norms.max())
    mk = int(c.k_u.max())
    cus = sorted({1, 2, 3, 5, 8, 13, 21, 34, 55, mk})
    r = np.random.default_rng(99)
    ranks = [c.rank_random(r) for _ in range(a.trials)]
    vt = {(t, ce): votes(c, ranks[t], ce) for t in range(a.trials) for ce in cus}

    # the filterless bound: max per-PU norm with NO filter, which is what frozen metadata stores
    u_filterless = float(con.execute(f"""
        SELECT max(n) FROM (SELECT o_custkey, sum(l_extendedprice) AS n
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey GROUP BY 1)""").fetchone()[0])
    print(f"{a.grouping}: {c.K:,} groups, {c.P:,} PUs, eps={EPS}")
    print(f"  filterless max per-PU norm U = {u_filterless:,.0f}   (frozen metadata)")
    print(f"  filtered   max per-PU norm   = {true_max:,.0f}   ({u_filterless/true_max:.1f}x "
          f"smaller under this filter)\n")

    # first: how much error-optimal headroom is there at all?
    def err_at(B):
        cl = np.clip(c.val, -B, B)
        n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
        tot = np.bincount(c.gi, weights=cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi],
                          minlength=c.K)
        bias = np.sum(np.abs(tot - c.truth)) / np.sum(np.abs(c.truth))
        noise = c.K * (np.sqrt(2) * B / (EPS * 0.6)) / np.sum(np.abs(c.truth))
        return bias + noise, bias, noise
    grid = true_max * 2.0 ** np.linspace(-8, 1, 200)
    curve = [err_at(B)[0] for B in grid]
    B_star = float(grid[int(np.argmin(curve))])
    print(f"  error-optimal B* = {B_star:,.0f}  ({B_star/true_max:.3f} x the true max)")
    print(f"  -> the optimum is BELOW the maximum, so the mechanism is not over-bounding;")
    print(f"     it is deliberately under-bounding. Bin width cannot fix an intentional gap.\n")

    # (lo, hi, n_bins) -- n_bins is fixed at 64 for every range-restricted scheme, which is the
    # whole point of the proposal: same bin budget, narrower range, finer resolution.
    SCHEMES = {"base 2 (current)": (1.0, 2.0 ** 45, 45),
               "range 2^32, 64 bins": (u_filterless / 2 ** 32, u_filterless, 64),
               "range 2^16, 64 bins": (u_filterless / 2 ** 16, u_filterless, 64),
               "range 2^8,  64 bins": (u_filterless / 2 ** 8, u_filterless, 64),
               "range 2^4,  64 bins": (u_filterless / 2 ** 4, u_filterless, 64)}
    print(f"  {'scheme':<28}{'bin width':>11}{'thr x eps_b':>13}{'median B':>14}"
          f"{'B/B*':>8}{'error':>9}")
    print("  " + "-" * 83)
    for name, (lo, hi, nb) in SCHEMES.items():
        width = (hi / lo) ** (1.0 / nb)
        Bs, tot = {}, {}
        for eb in sorted({s[0] for s in SPLITS}):
            for t in range(a.trials):
                B = approx_bounds_range(norms, EPS * eb, r, lo, hi, nb)
                Bs[(eb, t)] = B
                cl = np.clip(c.val, -B, B)
                n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
                tot[(eb, t)] = np.bincount(
                    c.gi, weights=cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi],
                    minlength=c.K)
        best = None
        for ce, (eb, ee, ev) in itertools.product(cus, SPLITS):
            thr = tau(EPS * ee, DELTA, ce)
            es = []
            for t in range(a.trials):
                v = vt[(t, ce)]
                rel = (v + r.laplace(0, ce / (EPS * ee), size=c.K) >= thr) & (v > 0)
                es.append(c.score(np.where(rel, tot[(eb, t)]
                                           + r.laplace(0, Bs[(eb, t)] / (EPS * ev), size=c.K), 0.0)))
            e = float(np.mean(es))
            if best is None or e < best[0]:
                best = (e, float(np.median([Bs[(eb, t)] for t in range(a.trials)])))
        print(f"  {name:<28}{width:>11.3f}{ab_threshold(nb,1.0):>13.2f}{best[1]:>14,.0f}"
              f"{best[1]/B_star:>8.2f}{100*best[0]:>8.2f}%")
    print("\n  Finer bins change WHICH power the bound snaps to, not whether the mechanism wants")
    print("  a bound near the maximum. Where the optimum sits well below the max, a tighter grid")
    print("  around the max has nothing to give.")


if __name__ == "__main__":
    main()
