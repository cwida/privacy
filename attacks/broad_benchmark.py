"""Filterless per-PU/per-group bounds versus query-local Google-style bounds.

Arms, all at eps total per query:

  google-EM     query-local private upper bound and a privately selected C_u.
  filterless    one private upper bound learned from the unfiltered per-PU/per-group partial sums
                using a noised factor-4 histogram; C_u is still selected privately per query.

The workloads here are nonnegative SUM/COUNT queries, so L=0 follows from query semantics. U is a
bound on SUM(value) for one privacy unit in one SQL group. It is not a raw-row bound and not an l1
bound over all groups. Both arms aggregate by (PU, group), clamp that cell to [0,U], cap each PU to
C_u groups, and only then aggregate across PUs and add noise.

Scoring throughout: relative L1 over the TRUE uncapped key set, tau-suppressed groups charged as
released 0, every arm tuned over its own remaining free parameters.

    python3 attacks/broad_benchmark.py --datasets tpch --tpch-db /path/to/tpch.db
"""

import argparse

import numpy as np

from fineness_sweep import (DELTA, SPLITS, Cells, approx_bounds, google_values, tau, votes)
from em_cu import em_select

# name: (select body with {W} for the filter, filter, groups-are-in dataset)
Q = {
    "tpch SUM orders / month (1j)": ("""SELECT o_custkey pu, strftime(o_orderdate,'%Y-%m') g,
        sum(o_totalprice) t FROM tpch.orders JOIN tpch.customer ON c_custkey=o_custkey
        WHERE {W} GROUP BY 1,2""", "c_acctbal>=8000"),
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
    "tpch SUM price / mo|suppnat (4j)": ("""SELECT o_custkey pu,
        strftime(l_shipdate,'%Y-%m')||'|'||cast(s_nationkey as varchar) g,
        sum(l_extendedprice) t FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey JOIN tpch.supplier ON s_suppkey=l_suppkey
        JOIN tpch.nation ON n_nationkey=s_nationkey WHERE {W} GROUP BY 1,2""",
        "c_acctbal>=8000"),
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
    "syn SUM bounded / 12 groups": ("""SELECT pu, cast(j%12 as varchar) g, sum(bounded) t
        FROM synthetic_events WHERE {W} GROUP BY 1,2""", "pu%5=0"),
    "syn SUM log-skew / 120 groups": ("""SELECT pu, cast(g120 as varchar) g, sum(log_skew) t
        FROM synthetic_events WHERE {W} GROUP BY 1,2""", "pu%5=0"),
    "syn SUM Pareto / 1200 groups": ("""SELECT pu, cast(g1200 as varchar) g, sum(pareto) t
        FROM synthetic_events WHERE {W} GROUP BY 1,2""", "pu%5=0"),
    "syn SUM billionaire-excluded": ("""SELECT pu, cast(g120 as varchar) g,
        sum(CASE WHEN pu%1000=0 THEN 100*bounded ELSE bounded END) t
        FROM synthetic_events WHERE {W} GROUP BY 1,2""", "pu%1000<>0"),
    "syn SUM everyone+billionaires": ("""SELECT pu, cast(g120 as varchar) g,
        sum(CASE WHEN pu%1000=0 THEN 100*bounded ELSE bounded END) t
        FROM synthetic_events WHERE {W} GROUP BY 1,2""", "true"),
    "syn COUNT / 1200 groups": ("""SELECT pu, cast(g1200 as varchar) g, count(*) t
        FROM synthetic_events WHERE {W} GROUP BY 1,2""", "pu%5=0"),
    "stable SUM scalar / 40 PUs": ("""SELECT pu, 'all' g, sum(bounded) t
        FROM synthetic_events WHERE {W} GROUP BY 1,2""", "pu%250=0"),
    "stable SUM scalar / 80 PUs": ("""SELECT pu, 'all' g, sum(bounded) t
        FROM synthetic_events WHERE {W} GROUP BY 1,2""", "pu%125=0"),
    "stable SUM scalar / 200 PUs": ("""SELECT pu, 'all' g, sum(bounded) t
        FROM synthetic_events WHERE {W} GROUP BY 1,2""", "pu%50=0"),
    "stable SUM scalar / 500 PUs": ("""SELECT pu, 'all' g, sum(bounded) t
        FROM synthetic_events WHERE {W} GROUP BY 1,2""", "pu%20=0"),
    "stable SUM 4 groups / 40 PUs": ("""SELECT pu, cast(j%4 as varchar) g, sum(bounded) t
        FROM synthetic_events WHERE {W} GROUP BY 1,2""", "pu%250=0"),
    "stable SUM 4 groups / 80 PUs": ("""SELECT pu, cast(j%4 as varchar) g, sum(bounded) t
        FROM synthetic_events WHERE {W} GROUP BY 1,2""", "pu%125=0"),
    "stable SUM 4 groups / 200 PUs": ("""SELECT pu, cast(j%4 as varchar) g, sum(bounded) t
        FROM synthetic_events WHERE {W} GROUP BY 1,2""", "pu%50=0"),
    "stable SUM 4 groups / 500 PUs": ("""SELECT pu, cast(j%4 as varchar) g, sum(bounded) t
        FROM synthetic_events WHERE {W} GROUP BY 1,2""", "pu%20=0"),
}


