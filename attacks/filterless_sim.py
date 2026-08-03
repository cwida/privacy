#!/usr/bin/env python3
"""Filterless / CROWD universal-bound simulator.

Tests the two claims of `filterless-crowd-dp-proposal.pdf` (10 June 2026) outside
the extension, as plain SQL over TPC-H, so we can decide whether the idea is worth
implementing before writing any C++.

Claim 1 (utility, Remark 2.1): freezing an exact full-domain per-user norm
    Delta1 = max_u sum_g min(a(u,g), B_g)
beats Wilson et al.'s random cross-partition truncation to C_u, because we avoid
the truncation error Error_{C_u}. The counter-worry is that Delta1 is uncapped, so
a high-fan-out user could make the noise worse than Wilson's C_u * B.

Claim 2 (stability, Remark 3.1): because B_g comes from the *unfiltered* domain
contribution a(u,g), an analyst cannot steer the bound by tightening the query
filter around one target PU. Wilson's query-specific bounds can be steered.

Mechanisms compared per group g (PU = customer, measure = SUM(l_extendedprice)):

  true        sum_u t(u,g)                                  no privacy
  google      sum_u min(t,B_G) * min(1, C_u/k_u)            scalar bound from the
              scale = C_u * B_G / eps                       *filtered* data, random
                                                            truncation to C_u groups
  google_nocu sum_u min(t,B_G)                              same bound, no truncation
              scale = max_u k_u * B_G / eps                 (isolates truncation cost)
  filterless  sum_u min(t,B_g)                              per-group CROWD bound from
              scale = Delta1 / eps                          the *full* domain
  fl_l1crowd  sum_u min(t,B_g) * min(1, D_s/n_u)            same, but each user's
              scale = D_s / eps                             contribution vector is
                                                            l1-clipped to a CROWD-
                                                            supported norm D_s

`fl_l1crowd` is not in the note. It is the repair for the hole the simulator finds:
eq. (25)'s Delta1 = max_u (...) is a bare maximum over users, so it is *not* CROWD-
protected even though B_g is. One user who is both fat (above B_g) and wide (present
in every group) drives Delta1 to k * B_g and inflates everyone's noise. Wilson
survives that case precisely because C_u truncation caps the norm. So instead of
`max_u`, take the top of the highest exponential bin of the per-user norms whose
distinct-PU support is at least s (the same `priv_max` primitive, applied to the norm
distribution), and l1-clip each user's contribution vector to it. Sensitivity is then
exactly D_s, deterministically, with no random truncation error.

Truncation is scored by its exact expectation (Wilson samples C_u of a user's k_u
partitions without replacement, so P[keep g] = min(1, C_u/k_u)) rather than by
sampling it, which removes one source of Monte-Carlo noise from the comparison.
Laplace noise is then Monte-Carlo'd over the per-group vector.

Only nonnegative measures are handled; the note's signed two-histogram case (§9.2)
is not simulated.
"""

import argparse
import math
import os
import sys

import duckdb
import numpy as np

# Grouping expressions, ordered by how many groups one customer can touch.
GROUPBYS = {
    "priority": "o_orderpriority",
    "year": "cast(year(o_orderdate) as varchar)",
    "quarter": "cast(year(o_orderdate) as varchar) || 'Q' || cast(quarter(o_orderdate) as varchar)",
    "month": "strftime(o_orderdate, '%Y-%m')",
    "month_priority": "strftime(o_orderdate, '%Y-%m') || '|' || o_orderpriority",
}

DEFAULT_FILTER = "l_shipmode in ('AIR', 'REG AIR')"


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def open_db(path, sf):
    """Attach an existing TPC-H db read-only, or generate one at `path`."""
    con = duckdb.connect()
    if os.path.exists(path):
        con.execute(f"ATTACH '{path}' AS tpch (READ_ONLY)")
        log(f"attached {path} (read-only)")
    else:
        log(f"{path} not found — generating TPC-H sf{sf} (this takes a while)")
        con.execute(f"ATTACH '{path}' AS tpch")
        con.execute("INSTALL tpch; LOAD tpch")
        con.execute(f"USE tpch; CALL dbgen(sf={sf})")
        con.execute("USE memory")
    return con


