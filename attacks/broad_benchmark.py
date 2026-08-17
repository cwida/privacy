"""Broad utility comparison against Google DP: many queries, three datasets, SUM and COUNT.

Arms, all at eps total per query:

  google-best   Google DP as published (one C_u truncating values and votes together, Laplace on
                every channel, ApproxBounds over per-cell values) given its BEST C_u for that
                query. This is an upper bound on any Google configuration -- an analyst cannot do
                better than knowing the answer in advance.
  google-EM     the same, but C_u chosen by Dandan's exponential mechanism at p=0.95, paying
                eps_N + eps_C = 0.01. This is what an automated Google would actually achieve.
  ours          l1 per-PU-norm clip (no C_v to choose) + C_e=1 votes.
  ours+frozen   ... plus a frozen key set derived once from the filterless version of the same
                query under its own (eps_0, delta_0), amortised over N queries.

Scoring throughout: relative L1 over the TRUE uncapped key set, tau-suppressed groups charged as
released 0, every arm tuned over its own remaining free parameters.

    python3 attacks/broad_benchmark.py --no-frozen     # cheaper
"""

import argparse
import itertools

import numpy as np

from fineness_sweep import (DELTA, SPLITS, Cells, approx_bounds, google_values, tau, votes)
from em_cu import em_select

# name: (select body with {W} for the filter, filter, groups-are-in dataset)
Q = {
    "tpch SUM price / month": ("""SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m') g,
        sum(l_extendedprice) t FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE {W} GROUP BY 1,2""", "c_acctbal>=8000"),
    "tpch SUM price / mo|nation": ("""SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m')||'|'||
        cast(c_nationkey as varchar) g, sum(l_extendedprice) t
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE {W} GROUP BY 1,2""", "c_acctbal>=8000"),
    "tpch SUM price / mo|prio": ("""SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m')||'|'||
        o_orderpriority g, sum(l_extendedprice) t
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE {W} GROUP BY 1,2""", "c_acctbal>=9500"),
    "tpch SUM price / day": ("""SELECT o_custkey pu, cast(l_shipdate as varchar) g,
        sum(l_extendedprice) t FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE {W} GROUP BY 1,2""", "c_acctbal>=9500"),
    "tpch COUNT / mo|nation": ("""SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m')||'|'||
        cast(c_nationkey as varchar) g, count(*) t
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE {W} GROUP BY 1,2""", "c_acctbal>=8000"),
    "tpch SUM qty / mo|nation": ("""SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m')||'|'||
        cast(c_nationkey as varchar) g, sum(l_quantity) t
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE {W} GROUP BY 1,2""", "c_acctbal>=8000"),
    "tpch SUM price / yr|nation": ("""SELECT o_custkey pu, cast(year(l_shipdate) as varchar)||'|'||
        cast(c_nationkey as varchar) g, sum(l_extendedprice) t
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE {W} GROUP BY 1,2""", "c_acctbal>=8000"),
    "tpch SUM price / mo|nat AUTO": ("""SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m')||'|'||
        cast(c_nationkey as varchar) g, sum(l_extendedprice) t
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE {W} GROUP BY 1,2""",
        "c_mktsegment='AUTOMOBILE'"),
    "so COUNT posts / month": ("""SELECT OwnerUserId pu, strftime(CreationDate,'%Y-%m') g,
        count(*) t FROM so.Posts WHERE {W} GROUP BY 1,2""", "OwnerUserId IS NOT NULL"),
    "so SUM score / month": ("""SELECT OwnerUserId pu, strftime(CreationDate,'%Y-%m') g,
        sum(greatest(Score,0)) t FROM so.Posts WHERE {W} GROUP BY 1,2""",
        "OwnerUserId IS NOT NULL"),
    "so COUNT comments / month": ("""SELECT UserId pu, strftime(CreationDate,'%Y-%m') g,
        count(*) t FROM so.Comments WHERE {W} GROUP BY 1,2""", "UserId IS NOT NULL"),
    "so COUNT posts / day": ("""SELECT OwnerUserId pu, cast(cast(CreationDate as date) as varchar)
        g, count(*) t FROM so.Posts WHERE {W} GROUP BY 1,2""", "OwnerUserId IS NOT NULL"),
    "cb COUNT hits / region": ("""SELECT UserID pu, cast(RegionID as varchar) g, count(*) t
        FROM cb.hits WHERE {W} GROUP BY 1,2""", "UserID IS NOT NULL"),
    "cb COUNT hits / date|reg": ("""SELECT UserID pu, cast(EventDate as varchar)||'|'||
        cast(RegionID as varchar) g, count(*) t FROM cb.hits WHERE {W} GROUP BY 1,2""",
        "UserID IS NOT NULL"),
    "cb SUM width / region": ("""SELECT UserID pu, cast(RegionID as varchar) g,
        sum(ResolutionWidth) t FROM cb.hits WHERE {W} GROUP BY 1,2""", "UserID IS NOT NULL"),
}


def load(con, body, where, gfix=None):
    rows = con.execute(f"""WITH c AS ({body.format(W=where)})
        SELECT dense_rank() OVER (ORDER BY pu)-1 pid, dense_rank() OVER (ORDER BY g)-1 gid, g, t
        FROM c""").fetchnumpy()
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


