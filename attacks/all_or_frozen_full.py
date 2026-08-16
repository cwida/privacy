"""Comprehensive evaluation of Dandan's All-or-Frozen thresholding (draft 15 Aug, section 2).

The question: can the all-or-nothing rule ever actually fire? Release the query-specific group set
iff EVERY group passes a certification test, else fall back to the frozen set G_fix.

Three variants:
  as-specified   certify by max over B=64 bins of the per-group VALUE histogram already built for
                 automatic bounding, sensitivity C_u, noise Laplace(C_u/eps_B).
  +repair A      certify by a DEDICATED per-group distinct-PU count at C_e=1, sensitivity 1.
                 Stops paying the C_u sensitivity and the max-over-64-bins term.
  +repair A+D    ... and take the AND per BLOCK, where a block is a value of a grouping-key
                 component functionally determined by the PU (a customer has one nation). Every
                 group a PU can create lies in one block, so the AND need only cover that block and
                 a tiny group elsewhere cannot veto it.

The decisive statistic needs no simulation: a group whose true support is below the threshold fails
ALWAYS, so it vetoes its scope forever. Hence "min group support vs tau" decides the whole thing,
and the Monte Carlo only confirms it.

Everything is aggregated in SQL to per-group rows, so cost is one pass per query.

    python3 attacks/all_or_frozen_full.py
"""

import argparse

import numpy as np

B_BINS = 64
DELTA = 1e-6


def tau_af(b, delta, B=B_BINS):
    """Her exact threshold: inf{tau>=1 : (1-.5e^{-(tau-1)/b})(1-.5e^{-tau/b})^{B-1} >= 1-delta}."""
    lo, hi = 1.0, 1.0 + 400.0 * b
    for _ in range(300):
        mid = (lo + hi) / 2
        p = (1 - 0.5 * np.exp(-(mid - 1) / b)) * (1 - 0.5 * np.exp(-mid / b)) ** (B - 1)
        if p >= 1 - delta:
            hi = mid
        else:
            lo = mid
    return hi


def tau_count(eps_eta, delta, ce=1):
    """Threshold for a dedicated distinct-PU count: one statistic, no bins."""
    inner = 2.0 - 2.0 * (1.0 - delta) ** (1.0 / ce)
    return np.inf if inner <= 0 else 1.0 - ce * np.log(inner) / eps_eta