def build_contributions(con, groupby, filt, skew_factor, skew_spread):
    """One pass over the join giving, per (PU, group), the filtered contribution t
    and the full-domain contribution a. This is the per-user pre-aggregation the
    note's §3 describes; both sides come from the same pipeline."""
    gexpr = GROUPBYS[groupby]
    con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE contrib AS
        SELECT o_custkey                                            AS pu,
               {gexpr}                                              AS g,
               sum(case when {filt} then l_extendedprice else 0 end) AS t,
               sum(l_extendedprice)                                  AS a
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey = l_orderkey
        GROUP BY 1, 2
        """
    )
    n_rows, n_pu, n_groups = con.execute(
        "SELECT count(*), count(distinct pu), count(distinct g) FROM contrib"
    ).fetchone()
    log(f"contrib: {n_rows} (pu,group) rows, {n_pu} PUs, {n_groups} groups")

    target = None
    if skew_factor != 1.0 or skew_spread:
        # Pick the fattest customer and make it fatter — the "Elon" case. Skew is
        # applied to the contribution relation, not to the read-only source db.
        target = con.execute(
            "SELECT pu FROM contrib GROUP BY pu ORDER BY sum(a) DESC LIMIT 1"
        ).fetchone()[0]
        if skew_spread:
            # Give the target a presence in *every* group, maximising its fan-out k.
            con.execute(
                f"""
                INSERT INTO contrib
                SELECT {target}, g, med_t, med_a FROM (
                    SELECT g, median(t) AS med_t, median(a) AS med_a FROM contrib GROUP BY g
                ) WHERE g NOT IN (SELECT g FROM contrib WHERE pu = {target})
                """
            )
        if skew_factor != 1.0:
            con.execute(
                f"UPDATE contrib SET t = t * {skew_factor}, a = a * {skew_factor} WHERE pu = {target}"
            )
        log(f"skew: PU {target} scaled x{skew_factor}, spread={skew_spread}")
    return target


def crowd_bound_per_group(con, f, s):
    """Definition 5.1 + eq. (16): per group, bin the full-domain contributions into
    exponential bins [f^b, f^(b+1)), and take the top of the highest bin whose
    distinct-PU support is at least s."""
    con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE bg AS
        WITH bins AS (
            SELECT g, cast(floor(ln(a) / ln({f})) AS INTEGER) AS bin, count(*) AS support
            FROM contrib WHERE a > 0 GROUP BY 1, 2
        ), top AS (
            SELECT g, max(bin) AS best FROM bins WHERE support >= {s} GROUP BY g
        )
        SELECT g, pow({f}, best + 1) AS B FROM top
        """
    )
    # G*: groups with at least s distinct contributing PUs in the full domain (eq. 12).
    con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE gstar AS
        SELECT g FROM contrib WHERE a > 0 GROUP BY g HAVING count(*) >= {s}
        """
    )
    kept, total = con.execute(
        "SELECT (SELECT count(*) FROM gstar), (SELECT count(distinct g) FROM contrib)"
    ).fetchone()
    unbounded = con.execute(
        "SELECT count(*) FROM gstar WHERE g NOT IN (SELECT g FROM bg)"
    ).fetchone()[0]
    log(f"G*: {kept}/{total} groups have >= {s} PUs; {unbounded} of them have no supported bin")
    return kept, total


def global_bound(con, f, s):
    """Wilson's APPROX_BOUNDS stand-in: the same CROWD rule, but applied to the
    *filtered* per-(u,g) partials pooled across groups, yielding one scalar. This
    isolates the two real differences (filtered vs full-domain, per-group vs global)
    from any difference in how the bound is picked."""
    row = con.execute(
        f"""
        WITH bins AS (
            SELECT cast(floor(ln(t) / ln({f})) AS INTEGER) AS bin, count(distinct pu) AS support
            FROM contrib WHERE t > 0 GROUP BY 1
        )
        SELECT max(bin) FROM bins WHERE support >= {s}
        """
    ).fetchone()
    return 0.0 if row[0] is None else float(f) ** (row[0] + 1)


def sensitivities(con, f, s):
    """Delta1 (eq. 25) for filterless, the CROWD-supported norm D_s that repairs it,
    and the fan-out statistics Wilson's C_u caps."""
    con.execute(
        """
        CREATE OR REPLACE TEMP TABLE norms AS
        SELECT pu, sum(least(a, B)) AS na, sum(least(t, B)) AS nt
        FROM contrib JOIN bg USING (g) WHERE g IN (SELECT g FROM gstar)
        GROUP BY pu
        """
    )
    delta1 = con.execute("SELECT max(na) FROM norms").fetchone()[0]
    # Same priv_max rule as eq. (16), applied to the per-user norm distribution.
    row = con.execute(
        f"""
        WITH bins AS (
            SELECT cast(floor(ln(na) / ln({f})) AS INTEGER) AS bin, count(*) AS support
            FROM norms WHERE na > 0 GROUP BY 1
        )
        SELECT max(bin) FROM bins WHERE support >= {s}
        """
    ).fetchone()
    d_s = 0.0 if row[0] is None else float(f) ** (row[0] + 1)
    max_k, mean_k, p99_k = con.execute(
        """
        SELECT max(k), avg(k), quantile_cont(k, 0.99) FROM (
            SELECT pu, count(*) AS k FROM contrib
            WHERE t > 0 AND g IN (SELECT g FROM gstar) GROUP BY pu
        )
        """
    ).fetchone()
    return float(delta1 or 0.0), d_s, int(max_k or 0), float(mean_k or 0.0), float(p99_k or 0.0)


