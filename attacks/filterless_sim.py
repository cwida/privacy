#!/usr/bin/env python3
"""Filterless / CROWD universal-bound simulator.

Evaluates the 10 June 2026 `filterless-crowd-dp-proposal.pdf` outside the extension,
as plain SQL over TPC-H, so we can decide whether to implement it before writing C++.

The proposal is contribution bounding measured filterlessly: instead of asking the
analyst for a per-PU bound (dp_sum_bound) or estimating one per query with privacy
budget (Wilson's APPROX_BOUNDS), derive it once from the *unfiltered* domain and
freeze it. Two levels of bound:

  per-group bound B_g   caps one PU's contribution to one group     (note, eq. 16)
  per-PU total          caps one PU's contribution across groups    (note, eq. 25)

Both are picked with the CROWD rule: bin by powers of f, take the top of the highest
bin that at least s distinct PUs land in, so an isolated outlier never sets a bound.

Mechanisms compared, per group, with PU = customer:

  google      sum_u min(t,B_G) * min(1, C_u/k_u)   scalar bound from the *filtered*
              scale = C_u * B_G / eps              data + random truncation to C_u
  google_nocu sum_u min(t,B_G)                     same bound, no truncation
              scale = max_u k_u * B_G / eps        (isolates truncation's cost)
  filterless  sum_u min(t,B_g)                     the note as written: per-group
              scale = Delta1 / eps                 CROWD bound, Delta1 = max_u norm
  fl_l1crowd  sum_u min(t,B_g) * min(1, D_s/n_u)   the repair: CROWD applied to the
              scale = D_s / eps                    per-PU totals too, then l1-clip

`fl_l1crowd` is not in the note. eq. (25)'s Delta1 = max_u (...) is a bare maximum, so
it is not CROWD-protected even though B_g is; one PU that is fat and wide drives it to
k*B_g. Applying the note's own priv_max to the *norm* distribution and l1-clipping each
PU's vector to the result gives sensitivity exactly D_s, deterministically, with no
random truncation error.

`google` is a stand-in: Wilson's APPROX_BOUNDS is modelled by the same CROWD rule on
the filtered partials, pooled to one scalar. This isolates the real differences
(filtered vs full-domain, per-group vs global, truncation vs l1-clip) from any
difference in how a bound is picked. It flatters the baseline, which spends no budget
here. Truncation is scored by its exact expectation (P[keep g] = min(1, C_u/k_u))
rather than sampled, to keep Monte-Carlo noise out of the comparison.

Modes:
  (default)     one measure, one filter, all four mechanisms
  --sweep       the same over a ladder of increasingly selective filters. The metadata
                is built ONCE and reused, which is the point: bounds do not depend on
                the filter, so this also demonstrates the freezing property.
  --coalition   how many colluding fat+wide PUs it takes to move each bound, and how
                big the jump is when the support threshold trips
  --attack      narrow the filter around one target PU and watch both bounds

Adversaries (composable with any mode):
  --skew K --skew-spread   one PU inflated K-fold and/or present in every group
  --spike K                one PU inflated K-fold in a *single* group (the
                           expression-attack shape: case when pu=target then 1e9 ...)
  --coalition-size N       N colluding fat+wide PUs instead of one

Only nonnegative measures; the note's signed two-histogram case (§9.2) is not
simulated, nor MIN/MAX, nor the epsilon split across several aggregates.
"""

import argparse
import sys

import duckdb
import numpy as np

GROUPBYS = {
    "priority": "o_orderpriority",
    "year": "cast(year(o_orderdate) as varchar)",
    "quarter": "cast(year(o_orderdate) as varchar) || 'Q' || cast(quarter(o_orderdate) as varchar)",
    "month": "strftime(o_orderdate, '%Y-%m')",
    "month_priority": "strftime(o_orderdate, '%Y-%m') || '|' || o_orderpriority",
}

MEASURES = {
    "price": "l_extendedprice",
    "quantity": "l_quantity",
    "count": "1",  # SUM(1) = COUNT(*)
}

BASE = "l_shipmode in ('AIR', 'REG AIR')"

