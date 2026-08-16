"""Everything that survived, stacked -- and how much headroom is left above it.

Arms, all at the same total budget per query (frozen arms pay eps_0/N amortised):

  published   Google DP as published: ApproxBounds over per-cell values in base-2 bins, ONE C_u
              truncating values and votes together, Laplace tau per query, Laplace values at C_u*U.
  +frozen     ... plus a frozen key set with public predicate pruning (Google supports this).
  ours        l1 per-PU-norm clip (no C_v), C_e=1 votes, Laplace values at B.
  ours+froz   ... plus frozen + pruned key set, so eps_eta moves to the values.
  ours+all    ... plus count-conditioned shrinkage (free post-processing of the released counts).
  ORACLE      ours+all, but with B set to the error-optimal bound and no partition-selection cost
              at all. Not implementable -- it bounds what any amount of further tuning could reach.

The ORACLE arm is the point of this file: it says whether the remaining gap is worth chasing.

    python3 attacks/full_stack.py --no-oracle          # cheap
    python3 attacks/full_stack.py --oracle-points 12   # the oracle arm is the expensive part

RESOURCE NOTE: the ORACLE arm costs oracle_points x filters full passes over every cell. An
earlier version used 60 points x 3 trials x 4 filters = 720 passes and drove the machine to a load
average of 43. Keep --oracle-points small, use --no-oracle when the bound is not needed, and
respect --max-cells.
"""

import argparse
import itertools

import numpy as np

from fineness_sweep import (DELTA, GROUPINGS, SPLITS, Cells, approx_bounds, google_values, tau,
                            votes)

# All PU-side (functions of the customer), which is where the applicability rule says the frozen
# set may be used directly. Group-side filters need public predicate pruning first and are covered
# separately in attacks/frozen_vs_google.py.
FILTERS = {
    "none": "true",
    "acctbal>=4000": "c_acctbal>=4000",
    "acctbal>=8000": "c_acctbal>=8000",
    "acctbal>=9500": "c_acctbal>=9500",
    "acctbal<0": "c_acctbal<0",
    "mktseg=AUTOMOBILE": "c_mktsegment='AUTOMOBILE'",
    "mktseg=BUILDING": "c_mktsegment='BUILDING'",
    "acctbal>=9500 & AUTO": "c_acctbal>=9500 AND c_mktsegment='AUTOMOBILE'",
}


def load(con, gexpr, filt, gfix=None):
    """If gfix is given, the group space is extended with frozen groups absent after filtering:
    their truth is 0 and a frozen release must still emit noise for them, so the cost of freezing
    is charged rather than silently ignored. Without this, group-side filters look far better than
    they are."""
    rows = con.execute(f"""
        WITH c AS (SELECT o_custkey AS pu, {gexpr} AS g, sum(l_extendedprice) AS t
                   FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
                   JOIN tpch.customer ON c_custkey=o_custkey
                   JOIN tpch.nation ON n_nationkey=c_nationkey
                   WHERE {filt} GROUP BY 1,2)
        SELECT dense_rank() OVER (ORDER BY pu)-1 AS pid,
               dense_rank() OVER (ORDER BY g)-1 AS gid, g, t FROM c""").fetchnumpy()
    c = Cells.__new__(Cells)
    c._init_from(rows["pid"].astype(np.int64), rows["gid"].astype(np.int64),
                 rows["t"].astype(np.float64))
    lab = list(np.unique(rows["g"]))
    if gfix:
        extra = sorted(g for g in gfix if g not in set(lab))
        if extra:
            lab += extra
            c.K += len(extra)
            c.truth = np.concatenate([c.truth, np.zeros(len(extra))])
            c.npu_g = np.concatenate([c.npu_g, np.zeros(len(extra))])
    return c, np.array(lab, dtype=object)


def build_gfix(con, gexpr, eps0, delta0, r):
    rows = con.execute(f"""
        SELECT {gexpr} AS g, count(DISTINCT o_custkey) AS n
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey
        JOIN tpch.nation ON n_nationkey=c_nationkey GROUP BY 1""").fetchall()
    lab = np.array([x[0] for x in rows])
    n = np.array([x[1] for x in rows], dtype=float)
    return set(lab[n + r.laplace(0, 1.0 / eps0, size=len(n)) >= tau(eps0, delta0, 1)].tolist())