def per_group_sums(con, bg_scalar, cu, d_s):
    con.execute(
        """
        CREATE OR REPLACE TEMP TABLE kk AS
        SELECT pu, count(*) AS k FROM contrib
        WHERE t > 0 AND g IN (SELECT g FROM gstar) GROUP BY pu
        """
    )
    rows = con.execute(
        f"""
        SELECT c.g,
               sum(c.t)                                              AS true_sum,
               sum(least(c.t, bg.B))                                 AS fl_sum,
               sum(least(c.t, {bg_scalar}) * least(1.0, {cu} / kk.k)) AS goog_sum,
               sum(least(c.t, {bg_scalar}))                          AS goog_nocu_sum,
               sum(least(c.t, bg.B) * least(1.0, {d_s} / n.nt))      AS fl_l1_sum
        FROM contrib c
        JOIN bg USING (g)
        JOIN kk ON kk.pu = c.pu
        JOIN norms n ON n.pu = c.pu
        WHERE c.g IN (SELECT g FROM gstar) AND c.t > 0 AND n.nt > 0
        GROUP BY c.g
        ORDER BY c.g
        """
    ).fetchall()
    arr = np.array([[r[1], r[2], r[3], r[4], r[5]] for r in rows], dtype=float)
    return arr  # columns: true, filterless, google, google_nocu, fl_l1crowd


def score(name, released, true, scale, trials, rng):
    """Median-over-groups relative error, split into deterministic (clipping /
    truncation) and stochastic (Laplace) parts."""
    live = true > 0
    released, true = released[live], true[live]
    bias = np.abs(released - true) / true
    noise = rng.laplace(0.0, scale, size=(trials, released.size)) if scale > 0 else 0.0
    total = np.abs(released + noise - true) / true
    return {
        "mech": name,
        "scale": scale,
        "bias": float(np.median(bias)),
        "total": float(np.median(np.mean(total, axis=0))),
    }


def run(args):
    con = open_db(args.db, args.sf)
    target = build_contributions(con, args.groupby, args.filter, args.skew, args.skew_spread)
    n_kept, n_total = crowd_bound_per_group(con, args.f, args.s)
    if n_kept == 0:
        log("no group survives G* — raise --s or widen the query")
        return
    bg_scalar = global_bound(con, args.f, args.s)
    delta1, d_s, max_k, mean_k, p99_k = sensitivities(con, args.f, args.s)
    cu = args.cu if args.cu is not None else max(1, int(round(p99_k)))

    sums = per_group_sums(con, bg_scalar, cu, d_s)
    true = sums[:, 0]
    rng = np.random.default_rng(args.seed)
    eps = args.epsilon

    results = [
        score("filterless", sums[:, 1], true, delta1 / eps, args.trials, rng),
        score("google", sums[:, 2], true, cu * bg_scalar / eps, args.trials, rng),
        score("google_nocu", sums[:, 3], true, max_k * bg_scalar / eps, args.trials, rng),
        score("fl_l1crowd", sums[:, 4], true, d_s / eps, args.trials, rng),
    ]

    print()
    print(f"group-by={args.groupby}  filter={args.filter}")
    print(f"eps={eps}  f={args.f}  s={args.s}  C_u={cu} (p99 fan-out={p99_k:.0f}, max={max_k}, mean={mean_k:.1f})")
    print(f"groups released={len(true)}/{n_total}   B_global(filtered)={bg_scalar:,.0f}")
    print(f"Delta1 (eq.25, max_u)={delta1:,.0f}   D_s (CROWD norm, s={args.s})={d_s:,.0f}")
    if target is not None:
        print(f"skewed PU={target} x{args.skew} spread={args.skew_spread}")
    print()
    print(f"{'mechanism':<14}{'noise scale':>16}{'median |bias|':>16}{'median rel err':>18}")
    print("-" * 64)
    for r in sorted(results, key=lambda r: r["total"]):
        print(f"{r['mech']:<14}{r['scale']:>16,.0f}{r['bias']:>15.1%}{r['total']:>17.1%}")
    print()