# Progressively more selective filters. Bounds are frozen, so a filterless mechanism
# reuses one set of metadata for all of them; Wilson re-derives per query.
FILTER_LADDER = [
    ("no filter", "true"),
    ("shipmode AIR/REG AIR", BASE),
    ("+ returnflag=R", f"{BASE} and l_returnflag = 'R'"),
    ("+ quantity<10", f"{BASE} and l_returnflag = 'R' and l_quantity < 10"),
    ("+ discount<0.03", f"{BASE} and l_returnflag = 'R' and l_quantity < 10 and l_discount < 0.03"),
    (
        "+ tax<0.03",
        f"{BASE} and l_returnflag = 'R' and l_quantity < 10 and l_discount < 0.03 and l_tax < 0.03",
    ),
]


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def open_db(path, sf):
    con = duckdb.connect()
    try:
        con.execute(f"ATTACH '{path}' AS tpch (READ_ONLY)")
        log(f"attached {path} (read-only)")
    except duckdb.Error:
        log(f"cannot attach {path} — generating TPC-H sf{sf}")
        con.execute(f"ATTACH '{path}' AS tpch")
        con.execute("INSTALL tpch; LOAD tpch")
        con.execute(f"USE tpch; CALL dbgen(sf={sf}); USE memory")
    return con


def build_contributions(con, args, filters):
    """One pass giving, per (PU, group): the full-domain contribution a, and one
    filtered contribution t_i per filter in `filters`. This is the per-user
    pre-aggregation of §3; both sides come out of the same pipeline."""
    gexpr = GROUPBYS[args.groupby]
    mexpr = MEASURES[args.measure]
    tcols = ",\n               ".join(
        f"sum(case when {f} then {mexpr} else 0 end) AS t{i}" for i, (_, f) in enumerate(filters)
    )
    con.execute(
        f"""
        CREATE OR REPLACE TABLE contrib AS
        SELECT o_custkey AS pu,
               {gexpr}   AS g,
               sum({mexpr}) AS a,
               {tcols}
        FROM tpch.lineitem JOIN tpch.orders ON o_orderkey = l_orderkey
        GROUP BY 1, 2
        """
    )
    n_rows, n_pu, n_groups = con.execute(
        "SELECT count(*), count(distinct pu), count(distinct g) FROM contrib"
    ).fetchone()
    log(f"contrib: {n_rows} (pu,group) rows, {n_pu} PUs, {n_groups} groups")
    apply_adversary(con, args, len(filters))
    return n_groups


def apply_adversary(con, args, n_filters):
    """Plant the adversarial PU(s). Applied to the contribution relation, never to the
    read-only source db."""
    n_coal = args.coalition_size
    if n_coal == 0 and args.skew == 1.0 and not args.skew_spread and args.spike == 1.0:
        return
    tset_all = ", ".join(f"t{i} = t{i} * {{k}}" for i in range(n_filters))
    tcols = ", ".join(f"t{i}" for i in range(n_filters))

    if args.spike != 1.0:
        # Expression-attack shape: one PU, one group, huge value.
        target, grp = con.execute(
            "SELECT pu, g FROM contrib ORDER BY a DESC LIMIT 1"
        ).fetchone()
        con.execute(
            f"UPDATE contrib SET a = a * {args.spike}, "
            + tset_all.format(k=args.spike)
            + f" WHERE pu = {target} AND g = '{grp}'"
        )
        log(f"spike: PU {target} x{args.spike} in group {grp} only")
        return

    if n_coal > 0:
        # N colluding PUs, each present in every group at `skew` x the group median.
        con.execute(
            f"""
            INSERT INTO contrib
            SELECT -c.i, m.g, m.a * {args.skew},
                   {", ".join(f"m.t{i} * {args.skew}" for i in range(n_filters))}
            FROM (SELECT unnest(range(1, {n_coal} + 1)) AS i) c
            CROSS JOIN (SELECT g, median(a) AS a,
                               {", ".join(f"median(t{i}) AS t{i}" for i in range(n_filters))}
                        FROM contrib GROUP BY g) m
            """
        )
        log(f"coalition: {n_coal} PUs, every group, x{args.skew} the group median")
        return

    target = con.execute("SELECT pu FROM contrib GROUP BY pu ORDER BY sum(a) DESC LIMIT 1").fetchone()[0]
    if args.skew_spread:
        con.execute(
            f"""
            INSERT INTO contrib
            SELECT {target}, g, a, {tcols} FROM (
                SELECT g, median(a) AS a,
                       {", ".join(f"median(t{i}) AS t{i}" for i in range(n_filters))}
                FROM contrib GROUP BY g
            ) WHERE g NOT IN (SELECT g FROM contrib WHERE pu = {target})
            """
        )
    if args.skew != 1.0:
        con.execute(
            f"UPDATE contrib SET a = a * {args.skew}, "
            + tset_all.format(k=args.skew)
            + f" WHERE pu = {target}"
        )
    log(f"skew: PU {target} x{args.skew}, spread={args.skew_spread}")