def shrink(x, cnt, rel, sd):
    """Regress released sums on released counts and shrink toward the fit. Post-processing."""
    m = rel & (cnt > 0)
    if m.sum() <= 10:
        return x
    al = np.sum(cnt[m] * x[m]) / max(np.sum(cnt[m] ** 2), 1e-9)
    pred = al * cnt
    rv = max(float(np.mean((x[m] - pred[m]) ** 2)) - sd ** 2, 0.0)
    w = rv / (rv + sd ** 2)
    return np.where(m, pred + w * (x - pred), x)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--grouping", default="month|nation")
    ap.add_argument("--eps", type=float, default=1.0)
    ap.add_argument("--eps0", type=float, default=1.0)
    ap.add_argument("--n-queries", type=int, default=20)
    ap.add_argument("--trials", type=int, default=3)
    ap.add_argument("--max-cells", type=int, default=6_000_000,
                    help="skip a filter above this many (PU,group) cells")
    ap.add_argument("--oracle-points", type=int, default=12,
                    help="grid size for the ORACLE sweep. Each point is a full clip+bincount over "
                         "every cell, so cost is oracle_points * filters passes over the data -- "
                         "this is the expensive part of this file, keep it small")
    ap.add_argument("--no-oracle", action="store_true",
                    help="skip the oracle arm entirely; it is by far the heaviest part")
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    gexpr = GROUPINGS[a.grouping][0]
    EPS, amort = a.eps, a.eps0 / a.n_queries
    r = np.random.default_rng(99)
    gfix = build_gfix(con, gexpr, a.eps0, 1e-4, r)
    print(f"{a.grouping}, eps={EPS}; G_fix {len(gfix):,} groups, amortised over N={a.n_queries}\n")
    hdr = (f"{'filter':<20}{'published':>11}{'+frozen':>9}{'ours':>8}{'ours+froz':>11}"
           f"{'ours+all':>10}{'ORACLE':>9}{'vs pub':>8}{'headroom':>10}")
    print(hdr)
    print("-" * len(hdr))
    for fname, filt in FILTERS.items():
        c, lab = load(con, gexpr, filt, gfix)
        if len(c.val) > a.max_cells:
            print(f"{fname:<20}  SKIPPED: {len(c.val):,} cells > --max-cells {a.max_cells:,}",
                  flush=True)
            del c
            continue
        in_fix = np.array([g in gfix for g in lab])       # pruning: a group present here is
        mk = int(c.k_u.max())                             # consistent with the predicate already
        cus = sorted({1, 2, 3, 5, 8, 13, 21, 34, mk})
        ranks = [c.rank_random(r) for _ in range(a.trials)]
        vt = {(t, ce): votes(c, ranks[t], ce) for t in range(a.trials) for ce in cus}
        cache = {}
        for eb in sorted({s[0] for s in SPLITS}):
            for t in range(a.trials):
                for eq in (EPS, EPS - amort):
                    U = approx_bounds(c.val, eq * eb, r)
                    B = approx_bounds(c.norms[c.norms > 0], eq * eb, r)
                    cl = np.clip(c.val, -B, B)
                    nu = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
                    o = np.bincount(c.gi, weights=cl * np.minimum(
                        1.0, B / np.maximum(nu, 1e-30))[c.pi], minlength=c.K)
                    cache[(eb, t, eq)] = (U, B, o,
                                          {cv: google_values(c, cv, U, ranks[t], False)
                                           for cv in cus})
        # oracle bound: sweep B on a fine grid against the true error
        mx = float(c.norms.max())
        grid = mx * 2.0 ** np.linspace(-6, 1, a.oracle_points)
        def err_at(B, ev):
            cl = np.clip(c.val, -B, B)
            nu = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
            tot = np.bincount(c.gi, weights=cl * np.minimum(1.0, B/np.maximum(nu, 1e-30))[c.pi],
                              minlength=c.K)
            x = tot + r.laplace(0, B / ev, size=c.K)   # single draw: this is a bound, not an arm
            return c.score(shrink(x, c.npu_g, np.ones(c.K, bool), np.sqrt(2) * B / ev))

        def run(arm, ce, cv, split):
            eb, ee, ev = split
            froz = "froz" in arm or arm in ("ours+all",)
            eq = EPS - amort if froz else EPS
            if froz:
                ev, ee = ev + ee, 1e-9
            thr = tau(eq * ee, DELTA, ce) if not froz else np.inf
            es = []
            for t in range(a.trials):
                U, B, o_tot, g_tot = cache[(eb, t, eq)]
                v = vt[(t, ce)]
                cnt = v + r.laplace(0, ce / (eq * ee), size=c.K)
                rel = in_fix.copy() if froz else ((cnt >= thr) & (v > 0))
                tot, sc = (g_tot[cv], cv * U) if arm.startswith("pub") else (o_tot, B)
                x = tot + r.laplace(0, sc / (eq * ev), size=c.K)
                if arm == "ours+all":
                    x = shrink(x, np.maximum(cnt, 0), rel, np.sqrt(2) * sc / (eq * ev))
                es.append(c.score(np.where(rel, x, 0.0)))
            return float(np.mean(es))

        best = {}
        for arm in ("published", "pub+frozen", "ours", "ours+froz", "ours+all"):
            cvs = cus if arm.startswith("pub") else [None]
            best[arm] = min(run(arm, ce, cv, s)
                            for ce, cv, s in itertools.product(cus, cvs, SPLITS))
        oracle = (float("nan") if a.no_oracle
                  else min(err_at(B, EPS * 0.999) for B in grid))
        print(f"{fname:<20}{100*best['published']:>10.2f}%{100*best['pub+frozen']:>8.2f}%"
              f"{100*best['ours']:>7.2f}%{100*best['ours+froz']:>10.2f}%"
              f"{100*best['ours+all']:>9.2f}%{100*oracle:>8.2f}%"
              f"{best['published']/best['ours+all']:>7.2f}x"
              f"{best['ours+all']/max(oracle,1e-9):>9.2f}x", flush=True)
        del c, cache
    print("\n'vs pub' = the full stack against Google DP as published.")
    print("'headroom' = full stack / ORACLE: how much any further tuning could still win.")


if __name__ == "__main__":
    main()