def run_attack(args):
    """Claim 2: narrow the filter around one target PU and watch the two bounds.
    Wilson's bound is computed on the filtered data, so it should track the target;
    the filterless bound is computed on the full domain, so it should not move."""
    con = open_db(args.db, args.sf)
    gexpr = GROUPBYS[args.groupby]
    target, target_bal = con.execute(
        "SELECT c_custkey, c_acctbal FROM tpch.customer ORDER BY c_acctbal DESC LIMIT 1"
    ).fetchone()
    log(f"attack target: customer {target} (acctbal {target_bal})")

    print()
    print(f"filter-construction attack, group-by={args.groupby}, s={args.s}, f={args.f}")
    print(f"target = customer {target}")
    print()
    print(f"{'filter':<28}{'PUs left':>10}{'B_global (filtered)':>22}{'B_g (filterless)':>20}")
    print("-" * 80)

    for thresh in args.attack_thresholds:
        filt = f"c_acctbal >= {thresh}"
        con.execute(
            f"""
            CREATE OR REPLACE TEMP TABLE contrib AS
            SELECT o_custkey AS pu, {gexpr} AS g,
                   sum(case when {filt} then l_extendedprice else 0 end) AS t,
                   sum(l_extendedprice)                                  AS a
            FROM tpch.lineitem
            JOIN tpch.orders ON o_orderkey = l_orderkey
            JOIN tpch.customer ON c_custkey = o_custkey
            GROUP BY 1, 2
            """
        )
        pus_left = con.execute("SELECT count(distinct pu) FROM contrib WHERE t > 0").fetchone()[0]
        crowd_bound_per_group(con, args.f, args.s)
        bg_scalar = global_bound(con, args.f, args.s)
        med_bg = con.execute(
            "SELECT median(B) FROM bg WHERE g IN (SELECT g FROM gstar)"
        ).fetchone()[0]
        gs = f"{bg_scalar:,.0f}" if bg_scalar else "0 (collapsed)"
        bs = f"{med_bg:,.0f}" if med_bg else "0"
        print(f"{filt:<28}{pus_left:>10,}{gs:>22}{bs:>20}")
    print()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--db", default="tpch_sass_sf10.db", help="TPC-H database (attached read-only)")
    p.add_argument("--sf", type=float, default=10, help="scale factor if --db must be generated")
    p.add_argument("--groupby", default="month", choices=sorted(GROUPBYS))
    p.add_argument("--filter", default=DEFAULT_FILTER, help="query filter over lineitem/orders")
    p.add_argument("--epsilon", type=float, default=1.0)
    p.add_argument("-f", "--f", type=float, default=4.0, help="exponential bin factor")
    p.add_argument("-s", "--s", type=int, default=350, help="CROWD support threshold")
    p.add_argument("--cu", type=int, default=None, help="Wilson C_u (default: p99 fan-out)")
    p.add_argument("--skew", type=float, default=1.0, help="multiply the fattest PU's contribution")
    p.add_argument("--skew-spread", action="store_true", help="give the fattest PU a row in every group")
    p.add_argument("--trials", type=int, default=200)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--attack", action="store_true", help="run the filter-construction attack instead")
    p.add_argument(
        "--attack-thresholds",
        type=float,
        nargs="+",
        default=[-1000, 0, 5000, 9000, 9800, 9990, 9998],
        help="c_acctbal thresholds, widest first",
    )
    args = p.parse_args()
    if args.attack:
        run_attack(args)
    else:
        run(args)


if __name__ == "__main__":
    main()
