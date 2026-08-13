"""Is our bound B paying a power-of-two rounding penalty that Google does not?

Google's effective value bound is C_v * U: an arbitrary integer times a power of two, i.e. a
fine grid. Ours is B = 2^j straight out of ApproxBounds -- a factor-2 grid, so on average we
overshoot the bound we actually want by ~1.4x and pay that in noise.

Finer log bins fix it, and they are nearly free. ApproxBounds' per-bin threshold is
    thr = -ln(2 (1 - P^(1/(2n)))) / eps_b,   P = 1 - 1e-9
which grows only logarithmically in the bin count n: 24.88 at n=64, 26.26 at n=256. Quartering
the bin width costs 6% on the threshold and buys a 4x finer bound.

    python3 attacks/bound_grid.py
"""

import argparse
import itertools

import numpy as np

from fineness_sweep import DELTA, EPS, GROUPINGS, SPLITS, Cells, tau, votes

P_SUCCESS = 1.0 - 1e-9


def ab_threshold(n_bins, eps_b):
    return -np.log(2.0 * (1.0 - P_SUCCESS ** (1.0 / (2.0 * n_bins)))) / eps_b


def approx_bounds_base(vals, eps_b, r, base, hi=2.0 ** 45):
    """ApproxBounds over log-`base` bins. Returns the upper edge of the highest surviving bin."""
    n_bins = int(np.ceil(np.log(hi) / np.log(base)))
    b = np.clip(np.floor(np.log(np.maximum(vals, 1.0)) / np.log(base)).astype(int), 0, n_bins - 1)
    ub, cb = np.unique(b, return_counts=True)
    noisy = cb + r.laplace(0, 1.0 / eps_b, size=len(ub))
    ok = ub[noisy >= ab_threshold(n_bins, eps_b)]
    return float(base ** ((ok.max() + 1) if len(ok) else 1))


BASES = {"2 (current)": 2.0, "sqrt(2)": 2.0 ** 0.5, "2^(1/4)": 2.0 ** 0.25,
         "2^(1/8)": 2.0 ** 0.125}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--filter", default="c_acctbal>=8000")
    ap.add_argument("--groupings", default="month,month|nation,day")
    ap.add_argument("--trials", type=int, default=4)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    print(f"SUM(l_extendedprice), PU=customer, WHERE {a.filter}, eps={EPS}, {a.trials} trials")
    print("l1 clip throughout; only the grid the bound B is chosen from changes.")
    print(f"ApproxBounds threshold: n=64 -> {ab_threshold(64,1.0):.2f}/eps_b, "
          f"n=512 -> {ab_threshold(512,1.0):.2f}/eps_b  (log growth, so fine bins are cheap)\n")
    for name in a.groupings.split(","):
        c = Cells(con, GROUPINGS[name][0], a.filter)
        mk = int(c.k_u.max())
        cus = sorted({1, 2, 5, 10, 19, mk})
        r = np.random.default_rng(99)
        ranks = [c.rank_random(r) for _ in range(a.trials)]
        vt = {(t, ce): votes(c, ranks[t], ce) for t in range(a.trials) for ce in cus}
        ebs = sorted({s[0] for s in SPLITS})
        print(f"  {name}: {c.K:,} groups, true max per-PU norm {c.norms.max():,.0f}")
        print(f"    {'bin base':<14}{'error':>9}{'median B':>14}{'C_e':>5}  best split")
        out = {}
        for label, base in BASES.items():
            Bs, tot = {}, {}
            for eb in ebs:
                for t in range(a.trials):
                    B = approx_bounds_base(c.norms[c.norms > 0], EPS * eb, r, base)
                    Bs[(eb, t)] = B
                    cl = np.clip(c.val, -B, B)
                    n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
                    tot[(eb, t)] = np.bincount(
                        c.gi, weights=cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi],
                        minlength=c.K)
            best = None
            for ce, (eb, ee, ev) in itertools.product(cus, SPLITS):
                thr = tau(EPS * ee, DELTA * ee, ce)
                es = []
                for t in range(a.trials):
                    rel = vt[(t, ce)] + r.laplace(0, ce / (EPS * ee), size=c.K) >= thr
                    y = np.where(rel, tot[(eb, t)]
                                 + r.laplace(0, Bs[(eb, t)] / (EPS * ev), size=c.K), 0.0)
                    es.append(c.score(y))
                e = float(np.mean(es))
                if best is None or e < best[0]:
                    best = (e, ce, (eb, ee, ev),
                            float(np.median([Bs[(eb, t)] for t in range(a.trials)])))
            out[label] = best
            print(f"    {label:<14}{100*best[0]:>8.3f}%{best[3]:>14,.0f}{best[1]:>5}"
                  f"  ({best[2][0]:.3f},{best[2][1]:.3f},{best[2][2]:.3f})", flush=True)
        base2 = out["2 (current)"][0]
        bst = min(out.items(), key=lambda kv: kv[1][0])
        print(f"    -> {bst[0]} is best: {base2/bst[1][0]:.3f}x vs the power-of-two grid\n")
        del c


if __name__ == "__main__":
    main()
