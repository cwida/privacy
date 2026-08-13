"""What happens on StackOverflow and ClickBench? A structural profile of the tau floor.

The crude quantity used so far, n_g / max k_u, is a caricature. The exact one is per group: with
Laplace votes truncated to C_e = 1 (proven optimal), a PU spread over k_u groups contributes
1/k_u of a vote to each group it touches, so

    E[votes in g]  =  sum_{u in g} 1/k_u        (call it the EFFECTIVE vote count)

and g is releasable iff that clears tau(1) = 1 + ln(1/2 delta_eta)/eps_eta. Gaussian votes need no
truncation, so they compare the FULL count n_g against a threshold that grows only as sqrt(C_e).

The ratio n_g / eff_g is the harmonic-mean k_u of the group's members -- the exact factor Laplace
throws away and Gaussian keeps. This script measures it, so "does the result apply to this
dataset" becomes a number rather than a guess.

All queries are aggregate-only; nothing of row scale is materialised in Python.

    python3 attacks/dataset_profile.py
"""

import argparse

import duckdb
import numpy as np
from scipy.stats import norm

EPS_ETA, DELTA_ETA = 0.4, 5e-7


def tau_laplace(eps_eta=EPS_ETA, delta_eta=DELTA_ETA, ce=1):
    inner = 2.0 - 2.0 * (1.0 - delta_eta) ** (1.0 / ce)
    return np.inf if inner <= 0 else 1.0 - ce * np.log(inner) / eps_eta


def tau_gauss(ce, eps_eta=EPS_ETA, delta_eta=DELTA_ETA):
    sigma = np.sqrt(ce) * np.sqrt(2.0 * np.log(1.25 / (delta_eta / 2))) / eps_eta
    return 1.0 + sigma * norm.ppf(1.0 - (delta_eta / 2) / max(ce, 1))


QUERIES = {
    # name: (from/join, pu, group expr, where)
    "tpch month|nation": ("tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey "
                          "JOIN tpch.customer ON c_custkey=o_custkey", "o_custkey",
                          "strftime(l_shipdate,'%Y-%m')||'|'||cast(c_nationkey as varchar)",
                          "c_acctbal>=8000"),
    "tpch day":          ("tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey", "o_custkey",
                          "cast(l_shipdate as varchar)", "true"),
    "tpch week|nation":  ("tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey "
                          "JOIN tpch.customer ON c_custkey=o_custkey", "o_custkey",
                          "cast(date_trunc('week',l_shipdate) as varchar)||'|'||"
                          "cast(c_nationkey as varchar)", "c_acctbal>=8000"),
    "tpch day|region":   ("tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey "
                          "JOIN tpch.customer ON c_custkey=o_custkey "
                          "JOIN tpch.nation ON n_nationkey=c_nationkey", "o_custkey",
                          "cast(l_shipdate as varchar)||'|'||cast(n_regionkey as varchar)",
                          "c_acctbal>=8000"),
    "so posts|month":    ("so.Posts", "OwnerUserId", "strftime(CreationDate,'%Y-%m')",
                          "OwnerUserId IS NOT NULL"),
    "so posts|day":      ("so.Posts", "OwnerUserId", "cast(cast(CreationDate as date) as varchar)",
                          "OwnerUserId IS NOT NULL"),
    "so comments|month": ("so.Comments", "UserId", "strftime(CreationDate,'%Y-%m')",
                          "UserId IS NOT NULL"),
    "so comments|day":   ("so.Comments", "UserId", "cast(cast(CreationDate as date) as varchar)",
                          "UserId IS NOT NULL"),
    "so votes|month":    ("so.Votes", "UserId", "strftime(CreationDate,'%Y-%m')",
                          "UserId IS NOT NULL"),
    "so badges|month":   ("so.Badges", "UserId", "strftime(Date,'%Y-%m')", "UserId IS NOT NULL"),
    "cb hits|date":      ("cb.hits", "UserID", "cast(EventDate as varchar)", "UserID IS NOT NULL"),
    "cb hits|region":    ("cb.hits", "UserID", "cast(RegionID as varchar)", "UserID IS NOT NULL"),
    "cb hits|date|region": ("cb.hits", "UserID",
                            "cast(EventDate as varchar)||'|'||cast(RegionID as varchar)",
                            "UserID IS NOT NULL"),
    "cb hits|url":       ("cb.hits", "UserID", "cast(URLHash as varchar)", "UserID IS NOT NULL"),
}


def profile(con, frm, pu, gexpr, where):
    """One row per group; k_u and effective vote counts computed entirely in SQL."""
    return con.execute(f"""
        WITH cells AS (SELECT {pu} AS pu, {gexpr} AS g FROM {frm} WHERE {where} GROUP BY 1,2),
             ku AS (SELECT pu, count(*) AS k FROM cells GROUP BY 1),
             gv AS (SELECT c.g, count(*) AS n_g, sum(1.0/k.k) AS eff
                    FROM cells c JOIN ku k ON k.pu=c.pu GROUP BY 1)
        SELECT (SELECT count(*) FROM gv),
               (SELECT count(*) FROM ku),
               (SELECT median(k) FROM ku), (SELECT quantile_cont(k,0.9) FROM ku),
               (SELECT max(k) FROM ku),
               (SELECT median(n_g) FROM gv), (SELECT median(eff) FROM gv),
               (SELECT sum(n_g) FROM gv)
    """).fetchone()