def crowd(con, value_expr, source, f, s, group_key=None):
    """The CROWD / priv_max rule (eq. 16): top of the highest power-of-f bin that at
    least s distinct PUs land in. Returns a scalar, or materialises one per group."""
    if group_key is None:
        row = con.execute(
            f"""
            WITH bins AS (
                SELECT cast(floor(ln({value_expr}) / ln({f})) AS INTEGER) AS bin,
                       count(distinct pu) AS support
                FROM {source} WHERE {value_expr} > 0 GROUP BY 1
            )
            SELECT max(bin) FROM bins WHERE support >= {s}
            """
        ).fetchone()
        return 0.0 if row[0] is None else float(f) ** (row[0] + 1)
    con.execute(
        f"""
        CREATE OR REPLACE TABLE bg AS
        WITH bins AS (
            SELECT {group_key} AS g, cast(floor(ln({value_expr}) / ln({f})) AS INTEGER) AS bin,
                   count(*) AS support
            FROM {source} WHERE {value_expr} > 0 GROUP BY 1, 2
        )
        SELECT g, pow({f}, max(bin) + 1) AS B FROM bins WHERE support >= {s} GROUP BY g
        """
    )
    return None


def build_metadata(con, args):
    """The frozen half: per-group bounds, released group universe, per-PU norms, and
    the two candidate sensitivities. None of this reads a filtered column."""
    crowd(con, "a", "contrib", args.f, args.s, group_key="g")
    con.execute(
        f"""
        CREATE OR REPLACE TABLE gstar AS
        SELECT g FROM contrib WHERE a > 0 GROUP BY g HAVING count(*) >= {args.s}
        """
    )
    con.execute(
        """
        CREATE OR REPLACE TABLE norms AS
        SELECT pu, sum(least(a, B)) AS na
        FROM contrib JOIN bg USING (g) WHERE g IN (SELECT g FROM gstar)
        GROUP BY pu
        """
    )
    delta1 = con.execute("SELECT max(na) FROM norms").fetchone()[0] or 0.0
    d_s = crowd(con, "na", "norms", args.f, args.s)
    n_kept = con.execute("SELECT count(*) FROM gstar").fetchone()[0]
    return float(delta1), d_s, n_kept


def evaluate_filter(con, args, tcol, delta1, d_s, rng):
    """The per-query half, for one filter column."""
    bg_scalar = crowd(con, tcol, "contrib", args.f, args.s)
    con.execute(
        f"""
        CREATE OR REPLACE TABLE kk AS
        SELECT pu, count(*) AS k, sum(least({tcol}, B)) AS nt
        FROM contrib JOIN bg USING (g)
        WHERE {tcol} > 0 AND g IN (SELECT g FROM gstar)
        GROUP BY pu
        """
    )
    max_k, p99_k = con.execute("SELECT max(k), quantile_cont(k, 0.99) FROM kk").fetchone()
    max_k, p99_k = int(max_k or 1), float(p99_k or 1.0)
    cu = args.cu if args.cu is not None else max(1, int(round(p99_k)))

    rows = con.execute(
        f"""
        SELECT sum(c.{tcol})                                        AS true_sum,
               sum(least(c.{tcol}, bg.B))                           AS fl,
               sum(least(c.{tcol}, {bg_scalar}) * least(1.0, {cu} / kk.k)) AS goog,
               sum(least(c.{tcol}, {bg_scalar}))                    AS goog_nocu,
               sum(least(c.{tcol}, bg.B) * least(1.0, {d_s} / kk.nt)) AS fl_l1
        FROM contrib c JOIN bg USING (g) JOIN kk ON kk.pu = c.pu
        WHERE c.g IN (SELECT g FROM gstar) AND c.{tcol} > 0 AND kk.nt > 0
        GROUP BY c.g
        """
    ).fetchall()
    if not rows:
        return None
    arr = np.array(rows, dtype=float)
    true = arr[:, 0]
    eps = args.epsilon
    scales = {
        "filterless": delta1 / eps,
        "google": cu * bg_scalar / eps,
        "google_nocu": max_k * bg_scalar / eps,
        "fl_l1crowd": d_s / eps,
    }
    cols = {"filterless": 1, "google": 2, "google_nocu": 3, "fl_l1crowd": 4}
    out = {}
    for name, col in cols.items():
        out[name] = score(arr[:, col], true, scales[name], args.trials, rng)
    return {
        "n_groups": len(true),
        "cu": cu,
        "max_k": max_k,
        "bg_scalar": bg_scalar,
        "mechs": out,
    }


