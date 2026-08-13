"""The same l1 trick applied to the partition-selection histogram, not just the values.

Wilson-style partition selection truncates each PU to C_u groups at random, then releases groups
whose noised distinct-PU count clears tau. At C_u=1 a PU that touches k_u groups votes in exactly
one of them, so a group with n PUs receives Binomial(n, 1/k_u) votes -- the right mean, but with
sampling variance on top of the Laplace noise.

Give the PU a FRACTIONAL vote instead, 1/k_u in each of its groups. The histogram still changes by
at most 1 in l1 when a PU is added or removed, so the sensitivity -- and therefore tau -- is
unchanged. But the count is now exact instead of binomial, which should push more groups over tau
for free.

  truncate    each PU votes 1 in min(C_u, k_u) randomly chosen groups     l1 sens = C_u
  top         ... in its C_u largest-value groups                         l1 sens = C_u
  fractional  each PU votes C_u/k_u in every group it touches             l1 sens = C_u

    python3 attacks/vote_geometry.py
"""

import argparse
import itertools

import numpy as np

from fineness_sweep import (DELTA, EPS, GROUPINGS, SPLITS, Cells, approx_bounds, tau)


def vote_hist(c, kind, cu, rank):
    if kind == "fractional":
        w = np.minimum(cu / c.k_u[c.pi], 1.0)
        return np.bincount(c.gi, weights=w, minlength=c.K)
    keep = rank < cu
    return np.bincount(c.gi[keep], minlength=c.K).astype(float)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--filter", default="c_acctbal>=8000")
    ap.add_argument("--groupings", default="month|nation,week|nation,day|region")
    ap.add_argument("--trials", type=int, default=3)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    print(f"SUM(l_extendedprice), PU=customer, WHERE {a.filter}, eps={EPS}, {a.trials} trials")
    print("value channel is the l1 clip in all rows; only the VOTE histogram changes.")
    print("all three have l1 vote sensitivity C_u, so tau is identical -- differences are"
          " sampling variance only\n")
    for name in a.groupings.split(","):
        c = Cells(con, GROUPINGS[name][0], a.filter)
        mk = int(c.k_u.max())
        cus = sorted({1, 2, 5, 10, 19, mk})
        r = np.random.default_rng(99)
        ranks = [c.rank_random(r) for _ in range(a.trials)]
        ebs = sorted({s[0] for s in SPLITS})
        Bs = {(eb, t): approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
              for eb in ebs for t in range(a.trials)}
        tot = {}
        for (eb, t), B in Bs.items():
            cl = np.clip(c.val, -B, B)
            n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
            tot[(eb, t)] = np.bincount(
                c.gi, weights=cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi],
                minlength=c.K)
        print(f"  {name}: {c.K:,} groups, max k_u={mk}, median PUs/group "
              f"{np.median(c.npu_g):,.0f}")
        print(f"    {'votes':<12}{'error':>9}{'released':>11}{'C_u':>5}  best split")
        res = {}
        for kind in ("truncate", "top", "fractional"):
            best = None
            for cu, (eb, ee, ev) in itertools.product(cus, SPLITS):
                thr = tau(EPS * ee, DELTA * ee, cu)
                es, rl = [], []
                for t in range(a.trials):
                    rk = c.rank_top if kind == "top" else ranks[t]
                    h = vote_hist(c, kind, cu, rk)
                    rel = h + r.laplace(0, cu / (EPS * ee), size=c.K) >= thr
                    out = np.where(rel, tot[(eb, t)]
                                   + r.laplace(0, Bs[(eb, t)] / (EPS * ev), size=c.K), 0.0)
                    es.append(c.score(out))
                    rl.append(rel.sum())
                e = float(np.mean(es))
                if best is None or e < best[0]:
                    best = (e, cu, (eb, ee, ev), float(np.mean(rl)))
            res[kind] = best
            print(f"    {kind:<12}{100*best[0]:>8.3f}%{best[3]:>10,.0f}{best[1]:>5}"
                  f"  ({best[2][0]:.3f},{best[2][1]:.3f},{best[2][2]:.3f})", flush=True)
        b = res["truncate"][0] / res["fractional"][0]
        print(f"    -> fractional votes are {b:.3f}x vs random truncation "
              f"({res['fractional'][3]:,.0f} vs {res['truncate'][3]:,.0f} groups released)\n")
        del c


if __name__ == "__main__":
    main()
