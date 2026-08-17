"""Section 1 across datasets: does EM-selected C_u beat what an analyst would pick by hand?

Google DP as published does not choose C_u for you -- `max_partitions_contributed` is a required
input the analyst supplies. So the honest comparison for her section 1 is not against "Google DP",
it is against the VALUES A HUMAN WOULD PLAUSIBLY SUPPLY:

  C_u = 1     the safe-looking default; also what a cautious analyst picks when unsure
  C_u = 10    a round number
  C_u = 100   a generous round number
  C_u = max   the schema-implied maximum, when it is known at all
  EM          her mechanism, paying eps_N + eps_C = 0.01 out of the query's own budget

Reported against each query's own error-optimal C_v, so 1.00x means "as good as an oracle that
already knew the best bound".

    python3 attacks/em_cu_vs_manual.py
"""

import argparse
import itertools

import numpy as np

from fineness_sweep import (DELTA, SPLITS, Cells, approx_bounds, google_values, tau, votes)
from em_cu import em_select

QUERIES = {
    "tpch month": ("""SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m') g, sum(l_extendedprice) t
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE c_acctbal>=8000 GROUP BY 1,2"""),
    "tpch month|nation": ("""SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m')||'|'||
        cast(c_nationkey as varchar) g, sum(l_extendedprice) t
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE c_acctbal>=8000 GROUP BY 1,2"""),
    "tpch day (9500)": ("""SELECT o_custkey pu, cast(l_shipdate as varchar) g,
        sum(l_extendedprice) t FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE c_acctbal>=9500 GROUP BY 1,2"""),
    "tpch month|prio (9500)": ("""SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m')||'|'||
        o_orderpriority g, sum(l_extendedprice) t
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE c_acctbal>=9500 GROUP BY 1,2"""),
    "so posts|month": ("""SELECT OwnerUserId pu, strftime(CreationDate,'%Y-%m') g, count(*) t
        FROM so.Posts WHERE OwnerUserId IS NOT NULL GROUP BY 1,2"""),
    "so posts|day": ("""SELECT OwnerUserId pu, cast(cast(CreationDate as date) as varchar) g,
        count(*) t FROM so.Posts WHERE OwnerUserId IS NOT NULL GROUP BY 1,2"""),
    "so comments|month": ("""SELECT UserId pu, strftime(CreationDate,'%Y-%m') g, count(*) t
        FROM so.Comments WHERE UserId IS NOT NULL GROUP BY 1,2"""),
    "cb hits|region": ("""SELECT UserID pu, cast(RegionID as varchar) g, count(*) t
        FROM cb.hits WHERE UserID IS NOT NULL GROUP BY 1,2"""),
    "cb hits|date|region": ("""SELECT UserID pu, cast(EventDate as varchar)||'|'||
        cast(RegionID as varchar) g, count(*) t FROM cb.hits WHERE UserID IS NOT NULL GROUP BY 1,2"""),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eps", type=float, default=1.0)
    ap.add_argument("--eps-em", type=float, default=0.01, help="total eps_N + eps_C")
    ap.add_argument("--p", type=float, default=0.7)
    ap.add_argument("--trials", type=int, default=2)
    ap.add_argument("--max-cells", type=int, default=6_000_000)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    for al, p in (("tpch", "tpch_sass_sf10.db"), ("so", "stackoverflow_dba_sqlstorm.db"),
                  ("cb", "clickbench_micro.db")):
        con.execute(f"ATTACH '/home/ila/Code/privacy/{p}' AS {al} (READ_ONLY)")
    EPS = a.eps
    r = np.random.default_rng(99)
    CVS = [1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144]

    print(f"eps={EPS}, EM pays eps_N+eps_C={a.eps_em} from the query's own budget, p={a.p}")
    print("penalty = error at that C_u / error at the query's own best C_v (1.00x = oracle)\n")
    hdr = (f"{'query':<22}{'k_u p50':>8}{'p70':>5}{'max':>5}{'best':>5}{'EM':>4}"
           f"{'  C_u=1':>9}{'C_u=10':>8}{'C_u=100':>9}{'C_u=max':>9}{'EM':>8}")
    print(hdr)
    print("-" * len(hdr))
    for name, sql in QUERIES.items():
        rows = con.execute(f"""WITH c AS ({sql})
            SELECT dense_rank() OVER (ORDER BY pu)-1 pid, dense_rank() OVER (ORDER BY g)-1 gid, t
            FROM c""").fetchnumpy()
        if len(rows["t"]) > a.max_cells:
            print(f"{name:<22}  SKIPPED: {len(rows['t']):,} cells")
            continue
        c = Cells.__new__(Cells)
        c._init_from(rows["pid"].astype(np.int64), rows["gid"].astype(np.int64),
                     rows["t"].astype(np.float64))
        mk = int(c.k_u.max())
        cands = sorted(set(CVS) | {mk})
        ranks = [c.rank_random(r) for _ in range(a.trials)]
        vt = {(t, ce): votes(c, ranks[t], ce) for t in range(a.trials) for ce in cands}
        cache = {}
        for eb in sorted({s[0] for s in SPLITS}):
            for t in range(a.trials):
                U = approx_bounds(c.val, EPS * eb, r)
                cache[(eb, t)] = (U, {cv: google_values(c, cv, U, ranks[t], False)
                                      for cv in cands})

        def err(cv, eq):
            best = None
            for ce, (eb, ee, ev) in itertools.product(cands, SPLITS):
                thr = tau(eq * ee, DELTA, ce)
                es = []
                for t in range(a.trials):
                    U, g = cache[(eb, t)]
                    v = vt[(t, ce)]
                    rel = (v + r.laplace(0, ce / (eq * ee), size=c.K) >= thr) & (v > 0)
                    es.append(c.score(np.where(rel, g[cv] + r.laplace(
                        0, cv * U / (eq * ev), size=c.K), 0.0)))
                e = float(np.mean(es))
                best = e if best is None or e < best else best
            return best

        def near(x):
            return min(cands, key=lambda y: abs(y - x))
        per = {cv: err(cv, EPS) for cv in cands}
        bcv = min(per, key=per.get)
        pick = int(np.median([em_select(c.k_u, mk, a.p, a.eps_em / 5, 4 * a.eps_em / 5, 0.01, r)[0]
                              for _ in range(9)]))
        e_em = err(near(pick), EPS - a.eps_em)
        manual = {m: per[near(m)] for m in (1, 10, 100)}
        manual["max"] = per[mk]
        print(f"{name:<22}{np.quantile(c.k_u,.5):>8.0f}{np.quantile(c.k_u,.7):>5.0f}{mk:>5}"
              f"{bcv:>5}{pick:>4}"
              f"{manual[1]/per[bcv]:>8.2f}x{manual[10]/per[bcv]:>7.2f}x"
              f"{manual[100]/per[bcv]:>8.2f}x{manual['max']/per[bcv]:>8.2f}x"
              f"{e_em/per[bcv]:>7.2f}x", flush=True)
        del c, cache
    print("\nA human supplying C_u has to guess; the EM does not. The columns show what each guess")
    print("costs relative to knowing the answer.")


if __name__ == "__main__":
    main()