def score(released, true, scale, trials, rng):
    live = true > 0
    released, true = released[live], true[live]
    bias = np.abs(released - true) / true
    noise = rng.laplace(0.0, scale, size=(trials, released.size)) if scale > 0 else 0.0
    total = np.abs(released + noise - true) / true
    return {
        "scale": scale,
        "bias": float(np.median(bias)),
        "total": float(np.median(np.mean(total, axis=0))),
    }


def selectivity(con, filt):
    return con.execute(
        f"SELECT avg(case when {filt} then 1.0 else 0.0 end) FROM tpch.lineitem"
    ).fetchone()[0]


def header(args, delta1, d_s, n_kept, n_groups):
    print()
    print(f"measure=SUM({MEASURES[args.measure]})  group-by={args.groupby}  eps={args.epsilon}")
    print(f"f={args.f}  s={args.s}  groups in G*={n_kept}/{n_groups}")
    print(f"Delta1 (eq.25, max_u)={delta1:,.0f}   D_s (CROWD norm)={d_s:,.0f}")


def run_single(con, args):
    filters = [("query", args.filter)]
    n_groups = build_contributions(con, args, filters)
    delta1, d_s, n_kept = build_metadata(con, args)
    if n_kept == 0:
        log("no group survives G* — lower --s or widen the query")
        return
    rng = np.random.default_rng(args.seed)
    res = evaluate_filter(con, args, "t0", delta1, d_s, rng)
    header(args, delta1, d_s, n_kept, n_groups)
    print(f"filter={args.filter}")
    print(f"C_u={res['cu']} (max fan-out {res['max_k']})   B_global(filtered)={res['bg_scalar']:,.0f}")
    print()
    print(f"{'mechanism':<14}{'noise scale':>16}{'median |bias|':>16}{'median rel err':>18}")
    print("-" * 64)
    for name, r in sorted(res["mechs"].items(), key=lambda kv: kv[1]["total"]):
        print(f"{name:<14}{r['scale']:>16,.0f}{r['bias']:>15.1%}{r['total']:>17.1%}")
    print()


def run_sweep(con, args):
    """Selectivity ladder. Metadata is built once and reused for every filter, which is
    the freezing property; Wilson's scalar bound is re-derived per filter."""
    n_groups = build_contributions(con, args, FILTER_LADDER)
    delta1, d_s, n_kept = build_metadata(con, args)
    if n_kept == 0:
        log("no group survives G*")
        return
    rng = np.random.default_rng(args.seed)
    header(args, delta1, d_s, n_kept, n_groups)
    print()
    print(
        f"{'filter':<24}{'sel.':>8}{'groups':>8}{'B_G':>14}"
        f"{'filterless':>13}{'fl_l1crowd':>13}{'google':>10}"
    )
    print("-" * 90)
    for i, (label, filt) in enumerate(FILTER_LADDER):
        sel = selectivity(con, filt)
        res = evaluate_filter(con, args, f"t{i}", delta1, d_s, rng)
        if res is None:
            print(f"{label:<24}{sel:>7.2%}{'0':>8}{'—':>14}{'(no groups released)':>36}")
            continue
        m = res["mechs"]
        print(
            f"{label:<24}{sel:>7.2%}{res['n_groups']:>8}{res['bg_scalar']:>14,.0f}"
            f"{m['filterless']['total']:>12.1%}{m['fl_l1crowd']['total']:>13.1%}"
            f"{m['google']['total']:>10.1%}"
        )
    print()
    print("filterless/fl_l1crowd reuse ONE frozen metadata set for every row above;")
    print("google re-derives B_G per filter (and would pay budget for it).")
    print()