# (label, sql producing g / n_pu / maxbin / mass / blk, note)
QUERIES = {
    "tpch month": ("""
        WITH cells AS (SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m') g,
                       sum(l_extendedprice) t FROM tpch.lineitem
                       JOIN tpch.orders ON o_orderkey=l_orderkey
                       JOIN tpch.customer ON c_custkey=o_custkey
                       WHERE c_acctbal>=8000 GROUP BY 1,2),
             bins AS (SELECT g, floor(log(greatest(t,1))/log(2)) b, count(*) c
                      FROM cells GROUP BY 1,2)
        SELECT c.g, count(*) n_pu, max(b.c) maxbin, sum(c.t) mass, NULL blk
        FROM cells c JOIN bins b ON b.g=c.g GROUP BY c.g""", None),
    "tpch month|nation": ("""
        WITH cells AS (SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m')||'|'||
                       cast(c_nationkey as varchar) g, c_nationkey nk, sum(l_extendedprice) t
                       FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
                       JOIN tpch.customer ON c_custkey=o_custkey
                       WHERE c_acctbal>=8000 GROUP BY 1,2,3),
             bins AS (SELECT g, floor(log(greatest(t,1))/log(2)) b, count(*) c
                      FROM cells GROUP BY 1,2)
        SELECT c.g, count(*) n_pu, max(b.c) maxbin, sum(c.t) mass, any_value(c.nk) blk
        FROM cells c JOIN bins b ON b.g=c.g GROUP BY c.g""", "nation"),
    "tpch month|priority": ("""
        WITH cells AS (SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m')||'|'||o_orderpriority g,
                       sum(l_extendedprice) t FROM tpch.lineitem
                       JOIN tpch.orders ON o_orderkey=l_orderkey
                       JOIN tpch.customer ON c_custkey=o_custkey
                       WHERE c_acctbal>=8000 GROUP BY 1,2),
             bins AS (SELECT g, floor(log(greatest(t,1))/log(2)) b, count(*) c
                      FROM cells GROUP BY 1,2)
        SELECT c.g, count(*) n_pu, max(b.c) maxbin, sum(c.t) mass, NULL blk
        FROM cells c JOIN bins b ON b.g=c.g GROUP BY c.g""", None),
    "tpch day|region": ("""
        WITH cells AS (SELECT o_custkey pu, cast(l_shipdate as varchar)||'|'||
                       cast(n_regionkey as varchar) g, n_regionkey rk, sum(l_extendedprice) t
                       FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
                       JOIN tpch.customer ON c_custkey=o_custkey
                       JOIN tpch.nation ON n_nationkey=c_nationkey
                       WHERE c_acctbal>=8000 GROUP BY 1,2,3),
             bins AS (SELECT g, floor(log(greatest(t,1))/log(2)) b, count(*) c
                      FROM cells GROUP BY 1,2)
        SELECT c.g, count(*) n_pu, max(b.c) maxbin, sum(c.t) mass, any_value(c.rk) blk
        FROM cells c JOIN bins b ON b.g=c.g GROUP BY c.g""", "region"),
    "so posts|month": ("""
        WITH cells AS (SELECT OwnerUserId pu, strftime(CreationDate,'%Y-%m') g, count(*) t
                       FROM so.Posts WHERE OwnerUserId IS NOT NULL GROUP BY 1,2),
             bins AS (SELECT g, floor(log(greatest(t,1))/log(2)) b, count(*) c
                      FROM cells GROUP BY 1,2)
        SELECT c.g, count(*) n_pu, max(b.c) maxbin, sum(c.t) mass, NULL blk
        FROM cells c JOIN bins b ON b.g=c.g GROUP BY c.g""", None),
    "so comments|month": ("""
        WITH cells AS (SELECT UserId pu, strftime(CreationDate,'%Y-%m') g, count(*) t
                       FROM so.Comments WHERE UserId IS NOT NULL GROUP BY 1,2),
             bins AS (SELECT g, floor(log(greatest(t,1))/log(2)) b, count(*) c
                      FROM cells GROUP BY 1,2)
        SELECT c.g, count(*) n_pu, max(b.c) maxbin, sum(c.t) mass, NULL blk
        FROM cells c JOIN bins b ON b.g=c.g GROUP BY c.g""", None),
    "cb hits|region": ("""
        WITH cells AS (SELECT UserID pu, cast(RegionID as varchar) g, count(*) t
                       FROM cb.hits WHERE UserID IS NOT NULL GROUP BY 1,2),
             bins AS (SELECT g, floor(log(greatest(t,1))/log(2)) b, count(*) c
                      FROM cells GROUP BY 1,2)
        SELECT c.g, count(*) n_pu, max(b.c) maxbin, sum(c.t) mass, NULL blk
        FROM cells c JOIN bins b ON b.g=c.g GROUP BY c.g""", None),
    "cb hits|date|region": ("""
        WITH cells AS (SELECT UserID pu, cast(EventDate as varchar)||'|'||
                       cast(RegionID as varchar) g, count(*) t
                       FROM cb.hits WHERE UserID IS NOT NULL GROUP BY 1,2),
             bins AS (SELECT g, floor(log(greatest(t,1))/log(2)) b, count(*) c
                      FROM cells GROUP BY 1,2)
        SELECT c.g, count(*) n_pu, max(b.c) maxbin, sum(c.t) mass, NULL blk
        FROM cells c JOIN bins b ON b.g=c.g GROUP BY c.g""", None),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eps-b", type=float, default=0.5, help="budget for the bounding histogram")
    ap.add_argument("--eps-eta", type=float, default=0.4, help="budget for the dedicated count")
    ap.add_argument("--cu", type=int, default=37, help="C_u for the histogram sensitivity")
    ap.add_argument("--trials", type=int, default=200)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    for al, p in (("tpch", "tpch_sass_sf10.db"), ("so", "stackoverflow_dba_sqlstorm.db"),
                  ("cb", "clickbench_micro.db")):
        con.execute(f"ATTACH '/home/ila/Code/privacy/{p}' AS {al} (READ_ONLY)")
    r = np.random.default_rng(7)

    b_hist = a.cu / a.eps_b
    t_hist = tau_af(b_hist, DELTA)
    t_cnt = tau_count(a.eps_eta, DELTA)
    print(f"eps_B={a.eps_b}, C_u={a.cu} -> histogram noise b={b_hist:,.1f}, tau_AF={t_hist:,.0f}")
    print(f"eps_eta={a.eps_eta}, C_e=1  -> count noise b={1/a.eps_eta:,.1f}, tau={t_cnt:,.1f}\n")
    hdr = (f"{'query':<22}{'groups':>8}{'min sup':>9}{'median':>9}"
           f"{'as-spec':>9}{'+A':>7}{'+A+D':>8}{'mass(A+D)':>11}  veto")
    print(hdr)
    print("-" * (len(hdr) + 12))
    for name, (sql, blkname) in QUERIES.items():
        try:
            rows = con.execute(sql).fetchall()
        except Exception as e:
            print(f"{name:<22}  ERROR {str(e)[:50]}")
            continue
        n_pu = np.array([x[1] for x in rows], float)
        maxbin = np.array([x[2] for x in rows], float)
        mass = np.abs(np.array([x[3] for x in rows], float))
        blk = (np.array([x[4] for x in rows]) if blkname else np.zeros(len(rows), int))
        K = len(rows)

        def mc(stat, thr, scale, blocks):
            hits, mfrac = 0, []
            ub = np.unique(blocks)
            for _ in range(a.trials):
                p = stat + r.laplace(0, scale, size=K) >= thr
                ok = [u for u in ub if p[blocks == u].all()]
                hits += len(ok) / len(ub)
                mfrac.append(mass[np.isin(blocks, ok)].sum() / max(mass.sum(), 1e-9))
            return hits / a.trials, float(np.mean(mfrac))
        one = np.zeros(K, int)
        spec, _ = mc(maxbin, t_hist, b_hist, one)
        ra, _ = mc(n_pu, t_cnt, 1 / a.eps_eta, one)
        rad, mad = mc(n_pu, t_cnt, 1 / a.eps_eta, blk)
        # the veto: scopes containing a group whose support is hopelessly below tau
        ub = np.unique(blk)
        vetoed = sum(1 for u in ub if n_pu[blk == u].min() < t_cnt)
        note = (f"{vetoed}/{len(ub)} blocks have a group < tau" if blkname
                else ("min < tau -> never" if n_pu.min() < t_cnt else "min >= tau"))
        print(f"{name:<22}{K:>8,}{n_pu.min():>9,.0f}{np.median(n_pu):>9,.0f}"
              f"{100*spec:>8.1f}%{100*ra:>6.1f}%{100*rad:>7.1f}%{100*mad:>10.1f}%  {note}",
              flush=True)
    print("\n'as-spec' / '+A' / '+A+D' = fraction of scopes where EVERY group passes, so the")
    print("compact group set may be used. 'mass(A+D)' = share of the true total in those scopes.")


if __name__ == "__main__":
    main()
