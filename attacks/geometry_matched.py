"""Geometry-matched noise: a generic rule, and the package measured against Google DP as published.

THE RULE. A grouped DP release has several independent channels. For each, look at ONE PU's
contribution vector to that channel and compute its l1 and l2 norms. Laplace pays l1, Gaussian
pays l2 x sqrt(2 ln(1.25/delta)). So Gaussian is the right choice for a channel iff

    l1 / l2  >  sqrt(ln(1.25/delta))    (~3.7 at delta = 1e-6)

Applying it channel by channel:

  channel                          per-PU vector          l1     l2        l1/l2     -> noise
  value, under an l1 norm clip     mass over groups        B      B          1.0        Laplace
  value, Google (C_v cells <= U)   <= C_v cells at U     C_v*U  sqrt(C_v)*U  sqrt(C_v)  Gaussian if C_v>14
  votes (partition selection)      1 in each of k_u       k_u    sqrt(k_u)   sqrt(k_u)  Gaussian if k_u>14
  bounds histogram, per-PU norms   exactly one bin         1      1          1.0        Laplace
  bounds histogram, per-cell       <= C_v bins           C_v    sqrt(C_v)    sqrt(C_v)  Gaussian if C_v>14

The l1 clip is what makes the value channel l1/l2 = 1: it lets a PU concentrate all its mass in
one group, so the worst case is fully concentrated and there is no l2 advantage to buy. That is
why Gaussian loses on values and wins on votes -- the same mechanism, opposite geometries.

"GOOGLE AS PUBLISHED" here means Wilson et al. 2020 / the Google DP library: ApproxBounds over
per-cell values, ONE C_u used to randomly truncate values and votes together, Laplace values at
C_u*U, Laplace votes at C_u with the Wilson tau. The rescale-to-true-total trick and the
C_e/C_v decoupling are NOT in that paper or that library -- they were invented in this project's
own adversarial review, so they belong in a separate column, not in the baseline.

    python3 attacks/geometry_matched.py
"""

import argparse
import itertools

import numpy as np
from scipy.stats import norm

from fineness_sweep import (DELTA, EPS, GROUPINGS, SPLITS, Cells, approx_bounds, google_values,
                            tau, votes)


def gauss_sigma(l2, eps, delta):
    return l2 * np.sqrt(2.0 * np.log(1.25 / delta)) / eps


def rule_threshold(delta):
    """l1/l2 ratio above which Gaussian beats Laplace, comparing standard deviations."""
    return np.sqrt(2.0 * np.log(1.25 / delta)) / np.sqrt(2.0)


def run(c, cache, ce, cv, split, arm, vote_noise, r, mk):
    eb, ee, ev = split
    if vote_noise == "gauss":
        sigma = gauss_sigma(np.sqrt(min(ce, mk)), EPS * ee, DELTA / 2)
        thr = 1.0 + sigma * norm.ppf(1.0 - (DELTA / 2) / max(ce, 1))
    else:
        thr = tau(EPS * ee, DELTA, ce)
    es, rel_n = [], []
    for t in range(cache["trials"]):
        vn = (r.normal(0, sigma, size=c.K) if vote_noise == "gauss"
              else r.laplace(0, ce / (EPS * ee), size=c.K))
        rel = cache["votes"][(t, ce)] + vn >= thr
        rel &= cache["votes"][(t, ce)] > 0            # vote-support gate: free, fixes delta
        if arm == "ours":
            tot, sc = cache["o_tot"][(eb, t)], cache["B"][(eb, t)]
        else:
            tot, sc = cache["g_tot"][(eb, t, cv)], cv * cache["U"][(eb, t)]
        out = np.where(rel, tot + r.laplace(0, sc / (EPS * ev), size=c.K), 0.0)
        es.append(c.score(out))
        rel_n.append(rel.sum())
    return float(np.mean(es)), float(np.mean(rel_n))