def run_coalition(con, args):
    """How many colluding fat+wide PUs it takes to move each sensitivity."""
    print()
    print(f"coalition attack: N PUs, each in every group, at {args.skew}x the group median")
    print(f"measure=SUM({MEASURES[args.measure]})  group-by={args.groupby}  f={args.f}  s={args.s}")
    print()
    print(f"{'N':>8}{'Delta1 (max_u)':>18}{'D_s (CROWD)':>16}{'D_s moved?':>13}")
    print("-" * 56)
    baseline = None
    skew = args.skew
    for n in args.coalition_ladder:
        args.coalition_size = int(n)
        args.skew = skew if n > 0 else 1.0  # N=0 must be a clean baseline
        build_contributions(con, args, [("query", args.filter)])
        delta1, d_s, _ = build_metadata(con, args)
        if baseline is None:
            baseline = d_s
        moved = "—" if d_s == baseline else f"x{d_s / baseline:.0f}"
        print(f"{int(n):>8}{delta1:>18,.0f}{d_s:>16,.0f}{moved:>13}")
    print()
    print(f"s = {args.s}: the coalition must reach s members before its bin is supported.")
    print()


def run_attack(con, args):
    """Narrow the filter around one target PU and watch both bounds (Rem. 3.1)."""
    gexpr = GROUPBYS[args.groupby]
    mexpr = MEASURES[args.measure]
    target, bal = con.execute(
        "SELECT c_custkey, c_acctbal FROM tpch.customer ORDER BY c_acctbal DESC LIMIT 1"
    ).fetchone()
    print()
    print(f"filter-construction attack, target = customer {target} (acctbal {bal})")
    print(f"group-by={args.groupby}  f={args.f}  s={args.s}")
    print()
    print(f"{'filter':<26}{'PUs left':>10}{'B_G (filtered)':>18}{'B_g (filterless)':>19}")
    print("-" * 74)
    for thresh in args.attack_thresholds:
        filt = f"c_acctbal >= {thresh}"
        con.execute(
            f"""
            CREATE OR REPLACE TABLE contrib AS
            SELECT o_custkey AS pu, {gexpr} AS g, sum({mexpr}) AS a,
                   sum(case when {filt} then {mexpr} else 0 end) AS t0
            FROM tpch.lineitem
            JOIN tpch.orders ON o_orderkey = l_orderkey
            JOIN tpch.customer ON c_custkey = o_custkey
            GROUP BY 1, 2
            """
        )
        left = con.execute("SELECT count(distinct pu) FROM contrib WHERE t0 > 0").fetchone()[0]
        build_metadata(con, args)
        bg_scalar = crowd(con, "t0", "contrib", args.f, args.s)
        med = con.execute("SELECT median(B) FROM bg WHERE g IN (SELECT g FROM gstar)").fetchone()[0]
        gs = f"{bg_scalar:,.0f}" if bg_scalar else "0 (collapsed)"
        print(f"{filt:<26}{left:>10,}{gs:>18}{(f'{med:,.0f}' if med else '0'):>19}")
    print()


def main():
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--db", default="tpch_sass_sf10.db")
    p.add_argument("--sf", type=float, default=10)
    p.add_argument("--groupby", default="month", choices=sorted(GROUPBYS))
    p.add_argument("--measure", default="price", choices=sorted(MEASURES))
    p.add_argument("--filter", default=BASE)
    p.add_argument("--epsilon", type=float, default=1.0)
    p.add_argument("-f", "--f", type=float, default=2.0, help="exponential bin factor")
    p.add_argument("-s", "--s", type=int, default=350, help="CROWD support threshold")
    p.add_argument("--cu", type=int, default=None, help="Wilson C_u (default: p99 fan-out)")
    p.add_argument("--skew", type=float, default=1.0)
    p.add_argument("--skew-spread", action="store_true")
    p.add_argument("--spike", type=float, default=1.0, help="inflate one PU in ONE group")
    p.add_argument("--coalition-size", type=int, default=0)
    p.add_argument("--trials", type=int, default=200)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--sweep", action="store_true", help="selectivity ladder")
    p.add_argument("--coalition", action="store_true", help="coalition-size ladder")
    p.add_argument("--attack", action="store_true", help="filter-construction attack")
    p.add_argument("--coalition-ladder", type=float, nargs="+", default=[0, 1, 10, 100, 349, 350, 700])
    p.add_argument(
        "--attack-thresholds", type=float, nargs="+", default=[-1000, 0, 5000, 9000, 9800, 9990, 9998]
    )
    args = p.parse_args()
    con = open_db(args.db, args.sf)
    if args.attack:
        run_attack(con, args)
    elif args.coalition:
        run_coalition(con, args)
    elif args.sweep:
        run_sweep(con, args)
    else:
        run_single(con, args)


if __name__ == "__main__":
    main()
