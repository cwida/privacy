"""Dandan's frozen group set G_fix: pay for partition selection once, reuse it across queries.

Release the group set ONCE from the filterless data under its own (eps_0, delta_0). It is then
public. Every later filtered query releases groups in G_fix with NO tau test -- their existence is
already public -- and applies tau only to groups outside it.

This is sound: partition selection protects group EXISTENCE, not values. A frozen group holding a
single user after filtering is still safe, because its value carries the usual Laplace(B/eps_v).
It is the public-partition model that FLEX/Chorus assume (and that dp_elastic already relies on),
obtained legitimately instead of assumed.

The structural reason it should work: the filterless query has the MOST users per group, so it
clears tau most easily; filtered queries have the fewest and suffer most. Freezing transfers the
easy query's key set to the hard ones.

Two gains, and the second is the larger:
  1. groups in G_fix are never suppressed
  2. eps_eta is freed for the value channel on those groups -- typically 30-60% of the budget

Cost: eps_0 is paid once and amortised over N queries, so the comparison is reported vs N.

    python3 attacks/frozen_partition.py
"""

import argparse
import itertools

import numpy as np

from fineness_sweep import DELTA, GROUPINGS, SPLITS, Cells, approx_bounds, tau, votes


def build_gfix(con, gexpr, eps0, delta0, cu, r, extra='true'):
    """DP group selection on the FILTERLESS data: noised distinct-PU count per group vs Wilson tau.
    Returns the frozen set as a set of group labels."""
    rows = con.execute(f"""
        SELECT {gexpr} AS g, count(DISTINCT o_custkey) AS n
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey
        JOIN tpch.nation ON n_nationkey=c_nationkey
        WHERE {extra} GROUP BY 1""").fetchall()
    labels = np.array([x[0] for x in rows])
    n = np.array([x[1] for x in rows], dtype=float)
    # each PU truncated to cu groups for this one release
    thr = tau(eps0, delta0, cu)
    keep = n / max(cu, 1) * cu + r.laplace(0, cu / eps0, size=len(n)) >= thr
    return set(labels[keep].tolist()), len(labels)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--filter", default="c_acctbal>=8000")
    ap.add_argument("--grouping", default="month|nation")
    ap.add_argument("--extra", default="true",
                    help="extra predicate applied to BOTH the filterless and filtered queries; "
                         "use a PU-function like c_nationkey<5 to shrink cells while preserving "
                         "n_g/k_u exactly")
    ap.add_argument("--eps", type=float, default=1.0)
    ap.add_argument("--eps0", type=float, default=1.0, help="budget for the one-off G_fix release")
    ap.add_argument("--delta0", type=float, default=1e-4, help="larger delta, paid once")
    ap.add_argument("--trials", type=int, default=4)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    gexpr = GROUPINGS[a.grouping][0]
    EPS = a.eps
    r = np.random.default_rng(99)

    gfix, n_all = build_gfix(con, gexpr, a.eps0, a.delta0, 1, r, a.extra)
    # the filtered query, with the group label kept so membership in G_fix can be tested
    rows = con.execute(f"""
        WITH c AS (SELECT o_custkey AS pu, {gexpr} AS g, sum(l_extendedprice) AS t
                   FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
                   JOIN tpch.customer ON c_custkey=o_custkey
                   JOIN tpch.nation ON n_nationkey=c_nationkey
                   WHERE ({a.filter}) AND ({a.extra}) GROUP BY 1,2)
        SELECT dense_rank() OVER (ORDER BY pu)-1 AS pid,
               dense_rank() OVER (ORDER BY g)-1 AS gid, g, t FROM c""").fetchnumpy()
    c = Cells.__new__(Cells)
    c._init_from(rows["pid"].astype(np.int64), rows["gid"].astype(np.int64),
                 rows["t"].astype(np.float64))
    lab = np.empty(c.K, dtype=object)
    lab[rows["gid"].astype(np.int64)] = rows["g"]
    in_fix = np.array([g in gfix for g in lab])

    print(f"{a.grouping}, filter '{a.filter}', eps={EPS}, delta={DELTA:g}")
    print(f"  filterless groups: {n_all:,};  G_fix (eps_0={a.eps0}, delta_0={a.delta0:g}): "
          f"{len(gfix):,} ({100*len(gfix)/n_all:.1f}%)")
    print(f"  filtered query has {c.K:,} groups, {c.P:,} PUs; "
          f"{int(in_fix.sum()):,} of them ({100*in_fix.mean():.1f}%) are in G_fix")
    print(f"  those hold {100*np.abs(c.truth[in_fix]).sum()/np.abs(c.truth).sum():.1f}% "
          f"of the true mass\n")

    mk = int(c.k_u.max())
    cus = sorted({1, 2, 3, 5, 8, 13, 21, 34, mk})
    ranks = [c.rank_random(r) for _ in range(a.trials)]
    vt = {(t, ce): votes(c, ranks[t], ce) for t in range(a.trials) for ce in cus}
    cache = {}
    for eb in sorted({s[0] for s in SPLITS}):
        for t in range(a.trials):
            B = approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
            cl = np.clip(c.val, -B, B)
            n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
            cache[(eb, t)] = (B, np.bincount(
                c.gi, weights=cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi],
                minlength=c.K))

    def run(ce, split, frozen, n_queries=None):
        """When n_queries is set, the frozen arm pays eps_0/N out of its OWN per-query budget, so
        both arms spend the same total. Without it the frozen arm is silently given extra eps."""
        eb, ee, ev = split
        eps_q = EPS if not frozen or n_queries is None else EPS - a.eps0 / n_queries
        if eps_q <= 0:
            return 1.0, 0.0
        if frozen:
            # groups in G_fix need no tau, so eps_eta only guards the rest -- and when G_fix
            # covers everything the whole eps_eta moves to the value channel
            ev = ev + ee * float(in_fix.mean())
            ee = max(ee * (1.0 - float(in_fix.mean())), 1e-6)
        thr = tau(eps_q * ee, DELTA, ce)
        es, nrel = [], []
        for t in range(a.trials):
            v = vt[(t, ce)]
            rel = (v + r.laplace(0, ce / (eps_q * ee), size=c.K) >= thr) & (v > 0)
            if frozen:
                rel = rel | in_fix
            B, tot = cache[(eb, t)]
            es.append(c.score(np.where(rel, tot + r.laplace(0, B / (eps_q * ev), size=c.K), 0.0)))
            nrel.append(rel.sum())
        return float(np.mean(es)), float(np.mean(nrel))

    best = {}
    for frozen in (False, True):
        cands = [(run(ce, s, frozen), ce, s) for ce, s in itertools.product(cus, SPLITS)]
        best[frozen] = min(cands)
        (e, nr), ce, s = best[frozen]
        print(f"  {'with G_fix' if frozen else 'tau only  '}  {100*e:>8.3f}%   "
              f"released {nr:>6,.0f}/{c.K:,}   C_e={ce:<3} "
              f"split=({s[0]:.3f},{s[1]:.3f},{s[2]:.3f})")
    print(f"\n  gain ignoring the cost of G_fix: "
          f"{best[False][0][0]/best[True][0][0]:.2f}x  (NOT a fair comparison)")
    print(f"\n  FAIR: eps_0={a.eps0} amortised over N queries, charged to the frozen arm so both")
    print(f"  arms spend the same total budget per query.")
    print(f"    {'N':>5}{'query eps':>11}{'tau only':>11}{'with G_fix':>12}{'gain':>8}")
    for N in (1, 2, 5, 10, 20, 100):
        v = min(run(ce, s, True, N) for ce, s in itertools.product(cus, SPLITS))
        print(f"    {N:>5}{EPS - a.eps0/N:>11.3f}{100*best[False][0][0]:>10.2f}%"
              f"{100*v[0]:>11.2f}%{best[False][0][0]/v[0]:>7.2f}x")


if __name__ == "__main__":
    main()