def build_gfix(con, body, eps0, delta0, r):
    """DP partition selection on the FILTERLESS version of the same query, C_e=1."""
    rows = con.execute(f"""WITH c AS ({body.format(W="true")})
        SELECT g, count(DISTINCT pu) n FROM c GROUP BY 1""").fetchall()
    lab = np.array([x[0] for x in rows])
    n = np.array([x[1] for x in rows], float)
    return set(lab[n + r.laplace(0, 1.0 / eps0, size=len(n)) >= tau(eps0, delta0, 1)].tolist())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eps", type=float, default=1.0)
    ap.add_argument("--eps0", type=float, default=1.0)
    ap.add_argument("--n-queries", type=int, default=20)
    ap.add_argument("--p", type=float, default=0.95)
    ap.add_argument("--trials", type=int, default=2)
    ap.add_argument("--max-cells", type=int, default=6_000_000)
    ap.add_argument("--no-frozen", action="store_true")
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    for al, p in (("tpch", "tpch_sass_sf10.db"), ("so", "stackoverflow_dba_sqlstorm.db"),
                  ("cb", "clickbench_micro.db")):
        con.execute(f"ATTACH '/home/ila/Code/privacy/{p}' AS {al} (READ_ONLY)")
    EPS, amort = a.eps, a.eps0 / a.n_queries
    r = np.random.default_rng(99)
    EM = 0.01
    print(f"eps={EPS}, delta={DELTA:g}, {a.trials} trials. google-best gets its optimal C_u per")
    print(f"query (an upper bound on any Google config); google-EM uses the EM at p={a.p}.")
    print(f"Frozen arm amortises eps_0={a.eps0} over N={a.n_queries}.\n")
    hdr = (f"{'query':<30}{'groups':>8}{'g-best':>9}{'g-EM':>8}{'ours':>8}"
           + ("" if a.no_frozen else f"{'ours+froz':>11}") + f"{'vs g-best':>11}")
    print(hdr)
    print("-" * len(hdr))
    gains = []
    for name, (body, where) in Q.items():
        gfix = None if a.no_frozen else build_gfix(con, body, a.eps0, 1e-4, r)
        c, lab = load(con, body, where, gfix)
        if len(c.val) > a.max_cells:
            print(f"{name:<30}  SKIPPED: {len(c.val):,} cells")
            del c
            continue
        in_fix = (np.array([g in gfix for g in lab]) if gfix else None)
        mk = int(c.k_u.max())
        cands = sorted(set([1, 2, 3, 5, 8, 13, 21, 34, 55, 89]) | {mk})
        ranks = [c.rank_random(r) for _ in range(a.trials)]
        vt = {(t, ce): votes(c, ranks[t], ce) for t in range(a.trials) for ce in cands}
        cache = {}
        for eb in sorted({s[0] for s in SPLITS}):
            for t in range(a.trials):
                for eq in ({EPS, EPS - amort, EPS - EM} if not a.no_frozen else {EPS, EPS - EM}):
                    U = approx_bounds(c.val, eq * eb, r)
                    B = approx_bounds(c.norms[c.norms > 0], eq * eb, r)
                    cl = np.clip(c.val, -B, B)
                    nu = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
                    o = np.bincount(c.gi, weights=cl * np.minimum(
                        1.0, B / np.maximum(nu, 1e-30))[c.pi], minlength=c.K)
                    cache[(eb, t, eq)] = (U, B, o,
                                          {cv: google_values(c, cv, U, ranks[t], False)
                                           for cv in cands})

        def run(arm, ce, cv, split, eq):
            eb, ee, ev = split
            froz = arm == "ours+froz"
            if froz:
                ev, ee = ev + ee, 1e-9
            thr = tau(eq * ee, DELTA, ce) if not froz else np.inf
            es = []
            for t in range(a.trials):
                U, B, o_tot, g_tot = cache[(eb, t, eq)]
                v = vt[(t, ce)]
                rel = in_fix if froz else ((v + r.laplace(0, ce / (eq * ee), size=c.K) >= thr)
                                          & (v > 0))
                tot, sc = (g_tot[cv], cv * U) if arm.startswith("g") else (o_tot, B)
                es.append(c.score(np.where(rel, tot + r.laplace(0, sc / (eq * ev), size=c.K), 0.0)))
            return float(np.mean(es))

        # google as published couples C_e and C_v to one C_u
        gbest = min(run("g", cu, cu, s, EPS) for cu, s in itertools.product(cands, SPLITS))
        pk = int(np.median([em_select(c.k_u, mk, a.p, EM / 5, 4 * EM / 5, 0.01, r)[0]
                            for _ in range(9)]))
        cu_em = min(cands, key=lambda y: abs(y - pk))
        gem = min(run("g", cu_em, cu_em, s, EPS - EM) for s in SPLITS)
        ours = min(run("o", ce, None, s, EPS) for ce, s in itertools.product(cands, SPLITS))
        row = (f"{name:<30}{c.K:>8,}{100*gbest:>8.2f}%{100*gem:>7.2f}%{100*ours:>7.2f}%")
        best_ours = ours
        if not a.no_frozen:
            of = min(run("ours+froz", ce, None, s, EPS - amort)
                     for ce, s in itertools.product(cands, SPLITS))
            row += f"{100*of:>10.2f}%"
            best_ours = min(ours, of)
        gains.append(gbest / best_ours)
        print(row + f"{gbest/best_ours:>10.2f}x", flush=True)
        del c, cache
    print(f"\nvs google-best: min {min(gains):.2f}x, median {np.median(gains):.2f}x, "
          f"max {max(gains):.2f}x  over {len(gains)} queries")


if __name__ == "__main__":
    main()
