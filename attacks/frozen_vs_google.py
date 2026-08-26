"""The persistent frozen key set, measured against Google DP -- and where it stops working.

Four arms, each spending the same total budget per query. The two frozen arms additionally pay
eps_0/N, amortising one key-set release over N later queries on the same (filterless query,
grouping) pair.

  google-tau      Google DP as published: ApproxBounds over cells, ONE C_u truncating values and
                  votes together, Laplace tau per query, Laplace values at C_u*U.
  google-frozen   the same, but the key set comes from the frozen table, so no tau and no eps_eta.
                  Google supports this (Privacy on Beam SelectPartitions -> PublicPartitions,
                  Tumult get_groups -> KeySet), so it is a fair arm, not a strawman.
  ours-tau        l1 per-PU-norm clip + per-query tau.
  ours-frozen     l1 clip + frozen key set.

The risk this is really testing: G_fix is derived from the FILTERLESS data, so a selective filter
leaves many frozen groups empty. Those get released anyway and contribute pure noise. There must be
a selectivity below which freezing hurts -- finding it defines the operating envelope.

    python3 attacks/frozen_vs_google.py [--grouping month|nation]
"""

import argparse
import itertools

import numpy as np

from fineness_sweep import (DELTA, GROUPINGS, SPLITS, Cells, approx_bounds, google_values, tau,
                            votes)

# filters spanning three orders of magnitude of selectivity, all functions of the PU
# PU-side filters remove users but rarely whole groups; GROUP-side filters remove whole groups,
# which is the case that should make freezing lose (frozen-but-absent groups get released as noise).
FILTERS = {
    "none": "true",
    "PU: acctbal>=8000": "c_acctbal>=8000",
    "PU: acctbal>=9900": "c_acctbal>=9900",
    "GRP: ship>=1996": "l_shipdate>=DATE '1996-01-01'",
    "GRP: ship>=1998": "l_shipdate>=DATE '1998-01-01'",
    "GRP: nation<5": "c_nationkey<5",
    "GRP: ship>=1998 & nat<5": "l_shipdate>=DATE '1998-01-01' AND c_nationkey<5",
}


def load(con, gexpr, filt, gfix=None):
    """Cells for the filtered query. If gfix is given, the group space is extended to include
    frozen groups absent after filtering -- their truth is 0, and a frozen release must still emit
    noise for them, so the cost of freezing is charged rather than ignored."""
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
            lab = lab + extra
            c.K += len(extra)
            c.truth = np.concatenate([c.truth, np.zeros(len(extra))])
            c.npu_g = np.concatenate([c.npu_g, np.zeros(len(extra))])
    return c, np.array(lab, dtype=object)


