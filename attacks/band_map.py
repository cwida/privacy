"""How wide is the band where Gaussian votes beat Laplace votes, and how much of a key set is in it?

A group is released under Laplace votes (C_e = 1, proven optimal) iff its EFFECTIVE vote count
sum_{u in g} 1/k_u clears tau_L(1); under Gaussian votes iff sum_u min(C_e,k_u)/k_u clears a
threshold growing as sqrt(C_e). Writing k_h = n_g / eff for the harmonic-mean k_u of a group's
members, the two conditions are

    Laplace :  n_g  >  k_h * tau_L(1)
    Gaussian:  n_g  >  thr_G(C*)                       (at C* = the tuned truncation)

so the groups that FLIP -- the entire source of the gain -- are exactly those with

    thr_G(C*)  <  n_g  <  k_h * tau_L(1)

a band whose multiplicative width is  k_h * tau_L(1) / thr_G(C*),  typically ~2x. Everything
above the band is released by both arms; everything below by neither. Both thresholds scale as
1/eps_eta, so changing eps SLIDES the band along n_g without widening it.

Consequences this script measures:
  * the gain is a resonance in eps, not a plateau: outside a ~2x window of eps it collapses;
  * the gain equals the share of the key set (and of the true mass) sitting in a 2x-wide band,
  * so it is largest exactly when all groups are the same size -- which is TPC-H, and is not
    a property of real data.

    python3 attacks/band_map.py
"""

import argparse

import duckdb
import numpy as np
from scipy.stats import norm

DELTA = 1e-6
CE_GRID = [1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144]

QUERIES = {
    "tpch week|nation": ("tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey JOIN "
                         "tpch.customer ON c_custkey=o_custkey", "o_custkey",
                         "cast(date_trunc('week',l_shipdate) as varchar)||'|'||"
                         "cast(c_nationkey as varchar)", "c_acctbal>=8000", "l_extendedprice"),
    "tpch day|region": ("tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey JOIN "
                        "tpch.customer ON c_custkey=o_custkey JOIN tpch.nation ON "
                        "n_nationkey=c_nationkey", "o_custkey",
                        "cast(l_shipdate as varchar)||'|'||cast(n_regionkey as varchar)",
                        "c_acctbal>=8000", "l_extendedprice"),
    "tpch month|nation": ("tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey JOIN "
                          "tpch.customer ON c_custkey=o_custkey", "o_custkey",
                          "strftime(l_shipdate,'%Y-%m')||'|'||cast(c_nationkey as varchar)",
                          "c_acctbal>=8000", "l_extendedprice"),
    "so posts|month": ("so.Posts", "OwnerUserId", "strftime(CreationDate,'%Y-%m')",
                       "OwnerUserId IS NOT NULL", "1"),
    "so posts|day": ("so.Posts", "OwnerUserId", "cast(cast(CreationDate as date) as varchar)",
                     "OwnerUserId IS NOT NULL", "1"),
    "so posts|month|tag": ("so.Posts", "OwnerUserId",
                           "strftime(CreationDate,'%Y-%m')||'|'||coalesce(Tags,'-')",
                           "OwnerUserId IS NOT NULL", "1"),
    "so comments|month": ("so.Comments", "UserId", "strftime(CreationDate,'%Y-%m')",
                          "UserId IS NOT NULL", "1"),
    "cb hits|date": ("cb.hits", "UserID", "cast(EventDate as varchar)", "UserID IS NOT NULL", "1"),
    "cb hits|region": ("cb.hits", "UserID", "cast(RegionID as varchar)", "UserID IS NOT NULL",
                       "1"),
    "cb hits|date|region": ("cb.hits", "UserID",
                            "cast(EventDate as varchar)||'|'||cast(RegionID as varchar)",
                            "UserID IS NOT NULL", "1"),
}


def tau_lap(eps_eta, delta_eta, ce=1):
    inner = 2.0 - 2.0 * (1.0 - delta_eta) ** (1.0 / ce)
    return np.inf if inner <= 0 else 1.0 - ce * np.log(inner) / eps_eta


def thr_gauss(ce, eps_eta, delta):
    sigma = np.sqrt(ce) * np.sqrt(2.0 * np.log(1.25 / (delta / 2))) / eps_eta
    return 1.0 + sigma * norm.ppf(1.0 - (delta / 2) / max(ce, 1))