def build(c, cus, trials):
    r = np.random.default_rng(99)
    ranks = [c.rank_random(r) for _ in range(trials)]
    cache = {"trials": trials,
             "votes": {(t, ce): votes(c, ranks[t], ce) for t in range(trials) for ce in cus},
             "U": {}, "B": {}, "g_tot": {}, "o_tot": {}}
    for eb in sorted({s[0] for s in SPLITS}):
        for t in range(trials):
            U = approx_bounds(c.val, EPS * eb, r)
            cache["U"][(eb, t)] = U
            for cv in cus:
                cache["g_tot"][(eb, t, cv)] = google_values(c, cv, U, ranks[t], False)
            B = approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
            cache["B"][(eb, t)] = B
            cl = np.clip(c.val, -B, B)
            n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
            cache["o_tot"][(eb, t)] = np.bincount(
                c.gi, weights=cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi],
                minlength=c.K)
    return cache, r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--filter", default="c_acctbal>=8000")
    ap.add_argument("--groupings", default="month,month|priority,month|nation,day,"
                                           "week|nation,day|region")
    ap.add_argument("--trials", type=int, default=3)
    ap.add_argument("--max-cells", type=int, default=7_000_000,
                    help="skip a grouping above this many (PU,group) cells; the tuner holds "
                         "several arrays of this length per trial and will OOM a laptop")
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    print(f"SUM(l_extendedprice), PU=customer, WHERE {a.filter}, eps={EPS}, delta={DELTA:g}")
    print(f"Gaussian is predicted to win a channel iff l1/l2 > {rule_threshold(DELTA):.2f}\n")
    print("  'published'  = Wilson et al. / Google DP library: one C_u truncating values AND")
    print("                 votes, Laplace everywhere, ApproxBounds on per-cell values.")
    print("  'package'    = l1 per-PU-norm clip + Laplace values + GAUSSIAN untruncated votes")
    print("                 + vote-support gate; ApproxBounds on per-PU norms.\n")
    hdr = (f"{'grouping':<15}{'n_g/k_u':>9}{'published':>11}{'package':>10}{'gain':>8}"
           f"{'rel(pub)':>10}{'rel(pkg)':>10}{'of':>8}")
    print(hdr)
    print("-" * len(hdr))
    rows = []
    for name in a.groupings.split(","):
        c = Cells(con, GROUPINGS[name][0], a.filter)
        if len(c.val) > a.max_cells:
            print(f"{name:<15}  SKIPPED: {len(c.val):,} cells > --max-cells "
                  f"{a.max_cells:,}", flush=True)
            del c
            continue
        mk = int(c.k_u.max())
        cus = sorted({1, 2, 5, 10, 19, 30, mk // 2 or 1, mk})
        cache, r = build(c, cus, a.trials)
        # published: ONE C_u for values and votes, Laplace both
        pub = min((run(c, cache, cu, cu, s, "google", "laplace", r, mk), cu, s)
                  for cu, s in itertools.product(cus, SPLITS))
        # package: our clip, Gaussian untruncated votes
        pkg = min((run(c, cache, ce, None, s, "ours", "gauss", r, mk), ce, s)
                  for ce, s in itertools.product(cus, SPLITS))
        ratio = np.median(c.npu_g) / mk
        rows.append((name, ratio, pub[0][0], pkg[0][0]))
        print(f"{name:<15}{ratio:>9.1f}{100*pub[0][0]:>10.2f}%{100*pkg[0][0]:>9.2f}%"
              f"{pub[0][0]/pkg[0][0]:>7.2f}x{pub[0][1]:>10,.0f}{pkg[0][1]:>10,.0f}"
              f"{c.K:>8,}", flush=True)
        del c, cache
    g = [r[2] / r[3] for r in rows]
    print(f"\ngain vs Google DP as published: min {min(g):.2f}x, median {np.median(g):.2f}x, "
          f"max {max(g):.2f}x")


if __name__ == "__main__":
    main()