def build_gfix(con, gexpr, eps0, delta0, r):
    """One DP partition-selection release on the filterless data, at C_u=1 (proven optimal)."""
    rows = con.execute(f"""
        SELECT {gexpr} AS g, count(DISTINCT o_custkey) AS n
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey
        JOIN tpch.nation ON n_nationkey=c_nationkey GROUP BY 1""").fetchall()
    lab = np.array([x[0] for x in rows])
    n = np.array([x[1] for x in rows], dtype=float)
    keep = n + r.laplace(0, 1.0 / eps0, size=len(n)) >= tau(eps0, delta0, 1)
    return set(lab[keep].tolist())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--grouping", default="month|nation")
    ap.add_argument("--eps", type=float, default=1.0)
    ap.add_argument("--eps0", type=float, default=1.0)
    ap.add_argument("--delta0", type=float, default=1e-4)
    ap.add_argument("--n-queries", type=int, default=20)
    ap.add_argument("--trials", type=int, default=3)
    ap.add_argument("--score-nonempty-only", action="store_true",
                    help="do not charge public frozen groups whose filtered truth is zero")
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    gexpr = GROUPINGS[a.grouping][0]
    r = np.random.default_rng(99)
    gfix = build_gfix(con, gexpr, a.eps0, a.delta0, r)
    EPS = a.eps
    amort = a.eps0 / a.n_queries

    print(f"{a.grouping}, eps={EPS}, delta={DELTA:g}; G_fix from filterless data at "
          f"eps_0={a.eps0}, delta_0={a.delta0:g} -> {len(gfix):,} groups")
    print(f"amortised over N={a.n_queries} queries, so the frozen arms run at "
          f"eps={EPS-amort:.3f} while the tau arms run at {EPS:.3f}\n")
    if a.score_nonempty_only:
        print("utility scope: filtered groups with nonzero truth; empty public groups are ignored\n")
    hdr = (f"{'filter':<22}{'groups':>8}{'in Gfix':>9}{'empty':>7}"
           f"{'goog-tau':>10}{'goog-froz':>11}{'ours-tau':>10}{'ours-froz':>11}{'vs goog':>9}")
    print(hdr)
    print("-" * len(hdr))
    for fname, filt in FILTERS.items():
        c, lab = load(con, gexpr, filt, gfix)
        in_fix = np.array([g in gfix for g in lab])
        n_empty = int((in_fix & (c.truth == 0)).sum())
        mk = int(c.k_u.max())
        cus = sorted({1, 2, 3, 5, 8, 13, 21, 34, mk})
        ranks = [c.rank_random(r) for _ in range(a.trials)]
        vt = {(t, ce): votes(c, ranks[t], ce) for t in range(a.trials) for ce in cus}
        cache = {}
        for eb in sorted({s[0] for s in SPLITS}):
            for t in range(a.trials):
                for e_q in (EPS, EPS - amort):
                    U = approx_bounds(c.val, e_q * eb, r)
                    B = approx_bounds(c.norms[c.norms > 0], e_q * eb, r)
                    cl = np.clip(c.val, -B, B)
                    n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
                    o = np.bincount(c.gi, weights=cl * np.minimum(
                        1.0, B / np.maximum(n_u, 1e-30))[c.pi], minlength=c.K)
                    cache[(eb, t, e_q)] = (
                        U, B, o, {cv: google_values(c, cv, U, ranks[t], False) for cv in cus})

        def run(arm, ce, cv, split):
            eb, ee, ev = split
            frozen = arm.endswith("frozen")
            e_q = EPS - amort if frozen else EPS
            if frozen:                       # no tau needed: the whole eps_eta goes to values
                ev, ee = ev + ee, 1e-9
            thr = tau(e_q * ee, DELTA, ce) if not frozen else np.inf
            es = []
            for t in range(a.trials):
                U, B, o_tot, g_tot = cache[(eb, t, e_q)]
                if frozen:
                    rel = in_fix.copy()
                else:
                    v = vt[(t, ce)]
                    rel = (v + r.laplace(0, ce / (e_q * ee), size=c.K) >= thr) & (v > 0)
                tot, sc = ((g_tot[cv], cv * U) if arm.startswith("goog") else (o_tot, B))
                out = np.where(rel, tot + r.laplace(0, sc / (e_q * ev), size=c.K), 0.0)
                if a.score_nonempty_only:
                    keep = c.truth != 0
                    es.append(float(np.sum(np.abs(out[keep] - c.truth[keep])) /
                                    np.sum(np.abs(c.truth[keep]))))
                else:
                    es.append(c.score(out))
            return float(np.mean(es))

        best = {}
        for arm in ("goog-tau", "goog-frozen", "ours-tau", "ours-frozen"):
            cvs = cus if arm.startswith("goog") else [None]
            best[arm] = min(run(arm, ce, cv, s)
                            for ce, cv, s in itertools.product(cus, cvs, SPLITS))
        vs = best["goog-tau"] / best["ours-frozen"]
        print(f"{fname:<22}{c.K:>8,}{int(in_fix.sum()):>9,}{n_empty:>7,}"
              f"{100*best['goog-tau']:>9.2f}%{100*best['goog-frozen']:>10.2f}%"
              f"{100*best['ours-tau']:>9.2f}%{100*best['ours-frozen']:>10.2f}%{vs:>8.2f}x",
              flush=True)
        del c, cache
    print("\n'empty' = groups in G_fix absent from the filtered query; they are released anyway")
    if a.score_nonempty_only:
        print("and are excluded from this Peter-scope utility metric.")
    else:
        print("and contribute pure noise, which is what should eventually make freezing lose.")
    print("'vs goog' compares ours-frozen against Google DP as published (goog-tau).")


if __name__ == "__main__":
    main()