def fetch(con, name):
    frm, pu, gexpr, where, val = QUERIES[name]
    effs = ", ".join(f"sum(least({ce}, k.k)*1.0/k.k) AS e{i}" for i, ce in enumerate(CE_GRID))
    rows = con.execute(f"""
        WITH cells AS (SELECT {pu} AS pu, {gexpr} AS g, sum({val}) AS m
                       FROM {frm} WHERE {where} GROUP BY 1,2),
             ku AS (SELECT pu, count(*) AS k FROM cells GROUP BY 1),
             gv AS (SELECT c.g, count(*) AS n_g, sum(c.m) AS mass, {effs}
                    FROM cells c JOIN ku k ON k.pu=c.pu GROUP BY c.g)
        SELECT * FROM gv""").fetchnumpy()
    n_g = rows["n_g"].astype(float)
    mass = rows["mass"].astype(float)
    eff = np.stack([rows[f"e{i}"].astype(float) for i in range(len(CE_GRID))])
    return n_g, mass, eff


def released(eff, n_g, eps_eta, arm):
    """Best release count over the C_e grid, and the C_e that achieves it (mass-weighted too)."""
    best = (-1, None, None)
    for i, ce in enumerate(CE_GRID):
        thr = tau_lap(eps_eta, DELTA, ce) if arm == "lap" else thr_gauss(ce, eps_eta, DELTA)
        m = eff[i] >= thr
        if m.sum() > best[0]:
            best = (int(m.sum()), ce, m)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--queries", default=",".join(QUERIES))
    ap.add_argument("--eps-eta", type=float, default=0.4)
    a = ap.parse_args()
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    for al, p in (("so", "stackoverflow_dba_sqlstorm.db"), ("cb", "clickbench_micro.db"),
                  ("tpch", "tpch_sass_sf10.db")):
        con.execute(f"ATTACH '/home/ila/Code/privacy/{p}' AS {al} (READ_ONLY)")

    print("Group-size dispersion vs the width of the flip band (both at eps_eta = "
          f"{a.eps_eta}, delta = {DELTA:g}).")
    print("'band' = k_h*tau_L(1) / thr_G(C*): the multiplicative window in n_g where Gaussian")
    print("releases and Laplace does not. 'n_g p95/p5' = how far the key set spreads. When the")
    print("spread exceeds the band, only part of the key set can ever flip.\n")
    hdr = (f"{'query':<22}{'groups':>8}{'k_h med':>9}{'n_g p5':>9}{'p50':>9}{'p95':>10}"
           f"{'p95/p5':>9}{'band':>7}{'flip%':>7}{'flip mass%':>11}")
    print(hdr)
    print("-" * len(hdr))
    data = {}
    for name in a.queries.split(","):
        n_g, mass, eff = fetch(con, name)
        data[name] = (n_g, mass, eff)
        k_h = np.median(n_g / np.maximum(eff[0], 1e-12))
        rl, cl, ml = released(eff, n_g, a.eps_eta, "lap")
        rg, cg, mg = released(eff, n_g, a.eps_eta, "gauss")
        band = k_h * tau_lap(a.eps_eta, DELTA) / thr_gauss(cg, a.eps_eta, DELTA)
        flip = mg & ~ml
        q = np.quantile(n_g, [.05, .5, .95])
        print(f"{name:<22}{len(n_g):>8,}{k_h:>9.1f}{q[0]:>9,.0f}{q[1]:>9,.0f}{q[2]:>10,.0f}"
              f"{q[2]/max(q[0],1e-9):>9.1f}{band:>7.1f}{100*flip.mean():>6.1f}%"
              f"{100*mass[flip].sum()/mass.sum():>10.1f}%", flush=True)

    print("\nSliding the band with eps: released groups (Laplace -> Gaussian), whole-eps sweep.")
    print("The peak is narrow because both thresholds scale as 1/eps_eta together.\n")
    epss = [0.05, 0.1, 0.2, 0.4, 0.8, 1.6, 3.2]
    print(f"{'query':<22}" + "".join(f"{e:>13}" for e in epss))
    print("-" * (22 + 13 * len(epss)))
    for name, (n_g, mass, eff) in data.items():
        cells = []
        for e in epss:
            rl, _, ml = released(eff, n_g, e, "lap")
            rg, _, mg = released(eff, n_g, e, "gauss")
            K = len(n_g)
            cells.append(f"{100*rl/K:>5.0f}/{100*rg/K:<4.0f}%")
        print(f"{name:<22}" + "".join(f"{c:>13}" for c in cells), flush=True)
    print("\n(each cell: % of key set released by Laplace / by Gaussian, both tuned over C_e)")


if __name__ == "__main__":
    main()