def load(con, body, where):
    rows = con.execute(f"""WITH c AS ({body.format(W=where)})
        SELECT dense_rank() OVER (ORDER BY pu)-1 pid, dense_rank() OVER (ORDER BY g)-1 gid, g, t
        FROM c ORDER BY pid, gid""").fetchnumpy()
    c = Cells.__new__(Cells)
    c._init_from(rows["pid"].astype(np.int64), rows["gid"].astype(np.int64),
                 rows["t"].astype(np.float64))
    lab = list(np.unique(rows["g"]))
    return c, np.array(lab, dtype=object)


def per_pu_max(c):
    """Reduce all group partials to one bound-estimation record per privacy unit."""
    out = np.zeros(c.P, dtype=np.float64)
    np.maximum.at(out, c.pi, c.val)
    return out


def dp_filterless_upper(c, epsilon, rng, support, factor, n_bins=30):
    """Pure-DP upper bound from a full noised histogram of per-PU maximum group partials."""
    if support <= 0 or factor <= 1:
        raise ValueError("support must be positive and bin_factor must exceed one")
    if np.any(c.val < 0):
        raise ValueError("this benchmark currently covers nonnegative aggregates only")
    maxima = per_pu_max(c)
    levels = np.clip(np.floor(np.log(np.maximum(maxima, 1.0)) / np.log(factor)).astype(np.int64),
                     0, n_bins - 1)
    counts = np.bincount(levels, minlength=n_bins).astype(np.float64)
    noisy = counts + rng.laplace(0.0, 1.0 / epsilon, size=n_bins)
    supported = np.flatnonzero(noisy >= support)
    level = int(supported[-1]) if len(supported) else 0
    return factor ** (level + 1)


def containing_power_of_two(values):
    """Oracle/public upper edge that contains every observed nonnegative value."""
    return 2.0 ** np.ceil(np.log2(max(float(np.max(values)), 1.0)))


