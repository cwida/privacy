"""Dandan's 18 Aug questions on All-or-Frozen, and two corrections to my own earlier experiment.

Her Q4 is correct and identifies a real error. My earlier "repair A" scored a dedicated distinct-PU
count with noise calibrated to C_e = 1, but never truncated any PU to C_e groups. Without
truncation one PU affects up to k_u group counts, so the sensitivity is k_u and not 1. Corrected
here: each PU votes in at most C_e of its groups, so group g's count is |{u : u votes in g}| with
expectation sum_{u in g} min(C_e, k_u)/k_u.

A second, independent bug surfaced while checking her Q1. The earlier profile computed support with

    FROM cells c JOIN bins b ON b.g = c.g GROUP BY c.g   -- count(*) as n_pu

which is a fan-out join: `cells` has one row per (PU, group) and `bins` one row per (group, bin), so
count(*) returns n_g * (occupied bins), inflating support by ~11x on TPC-H. Both the reported
support and the "veto" analysis derived from it were wrong.

The three questions this answers:

  Q1  Why do almost no queries pass now? Decomposed into the two multipliers: the C_u sensitivity
      on the reused histogram, and the fact that the statistic is a max BIN count rather than the
      group's support.
  Q3  Restrict AllPass to G_Q intersect G_fix -- test only the query groups that are also frozen,
      release that compact subset if all pass, else fall back to the full G_fix.
  Q4  Truncation, as above.

    python3 attacks/all_or_frozen_v2.py
"""

import argparse

import numpy as np

import fineness_sweep as F

B_BINS = 64
DELTA = 1e-6


def tau_af(b, delta, B=B_BINS):
    """Her exact All-or-Frozen threshold: the AND needs only rho_tau <= delta."""
    lo, hi = 1.0, 1.0 + 400.0 * b
    for _ in range(300):
        mid = (lo + hi) / 2
        p = (1 - 0.5 * np.exp(-(mid - 1) / b)) * (1 - 0.5 * np.exp(-mid / b)) ** (B - 1)
        if p >= 1 - delta:
            hi = mid
        else:
            lo = mid
    return hi


def tau_count(eps_eta, delta, ce):
    """Wilson threshold for a dedicated distinct-PU count truncated to C_e groups per PU."""
    inner = 2.0 - 2.0 * (1.0 - delta) ** (1.0 / ce)
    return np.inf if inner <= 0 else 1.0 - ce * np.log(inner) / eps_eta


def load(con, sql, gfix_sql=None):
    """Cells for the query, plus the frozen key set from the filterless version if asked."""
    rows = con.execute(f"""WITH c AS ({sql})
        SELECT dense_rank() OVER (ORDER BY pu)-1 pid, dense_rank() OVER (ORDER BY g)-1 gid, g, t
        FROM c""").fetchnumpy()
    c = F.Cells.__new__(F.Cells)
    c._init_from(rows["pid"].astype(np.int64), rows["gid"].astype(np.int64),
                 rows["t"].astype(np.float64))
    lab = np.empty(c.K, dtype=object)
    lab[rows["gid"].astype(np.int64)] = rows["g"]
    gfix = None
    if gfix_sql:
        gf = con.execute(f"""WITH c AS ({gfix_sql})
            SELECT g, count(DISTINCT pu) n FROM c GROUP BY 1""").fetchall()
        labs = np.array([x[0] for x in gf])
        n = np.array([x[1] for x in gf], float)
        r = np.random.default_rng(3)
        keep = n + r.laplace(0, 1.0, size=len(n)) >= tau_count(1.0, 1e-4, 1)
        gfix = set(labs[keep].tolist())
    return c, lab, gfix


def group_hist(c):
    """Per group, the 64-bin log histogram of its per-(PU,group) values. One PU lands in exactly
    one bin of exactly one group's histogram, so the columns sum to the group's support."""
    b = np.clip(np.floor(np.log2(np.maximum(np.abs(c.val), 1.0))).astype(int), 0, B_BINS - 1)
    H = np.zeros((c.K, B_BINS))
    np.add.at(H, (c.gi, b), 1.0)
    return H


def truncated_counts(c, ce, r):
    """Group counts with each PU voting in at most ce of its groups -- what Q4 requires."""
    rank = c.rank_random(r)
    return np.bincount(c.gi[rank < ce], minlength=c.K).astype(float)