def releasable(con, frm, pu, gexpr, where, ce_max):
    """Tune C_e SEPARATELY for each arm. With truncation to C_e a group's expected vote count is
    sum_u min(C_e,k_u)/k_u, which both arms share; only the threshold differs. Pinning C_e = k_max
    (i.e. 'no truncation') is catastrophic for Gaussian on heavy-tailed k_u, since its sensitivity
    is sqrt(C_e) -- driven by the tail, not the typical PU."""
    grid = sorted({1, 2, 3, 5, 8, 13, 21, 34, 55, 89, max(int(ce_max), 1)})
    cols = ", ".join(f"count(*) FILTER (WHERE eff{i} >= {tau_laplace(ce=ce):.6f}) AS l{i}, "
                     f"count(*) FILTER (WHERE eff{i} >= {tau_gauss(ce):.6f}) AS g{i}"
                     for i, ce in enumerate(grid))
    effs = ", ".join(f"sum(least({ce}, k.k)*1.0/k.k) AS eff{i}" for i, ce in enumerate(grid))
    row = con.execute(f"""
        WITH cells AS (SELECT {pu} AS pu, {gexpr} AS g FROM {frm} WHERE {where} GROUP BY 1,2),
             ku AS (SELECT pu, count(*) AS k FROM cells GROUP BY 1),
             gv AS (SELECT c.g, {effs} FROM cells c JOIN ku k ON k.pu=c.pu GROUP BY c.g)
        SELECT {cols}, count(*) FROM gv
    """).fetchone()
    lap = [(row[2 * i], grid[i]) for i in range(len(grid))]
    gau = [(row[2 * i + 1], grid[i]) for i in range(len(grid))]
    return max(lap), max(gau), row[-1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--queries", default=",".join(QUERIES))
    a = ap.parse_args()
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    for alias, path in (("so", "stackoverflow_dba_sqlstorm.db"), ("cb", "clickbench_micro.db"),
                        ("tpch", "tpch_sass_sf10.db")):
        con.execute(f"ATTACH '/home/ila/Code/privacy/{path}' AS {alias} (READ_ONLY)")

    t_lap = tau_laplace()
    print(f"eps_eta={EPS_ETA}, delta_eta={DELTA_ETA:g}  ->  Laplace floor tau(C_e=1) = {t_lap:.1f}")
    print("A group is releasable under Laplace iff its EFFECTIVE vote count sum_u 1/k_u clears")
    print("that floor; under Gaussian iff its FULL count n_g clears a sqrt(C_e)-scaled threshold.")
    print("n_g/eff is the harmonic-mean k_u of the group's members -- what Laplace throws away.\n")
    hdr = (f"{'query':<22}{'groups':>8}{'PUs':>10}{'k_u med':>8}{'p90':>6}{'max':>6}"
           f"{'n_g med':>9}{'eff med':>9}{'n_g/eff':>8}")
    print(hdr)
    print("-" * len(hdr))
    rows = {}
    for name in a.queries.split(","):
        frm, pu, gexpr, where = QUERIES[name]
        try:
            k, p, km, k9, kx, ng, eff, tot = profile(con, frm, pu, gexpr, where)
        except Exception as e:
            print(f"{name:<22}  ERROR {str(e)[:44]}")
            continue
        rows[name] = (frm, pu, gexpr, where, kx)
        print(f"{name:<22}{k:>8,}{p:>10,}{km:>8.0f}{k9:>6.0f}{kx:>6.0f}"
              f"{ng:>9,.0f}{eff:>9,.1f}{ng/max(eff,1e-9):>8.1f}", flush=True)

    print(f"\nBoth arms tune C_e separately (the vote count sum_u min(C_e,k_u)/k_u is shared;")
    print("only the threshold differs). 'best C_e' is each arm's own optimum.\n")
    hdr2 = (f"{'query':<22}{'Laplace rel':>13}{'C_e':>5}{'Gaussian rel':>14}{'C_e':>5}"
            f"{'of':>9}{'gain':>7}")
    print(hdr2)
    print("-" * len(hdr2))
    for name, (frm, pu, gexpr, where, kx) in rows.items():
        (rl, cl), (rg, cg), tot = releasable(con, frm, pu, gexpr, where, int(kx))
        g = f"{rg/max(rl,1):.2f}x" if rl else ("inf" if rg else "-")
        print(f"{name:<22}{rl:>13,}{cl:>5}{rg:>14,}{cg:>5}{tot:>9,}{g:>7}", flush=True)
    print("\nWhere n_g/eff is near 1 the two mechanisms see the same counts and Gaussian cannot")
    print("help -- the geometry gain needs privacy units that genuinely spread across groups.")


if __name__ == "__main__":
    main()