def build_synthetic(con):
    """Deterministic raw events: 20 group memberships and three rows per membership per PU."""
    con.execute("""CREATE TEMP TABLE synthetic_events AS
        SELECT pu, j, (pu*17+j*31)%120 g120, (pu*101+j*307)%1200 g1200,
               80.0+40.0*u bounded,
               exp(log(100.0)+3.0*(u-0.5)) log_skew,
               20.0/pow(greatest(1.0-u, 1e-6), 1.0/1.5) pareto
        FROM (SELECT pu, j, row_no,
                     (hash(pu*1009+j*97+row_no*17)%1000000)/1000001.0 u
              FROM range(10000) p(pu), range(20) q(j), range(3) r(row_no))""")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eps", type=float, default=1.0)
    ap.add_argument("--eps0", type=float, default=1.0)
    ap.add_argument("--n-queries", type=int, default=20)
    ap.add_argument("--support", type=int, default=40)
    ap.add_argument("--bin-factor", type=float, default=4.0)
    ap.add_argument("--p", type=float, default=0.95)
    ap.add_argument("--trials", type=int, default=2)
    ap.add_argument("--max-cells", type=int, default=6_000_000)
    ap.add_argument("--datasets", default="tpch,so,cb",
                    help="comma-separated dataset aliases to benchmark")
    ap.add_argument("--tpch-db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--so-db", default="/home/ila/Code/privacy/stackoverflow_dba_sqlstorm.db")
    ap.add_argument("--cb-db", default="/home/ila/Code/privacy/clickbench_micro.db")
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    datasets = {name.strip() for name in a.datasets.split(",") if name.strip()}
    paths = {"tpch": a.tpch_db, "so": a.so_db, "cb": a.cb_db, "syn": None, "stable": None}
    unknown = datasets - paths.keys()
    if unknown:
        raise ValueError(f"unknown datasets: {', '.join(sorted(unknown))}")
    if not datasets:
        raise ValueError("at least one dataset is required")
    for alias in sorted(datasets):
        if paths[alias]:
            con.execute(f"ATTACH '{paths[alias]}' AS {alias} (READ_ONLY)")
    if "syn" in datasets or "stable" in datasets:
        build_synthetic(con)
    EPS = a.eps
    amort = a.eps0 / a.n_queries
    if EPS <= 0.01 + amort:
        raise ValueError("eps must exceed 0.01 + eps0/n_queries")
    r = np.random.default_rng(99)
    EM = 0.01
    print(f"eps={EPS}, delta={DELTA:g}, {a.trials} trials; filterless bounds use a pure-DP factor-"
          f"{a.bin_factor:g} histogram with support={a.support} and eps0={a.eps0}, amortised over "
          f"N={a.n_queries} queries.")
    print("C_u costs eps=0.01 per query in both deployable arms.")
    print("g-wide* assumes an oracle/public bound containing every observed contribution; no value")
    print("is clipped, but noise is calibrated to that wide bound. It is not deployable as measured.\n")
    hdr = (f"{'query':<30}{'groups':>8}  {'U query':>18}{'U filtless':>12}"
           f"{'Google':>9}{'g-wide*':>9}{'filterless':>12}{'gain':>8}")
    print(hdr)
    print("-" * len(hdr))
    gains = []
    for name, (body, where) in Q.items():
        if name.split()[0] not in datasets:
            continue
        full, _ = load(con, body, "true")
        c, _ = load(con, body, where)
        if len(c.val) > a.max_cells:
            print(f"{name:<30}  SKIPPED: {len(c.val):,} cells")
            del c
            continue
        mk = int(c.k_u.max())
        cands = sorted(set([1, 2, 3, 5, 8, 13, 21, 34, 55, 89]) | {mk})
        ranks = [c.rank_random(r) for _ in range(a.trials)]
        vt = {(t, ce): votes(c, ranks[t], ce) for t in range(a.trials) for ce in cands}
        vote_unit = r.laplace(0.0, 1.0, size=(a.trials, c.K))
        value_unit = r.laplace(0.0, 1.0, size=(a.trials, c.K))
        query_bounds = {}
        filterless_bounds = [dp_filterless_upper(full, a.eps0, r, a.support, a.bin_factor)
                             for _ in range(a.trials)]
        for eb in sorted({s[0] for s in SPLITS}):
            for t in range(a.trials):
                eq = EPS - EM
                for cu in cands:
                    kept = ranks[t] < cu
                    query_bounds[(eb, t, eq, cu)] = approx_bounds(
                        c.val[kept], eq * eb, r, l1_sensitivity=cu,
                        p_success=1.0 - DELTA / 2, max_failure=DELTA / 2,
                        failure_bound=2.0 ** 46)

        def run_query_bound(cu, split, eq):
            eb, ee, ev = split
            thr = tau(eq * ee, DELTA / 2, cu)
            es = []
            for t in range(a.trials):
                U = query_bounds[(eb, t, eq, cu)]
                v = vt[(t, cu)]
                rel = (v + vote_unit[t] * cu / (eq * ee) >= thr) & (v > 0)
                tot = google_values(c, cu, U, ranks[t], False)
                out = np.where(rel, tot + value_unit[t] * cu * U / (eq * ev), 0.0)
                es.append(c.score(out))
            return float(np.mean(es))

        def run_filterless(cu, split):
            _, ee, ev = split
            weight = ee + ev
            ee, ev = ee / weight, ev / weight
            eq = EPS - EM - amort
            thr = tau(eq * ee, DELTA, cu)
            es = []
            for t in range(a.trials):
                U = filterless_bounds[t]
                v = vt[(t, cu)]
                rel = (v + vote_unit[t] * cu / (eq * ee) >= thr) & (v > 0)
                tot = google_values(c, cu, U, ranks[t], False)
                out = np.where(rel, tot + value_unit[t] * cu * U / (eq * ev), 0.0)
                es.append(c.score(out))
            return float(np.mean(es))

        def run_google_wide(cu, split):
            _, ee, ev = split
            weight = ee + ev
            ee, ev = ee / weight, ev / weight
            eq = EPS - EM
            U = containing_power_of_two(c.val)
            thr = tau(eq * ee, DELTA, cu)
            es = []
            for t in range(a.trials):
                v = vt[(t, cu)]
                rel = (v + vote_unit[t] * cu / (eq * ee) >= thr) & (v > 0)
                tot = google_values(c, cu, U, ranks[t], False)
                out = np.where(rel, tot + value_unit[t] * cu * U / (eq * ev), 0.0)
                es.append(c.score(out))
            return float(np.mean(es))

        pk = int(np.median([em_select(c.k_u, mk, a.p, EM / 5, 4 * EM / 5, 0.01, r)[0]
                            for _ in range(9)]))
        cu_em = min(cands, key=lambda y: abs(y - pk))
        gem = min(run_query_bound(cu_em, s, EPS - EM) for s in SPLITS)
        gwide = min(run_google_wide(cu_em, s) for s in SPLITS)
        filt = min(run_filterless(cu_em, s) for s in SPLITS)
        uq = float(np.median([query_bounds[(1 / 3, t, EPS - EM, cu_em)]
                              for t in range(a.trials)]))
        uf = float(np.median(filterless_bounds))
        gains.append(gem / filt)
        print(f"{name:<30}{c.K:>8,}  {uq:>18,.0f}{uf:>12,.0f}{100*gem:>8.2f}%"
              f"{100*gwide:>8.2f}%{100*filt:>11.2f}%{gem/filt:>7.2f}x",
              flush=True)
        del c, full, query_bounds
    print(f"\nfilterless vs google-EM: min {min(gains):.2f}x, median {np.median(gains):.2f}x, "
          f"max {max(gains):.2f}x  over {len(gains)} queries")


if __name__ == "__main__":
    main()