QUERIES = {
    "tpch month": ("""SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m') g, sum(l_extendedprice) t
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE {W} GROUP BY 1,2""", "c_acctbal>=8000"),
    "tpch month|nation": ("""SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m')||'|'||
        cast(c_nationkey as varchar) g, sum(l_extendedprice) t
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE {W} GROUP BY 1,2""", "c_acctbal>=8000"),
    "tpch month|prio": ("""SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m')||'|'||o_orderpriority
        g, sum(l_extendedprice) t FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
        JOIN tpch.customer ON c_custkey=o_custkey WHERE {W} GROUP BY 1,2""", "c_acctbal>=8000"),
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--eps-b", type=float, default=0.5)
    ap.add_argument("--eps-eta", type=float, default=0.4)
    ap.add_argument("--cu", type=int, default=37)
    ap.add_argument("--trials", type=int, default=200)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute("ATTACH '/home/ila/Code/privacy/tpch_sass_sf10.db' AS tpch (READ_ONLY)")
    r = np.random.default_rng(7)

    print("Q1  WHY SO FEW PASS. Two multipliers, measured separately.\n")
    print(f"  {'query':<20}{'groups':>8}{'k_u med':>9}{'support med':>13}{'max-bin med':>13}"
          f"{'bin/supp':>10}")
    print("  " + "-" * 74)
    store = {}
    for name, (sql, filt) in QUERIES.items():
        c, lab, gfix = load(con, sql.format(W=filt), sql.format(W="true"))
        H = group_hist(c)
        n_g = c.npu_g
        maxbin = H.max(axis=1)
        store[name] = (c, lab, gfix, H, n_g, maxbin)
        print(f"  {name:<20}{c.K:>8,}{np.median(c.k_u):>9.0f}{np.median(n_g):>13,.0f}"
              f"{np.median(maxbin):>13,.0f}{np.median(maxbin)/max(np.median(n_g),1):>10.2f}")

    b_hist = a.cu / a.eps_b
    t_hist = tau_af(b_hist, DELTA)
    print(f"\n  (a) SENSITIVITY. Reused histogram: sensitivity C_u = {a.cu} (a PU touches up to")
    print(f"      C_u group-histograms), so noise b = C_u/eps_B = {b_hist:.1f} and tau_AF = "
          f"{t_hist:,.0f}.")
    for ce in (1, 5, 37):
        print(f"      Dedicated count at C_e={ce:<3}: b = {ce/a.eps_eta:>5.1f}, "
              f"tau = {tau_count(a.eps_eta, DELTA, ce):>7.1f}"
              f"   ({t_hist/tau_count(a.eps_eta, DELTA, ce):>6.1f}x lower)")
    print("  (b) STATISTIC. The histogram's max BIN count is a fraction of the group's support,")
    print("      since one PU lands in one bin: the ratio above is the second multiplier.")
    print("  -> the two compose. It is not the budget: eps_B = 0.5 here is generous, and the")
    print("     tuned optimum for bound selection elsewhere in this work is eps_b = 0.002.\n")

    print("Q4  TRUNCATION. My earlier run scored raw distinct-PU counts with C_e=1 noise and never")
    print("    truncated. Corrected: each PU votes in at most C_e groups.\n")
    print(f"  {'query':<20}{'C_e':>5}{'raw count med':>15}{'truncated med':>15}{'tau':>9}"
          f"{'AllPass':>9}")
    print("  " + "-" * 73)
    for name, (c, lab, gfix, H, n_g, maxbin) in store.items():
        for ce in (1, 5, 37):
            tc = tau_count(a.eps_eta, DELTA, ce)
            hits = 0
            for _ in range(a.trials):
                cnt = truncated_counts(c, ce, r)
                hits += int((cnt + r.laplace(0, ce / a.eps_eta, size=c.K) >= tc).all())
            med = float(np.median(truncated_counts(c, ce, r)))
            print(f"  {name if ce == 1 else '':<20}{ce:>5}{np.median(n_g):>15,.0f}{med:>15,.0f}"
                  f"{tc:>9.1f}{100*hits/a.trials:>8.1f}%")
    print("  -> truncation divides the count by roughly k_u/C_e, which is the factor my earlier")
    print("     numbers were missing.\n")

    print("Q3  RESTRICT THE AND TO G_Q intersect G_fix, releasing that compact subset.\n")
    print(f"  {'query':<20}{'|G_Q|':>8}{'|G_fix|':>9}{'|intersect|':>12}{'new':>6}"
          f"{'AND over G_Q':>14}{'AND over cap':>14}")
    print("  " + "-" * 83)
    for name, (c, lab, gfix, H, n_g, maxbin) in store.items():
        in_fix = np.array([g in gfix for g in lab])
        ce = 1
        tc = tau_count(a.eps_eta, DELTA, ce)
        hq = hi = 0
        for _ in range(a.trials):
            cnt = truncated_counts(c, ce, r)
            p = cnt + r.laplace(0, ce / a.eps_eta, size=c.K) >= tc
            hq += int(p.all())
            hi += int(p[in_fix].all())
        print(f"  {name:<20}{c.K:>8,}{len(gfix):>9,}{int(in_fix.sum()):>12,}"
              f"{int((~in_fix).sum()):>6}{100*hq/a.trials:>13.1f}%{100*hi/a.trials:>13.1f}%")
    print("  -> on PU-side filters G_Q is almost a subset of G_fix, so the intersection is nearly")
    print("     all of G_Q and the restriction changes little. It would matter for a query with")
    print("     many groups outside G_fix, i.e. one whose filter creates new groups.")


if __name__ == "__main__":
    main()
