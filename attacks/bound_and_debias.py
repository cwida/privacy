"""Two levers on the value channel that have measured positives but were never re-validated.

Both reuse the per-PU norm histogram we ALREADY pay eps_b for, so both are post-processing of a
released object -- no extra budget.

1. MSE-OPTIMAL B instead of ApproxBounds.
   ApproxBounds is a MAX-finder: it returns the top occupied bin, which is the largest value in
   the data, not the value that minimises error. Adversarial review found exactly this costs
   Google 2x on its per-cell bound U. We never applied the same criticism to our own B. Choosing
   B to minimise (clip bias + noise) directly, estimated from the noisy histogram:

       err(B) ~ sum_bins noisy_j * max(mid_j - B, 0)   +   K * B / eps_v
                \_______ mass clipped away _______/       \___ total noise ___/

   The earlier "adaptive objective rule" in the notes was this idea, but implemented over all 64
   log2 bins so that empty high bins carried Laplace noise times 2^63.5 and swamped the objective.
   Fixed here: clamp noisy counts at zero and cap the search at the top OCCUPIED bin.

2. DEBIASING THE CLIP LOSS.
   The clip removes sum_u max(||t_u||_1 - B, 0) of mass. That total is estimable from the same
   noisy histogram, and it is removed roughly in proportion to each group's share, so adding it
   back proportionally cancels most of the bias. Measured at 1.19-1.25x against the OLD baseline,
   never re-checked against the fixed one.

    python3 attacks/bound_and_debias.py [--grouping month|nation]
"""

import argparse
import itertools

import numpy as np

from fineness_sweep import (DELTA, GROUPINGS, SPLITS, Cells, approx_bounds, google_values, tau,
                            votes)


def norm_histogram(norms, eps_b, r):
    """The object both levers post-process: a noisy log2 histogram of per-PU norms, l1 sens 1."""
    bins = np.clip(np.floor(np.log2(np.maximum(norms, 1.0))).astype(int), 0, 45)
    ub, cb = np.unique(bins, return_counts=True)
    return ub, np.maximum(cb + r.laplace(0, 1.0 / eps_b, size=len(ub)), 0.0)


def mse_optimal_B(ub, noisy, K, eps_v):
    """Pick B minimising estimated clip bias + total noise, over the occupied bins only."""
    mids = 2.0 ** (ub + 0.5)
    cands = 2.0 ** (ub + 1)
    clipped = np.array([float(np.sum(noisy * np.maximum(mids - B, 0.0))) for B in cands])
    noise = K * cands / eps_v
    return float(cands[int(np.argmin(clipped + noise))])


def clipped_mass(ub, noisy, B):
    """Estimated total mass the clip removes, from the same noisy histogram."""
    mids = 2.0 ** (ub + 0.5)
    return float(np.sum(noisy * np.maximum(mids - B, 0.0)))


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
    mk = int(c.k_u.max())
    cus = sorted({1, 2, 3, 5, 8, 13, 21, 34, 55, mk})
    r = np.random.default_rng(99)
    ranks = [c.rank_random(r) for _ in range(a.trials)]
    vt = {(t, ce): votes(c, ranks[t], ce) for t in range(a.trials) for ce in cus}
    print(f"{a.grouping}: {c.K:,} groups, {c.P:,} PUs; eps={EPS}. True max per-PU norm "
          f"{c.norms.max():,.0f}\n")

    def totals(B):
        cl = np.clip(c.val, -B, B)
        n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
        return np.bincount(c.gi, weights=cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi],
                           minlength=c.K)

    cache = {}
    for eb, ev in {(s[0], s[2]) for s in SPLITS}:
        for t in range(a.trials):
            ub, noisy = norm_histogram(c.norms[c.norms > 0], EPS * eb, r)
            B_ab = approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
            B_ms = mse_optimal_B(ub, noisy, c.K, EPS * ev)
            cache[(eb, ev, t)] = (B_ab, B_ms, totals(B_ab), totals(B_ms),
                                  clipped_mass(ub, noisy, B_ab), clipped_mass(ub, noisy, B_ms))
    U, g_tot = {}, {}
    for eb in sorted({s[0] for s in SPLITS}):
        for t in range(a.trials):
            U[(eb, t)] = approx_bounds(c.val, EPS * eb, r)
            for cv in cus:
                g_tot[(eb, t, cv)] = google_values(c, cv, U[(eb, t)], ranks[t], False)

    def run(arm, ce, cv, split):
        eb, ee, ev = split
        thr = tau(EPS * ee, DELTA, ce)
        es = []
        for t in range(a.trials):
            rel = (vt[(t, ce)] + r.laplace(0, ce / (EPS * ee), size=c.K) >= thr) & (vt[(t, ce)] > 0)
            B_ab, B_ms, tot_ab, tot_ms, cm_ab, cm_ms = cache[(eb, ev, t)]
            if arm == "published":
                tot, sc, add = g_tot[(eb, t, cv)], cv * U[(eb, t)] / (EPS * ev), 0.0
            elif arm == "l1+ApproxBounds":
                tot, sc, add = tot_ab, B_ab / (EPS * ev), 0.0
            elif arm == "l1+MSE-B":
                tot, sc, add = tot_ms, B_ms / (EPS * ev), 0.0
            elif arm == "l1+AB+debias":
                tot, sc, add = tot_ab, B_ab / (EPS * ev), cm_ab
            else:
                tot, sc, add = tot_ms, B_ms / (EPS * ev), cm_ms
            out = tot + r.laplace(0, sc, size=c.K)
            if add > 0:                       # spread the estimated clipped mass proportionally
                share = np.maximum(out, 0.0)
                s = share.sum()
                if s > 0:
                    out = out + add * share / s
            es.append(c.score(np.where(rel, out, 0.0)))
        return float(np.mean(es))

    best = {}
    for arm in ("published", "l1+ApproxBounds", "l1+MSE-B", "l1+AB+debias", "l1+MSE-B+debias"):
        cvs = cus if arm == "published" else [None]
        best[arm] = min((run(arm, ce, cv, s), ce, s)
                        for ce, cv, s in itertools.product(cus, cvs, SPLITS))
        e, ce, s = best[arm]
        print(f"  {arm:<18}{100*e:>8.3f}%   C_e={ce:<4} "
              f"split=({s[0]:.3f},{s[1]:.3f},{s[2]:.3f})")
    base = best["l1+ApproxBounds"][0]
    print(f"\n  vs published        : {best['published'][0]/min(v[0] for v in best.values()):.2f}x")
    for arm in ("l1+MSE-B", "l1+AB+debias", "l1+MSE-B+debias"):
        print(f"  {arm:<18} vs plain l1: {base/best[arm][0]:.3f}x")
    eb0, ev0 = 0.002, 0.598
    B_ab, B_ms = cache[(eb0, ev0, 0)][0], cache[(eb0, ev0, 0)][1]
    print(f"\n  at eps_b=0.002: ApproxBounds B = {B_ab:,.0f}, MSE-optimal B = {B_ms:,.0f} "
          f"({B_ab/B_ms:.1f}x lower)")


if __name__ == "__main__":
    main()
