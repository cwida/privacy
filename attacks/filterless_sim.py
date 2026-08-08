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

# Per-dataset plumbing: where the facts come from, what identifies the PU, and a
# ladder of increasingly selective FACT filters (predicates on the fact rows, not on
# the PU entity -- see the entity/fact distinction in docs/dp/bound_derivation.md).
SO = "so.Posts WHERE OwnerUserId IS NOT NULL"
DATASETS = {
    "tpch": {
        "from": "tpch.lineitem JOIN tpch.orders ON o_orderkey = l_orderkey",
        "where": "true",
        "pu": "o_custkey",
        "groupbys": None,   # use GROUPBYS
        "measures": None,   # use MEASURES
        "ladder": None,     # use FILTER_LADDER
        "row_table": "tpch.lineitem",
    },
    "cb": {                                   # ClickBench: 1.06M users, median 2 hits, max 2,496
        "from": "cb.hits", "where": "UserID IS NOT NULL", "pu": "UserID",
        "groupbys": {"date": "cast(EventDate as varchar)", "region": "cast(RegionID as varchar)"},
        "measures": {"count": "1", "width": "ResolutionWidth"},
        "ladder": [("no filter", "true"),
                   ("searched", "SearchPhrase <> ''"),
                   ("+ refresh", "SearchPhrase <> '' AND IsRefresh = 1"),
                   ("+ mobile", "SearchPhrase <> '' AND IsRefresh = 1 AND IsMobile = 1")],
        "row_table": "cb.hits",
    },
    "supp": {                                 # TPC-H with PU = supplier: ~600 rows each
        "from": "tpch.lineitem", "where": "true", "pu": "l_suppkey",
        "groupbys": {"month": "strftime(l_shipdate, '%Y-%m')",
                     "year": "cast(year(l_shipdate) as varchar)"},
        "measures": {"price": "l_extendedprice", "quantity": "l_quantity", "count": "1"},
        "ladder": None,          # falls back to FILTER_LADDER via dsconf
        "row_table": "tpch.lineitem",
    },
    "so": {
        "from": "so.Posts",
        "where": "OwnerUserId IS NOT NULL",
        "pu": "OwnerUserId",
        "groupbys": {
            "month": "strftime(CreationDate, '%Y-%m')",
            "year": "cast(year(CreationDate) as varchar)",
            "posttype": "cast(PostTypeId as varchar)",
        },
        "measures": {"count": "1", "views": "coalesce(ViewCount, 0)"},
        "ladder": [
            ("no filter", "true"),
            ("questions only", "PostTypeId = 1"),
            ("+ score > 0", "PostTypeId = 1 AND Score > 0"),
            ("+ views > 500", "PostTypeId = 1 AND Score > 0 AND coalesce(ViewCount,0) > 500"),
            ("+ views > 5000", "PostTypeId = 1 AND Score > 0 AND coalesce(ViewCount,0) > 5000"),
        ],
        "row_table": "so.Posts",
    },
}


CELLS_TPCH = [("price", "month", 0, False), ("price", "quarter", 0, False),
              ("count", "month", 0, False), ("quantity", "month", 0, False),
              ("price", "month", 1.5, False), ("price", "month", 1.0, False),
              ("price", "month", 0, True)]
CELLS_SO = [("count", "month", 0, False), ("count", "year", 0, False),
            ("views", "month", 0, False), ("views", "year", 0, False)]
CELLS_CB = [("count", "date", 0, False), ("count", "region", 0, False),
            ("width", "date", 0, False), ("width", "region", 0, False)]
CELLS_SUPP = [("price", "month", 0, False), ("price", "year", 0, False),
              ("quantity", "month", 0, False), ("count", "month", 0, False)]


def dsconf(args):
    d = DATASETS[args.dataset]
    return (d, d["groupbys"] or GROUPBYS, d["measures"] or MEASURES,
            d["ladder"] or FILTER_LADDER)

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
    d, gbs, ms, _ = dsconf(args)
    gexpr = gbs[args.groupby]
    mexpr = ms[args.measure]
    tcols = ",\n               ".join(
        f"sum(case when {f} then {mexpr} else 0 end) AS t{i}" for i, (_, f) in enumerate(filters)
    )
    con.execute(
        f"""
        CREATE OR REPLACE TABLE contrib AS
        SELECT {d["pu"]} AS pu,
               {gexpr}   AS g,
               sum({mexpr}) AS a,
               {tcols}
        FROM {d["from"]}
        WHERE {d["where"]}
        GROUP BY 1, 2
        """
    )
    n_rows, n_pu, n_groups = con.execute(
        "SELECT count(*), count(distinct pu), count(distinct g) FROM contrib"
    ).fetchone()
    log(f"contrib: {n_rows} (pu,group) rows, {n_pu} PUs, {n_groups} groups")
    if args.pareto:
        # Heavy-tail the per-PU contribution: scale by (1/u)^(1/alpha), u ~ U(0,1) keyed on
        # the PU so it is reproducible. Small alpha = heavier tail = thinner top bins.
        cols = ", ".join([f"a = a * s.f"] + [f"t{i} = t{i} * s.f" for i in range(len(filters))])
        con.execute(
            f"""
            CREATE OR REPLACE TABLE pareto AS
            SELECT pu, pow(1.0 / ((abs(hash(pu)) % 1000000 + 1) / 1000000.0),
                           1.0 / {args.pareto}) AS f
            FROM (SELECT DISTINCT pu FROM contrib)
            """
        )
        con.execute(f"UPDATE contrib SET {cols} FROM pareto s WHERE s.pu = contrib.pu")
        log(f"pareto: alpha={args.pareto}")
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
    if args.no_group_bound:
        # Per-group clipping affects bias only; the l1 norm bound alone is the
        # sensitivity. Dropping it reduces the frozen metadata to a single scalar whose
        # histogram has sensitivity 1 -- much cheaper to make DP.
        con.execute("UPDATE bg SET B = 1e30")
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


def rung_releases(con, args, tcol, d_s, max_shift=16):
    """Bucketed bound selection.

    The frozen per-group bounds and the frozen norm already sit on an exponential
    ladder (note §6: b* = the highest bin with support >= s). Nothing forces the
    mechanism to *clip* at the top rung. Clipping at rung `b* - n` and scaling the
    frozen norm by the same f^n gives a family of releases: a broad query wants n = 0,
    a selective query wants a large n.

    Returns the true per-group sums and one released vector per rung, so both the
    oracle and the DP selection rule below can be scored from the same data.
    """
    # The per-group bound B_g stays at its frozen value for every rung. Only the norm
    # bound moves, because the norm bound alone is the sensitivity — per-group clipping
    # affects bias but not sensitivity once the l1 clip is enforced. Keeping B_g fixed
    # also means the only bias a lower rung introduces is l1 scaling loss, which the
    # noisy norm histogram below can predict.
    con.execute(
        f"""
        CREATE OR REPLACE TABLE kb AS
        SELECT pu, sum(least({tcol}, B)) AS nt
        FROM contrib JOIN bg USING (g)
        WHERE {tcol} > 0 AND g IN (SELECT g FROM gstar)
        GROUP BY pu
        """
    )
    true = None
    releases = []
    for n in range(max_shift):
        div = float(args.f) ** n
        rows = con.execute(
            f"""
            SELECT c.g,
                   sum(c.{tcol}) AS true_sum,
                   sum(least(c.{tcol}, bg.B) * least(1.0, ({d_s} / {div}) / kb.nt)) AS rel
            FROM contrib c JOIN bg USING (g) JOIN kb ON kb.pu = c.pu
            WHERE c.g IN (SELECT g FROM gstar) AND c.{tcol} > 0 AND kb.nt > 0
            GROUP BY c.g ORDER BY c.g
            """
        ).fetchall()
        if not rows:
            break
        arr = np.array([[r[1], r[2]] for r in rows], dtype=float)
        if true is None:
            true = arr[:, 0]
        elif arr.shape[0] != true.size:
            break
        releases.append(arr[:, 1])
    return true, releases


def bucketed_oracle(true, releases, args, d_s, rng):
    """Best rung chosen with full knowledge of the answer, and the whole budget spent
    on the value noise. An upper bound on any real selection rule, not a mechanism."""
    best = None
    for n, rel in enumerate(releases):
        r = score(rel, true, (d_s / float(args.f) ** n) / args.epsilon, args.trials, rng)
        r["rung"] = n
        if best is None or r["total"] < best["total"]:
            best = r
    return best


def bucketed_dp(true, releases, args, d_s, rng):
    """Deployable version: pick the rung with an exponential mechanism over the public
    ladder, then add Laplace noise with the rest of the budget.

    eps = eps_select + eps_value. The score of rung n trades the mass clipped away
    against the noise it saves:

        score(n) = -( clipped_away(n) + groups * D_s / (f^n * eps_value) )

    where clipped_away(n) = sum_g (release_0(g) - release_n(g)). One PU's influence on
    that term is at most its l1-clipped norm, i.e. at most D_s, and the noise term is
    data-independent (D_s, f, n and the group count are all frozen metadata). So the
    score sensitivity is D_s and the exponential mechanism is eps_select-DP:

        P(n) ~ exp( eps_select * score(n) / (2 * D_s) )

    The rung can never exceed the frozen top, and it moves in factor-f steps, so an
    analyst can steer only downward within a capped, coarse range — unlike Wilson's
    freshly derived bound. Like everything else here, this is DP relative to the frozen
    metadata (Assumption 8.1), not on top of it.
    """
    eps_sel = args.eps_select_frac * args.epsilon
    eps_val = args.epsilon - eps_sel
    n_groups = true.size
    scales = [(d_s / float(args.f) ** n) / eps_val for n in range(len(releases))]
    clipped_away = np.array([float(np.sum(releases[0] - r)) for r in releases])
    noise_total = np.array([n_groups * s for s in scales])
    utility = -(clipped_away + noise_total)

    logits = eps_sel * utility / (2.0 * d_s)
    logits -= logits.max()
    probs = np.exp(logits)
    probs /= probs.sum()

    picks = rng.choice(len(releases), size=args.trials, p=probs)
    acc = np.zeros(n_groups)
    for n in picks:
        noise = rng.laplace(0.0, scales[n], size=n_groups)
        acc += np.abs(releases[n] + noise - true) / true
    acc /= args.trials
    return {
        "total": float(np.median(acc)),
        "rung": int(np.argmax(np.bincount(picks, minlength=len(releases)))),
        "rung_prob": float(probs.max()),
        "eps_sel": eps_sel,
    }


def norm_histogram(con, args, tcol):
    """Histogram of the per-PU filtered norms over the frozen ladder. Each PU falls in
    exactly one bin, so one PU changes one count by one: L1 sensitivity 1."""
    rows = con.execute(
        f"""
        WITH n AS (
            SELECT pu, sum(least({tcol}, B)) AS nt
            FROM contrib JOIN bg USING (g)
            WHERE {tcol} > 0 AND g IN (SELECT g FROM gstar)
            GROUP BY pu
        )
        SELECT cast(floor(ln(nt) / ln({args.f})) AS INTEGER) AS bin, count(*) AS c
        FROM n WHERE nt > 0 GROUP BY 1
        """
    ).fetchall()
    return {int(b): float(c) for b, c in rows}


def bucketed_dp_hist(true, releases, hist, args, d_s, rng):
    """Rung selection from a noisy histogram of the per-PU norms — the rule that works.

    Two problems with the mass-scored exponential mechanism above: its per-PU
    sensitivity is D_s, the same order as the score differences, so it picks close to at
    random. Scoring on a raw count quantile instead has sensitivity 1 but ignores the
    noise/bias tradeoff and over-clips.

    This rule gets both. Pay eps_select ONCE for a Laplace-noised histogram of the
    per-PU filtered norms over the frozen ladder (each PU in exactly one bin →
    sensitivity 1, so the noise is tiny). Then do the whole optimisation on that noisy
    histogram, which costs nothing further: estimate the mass a candidate rung would
    clip away by treating the PUs in bin b as sitting at the geometric midpoint of the
    bin, and trade it against the noise the rung saves.

        est_clipped(n) = sum_b  noisy_count_b * max(0, f^(b+0.5) - D_s/f^n)
        est_noise(n)   = groups * (D_s / f^n) / eps_value
        pick n minimising est_clipped(n) + est_noise(n)

    This is Wilson's APPROX_BOUNDS objective confined to the frozen public ladder and
    capped by the frozen rung: the analyst can steer only downward, in factor-f steps,
    inside a range fixed before the query was written.
    """
    eps_sel = args.eps_select_frac * args.epsilon
    eps_val = args.epsilon - eps_sel
    n_groups = true.size
    levels = np.array([d_s / float(args.f) ** n for n in range(len(releases))])
    bins = sorted(hist)
    counts = np.array([hist[b] for b in bins])
    mids = np.array([float(args.f) ** (b + 0.5) for b in bins])
    est_noise = n_groups * levels / eps_val

    acc = np.zeros(n_groups)
    picks = []
    for _ in range(args.trials):
        noisy = np.maximum(counts + rng.laplace(0.0, 1.0 / eps_sel, size=counts.size), 0.0)
        est_clipped = np.array([float(np.sum(noisy * np.maximum(mids - lvl, 0.0))) for lvl in levels])
        chosen = int(np.argmin(est_clipped + est_noise))
        picks.append(chosen)
        scale = (d_s / float(args.f) ** chosen) / eps_val
        acc += np.abs(releases[chosen] + rng.laplace(0.0, scale, size=n_groups) - true) / true
    acc /= args.trials
    return {
        "total": float(np.median(acc)),
        "rung": int(np.bincount(picks, minlength=len(releases)).argmax()),
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


def selectivity(con, args, filt):
    d, _, _, _ = dsconf(args)
    return con.execute(
        f"SELECT avg(case when {filt} then 1.0 else 0.0 end) FROM {d['row_table']} "
        f"WHERE {d['where']}"
    ).fetchone()[0]


def header(args, delta1, d_s, n_kept, n_groups):
    print()
    _, _, ms, _ = dsconf(args)
    print(f"measure=SUM({ms[args.measure]})  group-by={args.groupby}  eps={args.epsilon}")
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
        f"{'filter':<22}{'sel.':>7}{'grp':>5}{'fless':>8}{'l1crowd':>9}"
        f"{'oracle':>8}{'rung':>5}{'dp-mass':>9}{"dp-hist":>9}{'rung':>5}{'google':>8}"
    )
    print("-" * 93)
    for i, (label, filt) in enumerate(FILTER_LADDER):
        sel = selectivity(con, args, filt)
        res = evaluate_filter(con, args, f"t{i}", delta1, d_s, rng)
        if res is None:
            print(f"{label:<22}{sel:>6.2%}{'0':>5}{'(no groups released)':>45}")
            continue
        m = res["mechs"]
        cols = f"{'—':>8}{'—':>5}{'—':>9}{'—':>8}{'—':>5}"
        if args.bucketed:
            true, releases = rung_releases(con, args, f"t{i}", d_s)
            orc = bucketed_oracle(true, releases, args, d_s, rng)
            dpm = bucketed_dp(true, releases, args, d_s, rng)
            hist = norm_histogram(con, args, f"t{i}")
            dpc = bucketed_dp_hist(true, releases, hist, args, d_s, rng)
            cols = (f"{orc['total']:>7.1%}{orc['rung']:>5}{dpm['total']:>8.1%}"
                    f"{dpc['total']:>8.1%}{dpc['rung']:>5}")
        print(
            f"{label:<22}{sel:>6.2%}{res['n_groups']:>5}"
            f"{m['filterless']['total']:>7.1%}{m['l1' if False else 'fl_l1crowd']['total']:>8.1%}"
            f"{cols}{m['google']['total']:>7.1%}"
        )
    print()
    if args.bucketed:
        print("oracle = best rung chosen with full knowledge, whole budget on the value")
        print("         noise. an upper bound, not a mechanism.")
        print("dp-mass = exponential mechanism scored on clipped mass (sensitivity D_s)")
        print("dp-hist = noisy norm histogram (sensitivity 1) + tradeoff optimisation, "
              f"{args.clip_tolerance:.1%} of PUs clipped")
        print(f"both spend eps_select = "
              f"{args.eps_select_frac:.0%} of eps,")
        print("         remainder on the value noise. rung 0 = the frozen top bound;")
        print("         rung n = that bound divided by f^n.")
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


def choose_rung(noisy, mids, levels, est_noise):
    est_clipped = np.array([float(np.sum(noisy * np.maximum(mids - lvl, 0.0))) for lvl in levels])
    return int(np.argmin(est_clipped + est_noise))


def run_rung_attack(con, args):
    """Membership inference on the *rung*.

    The bucketed hybrid picks the clipping rung from a noisy histogram of the filtered
    per-PU norms, so the bound is query-dependent again and Rem. 3.1 no longer applies
    by construction. This tests whether that channel actually leaks.

    Sharpest form of the filter-construction attack: let the analyst restrict the query
    to a PU set S of their choosing, shrinking S around one target. Run the rung
    selection with the target in S and with it removed, and try to tell the two apart
    from the released rung alone. 50% = no signal.

    The frozen metadata (B_g, D_s) is built once from the full domain and reused for
    every population size, as the design requires.
    """
    build_contributions(con, args, [("full", "true")])
    delta1, d_s, n_kept = build_metadata(con, args)
    target, target_na = con.execute(
        "SELECT pu, na FROM norms ORDER BY na DESC LIMIT 1"
    ).fetchone()
    eps_sel = args.eps_select_frac * args.epsilon
    eps_val = args.epsilon - eps_sel
    n_rungs = 16
    levels = np.array([d_s / float(args.f) ** n for n in range(n_rungs)])
    est_noise = n_kept * levels / eps_val
    rng = np.random.default_rng(args.seed)

    print()
    print("membership inference on the chosen rung")
    print(f"target = PU {target} (full-domain norm {target_na:,.0f}), D_s = {d_s:,.0f}")
    print(f"eps_select = {eps_sel} (histogram sensitivity 1), s = {args.s}, f = {args.f}")
    print()
    print(f"{'|S|':>10}{'mean rung in':>15}{'mean rung out':>15}{'attack acc':>13}")
    print("-" * 53)
    for pop in args.rung_attack_pops:
        pop = int(pop)
        con.execute(
            f"""
            CREATE OR REPLACE TABLE sel AS
            SELECT pu, na FROM norms WHERE pu <> {target} ORDER BY hash(pu) LIMIT {max(pop - 1, 0)}
            """
        )
        con.execute(f"INSERT INTO sel SELECT pu, na FROM norms WHERE pu = {target}")
        rows = con.execute(
            f"""
            SELECT cast(floor(ln(na) / ln({args.f})) AS INTEGER) AS bin, count(*) AS c
            FROM sel WHERE na > 0 GROUP BY 1 ORDER BY 1
            """
        ).fetchall()
        bins = [int(b) for b, _ in rows]
        counts_in = np.array([float(c) for _, c in rows])
        mids = np.array([float(args.f) ** (b + 0.5) for b in bins])
        tbin = int(np.floor(np.log(target_na) / np.log(args.f)))
        counts_out = counts_in.copy()
        if tbin in bins:
            counts_out[bins.index(tbin)] -= 1.0

        r_in, r_out = [], []
        for _ in range(args.trials):
            noisy = np.maximum(counts_in + rng.laplace(0.0, 1.0 / eps_sel, size=counts_in.size), 0.0)
            r_in.append(choose_rung(noisy, mids, levels, est_noise))
            noisy = np.maximum(counts_out + rng.laplace(0.0, 1.0 / eps_sel, size=counts_out.size), 0.0)
            r_out.append(choose_rung(noisy, mids, levels, est_noise))
        r_in, r_out = np.array(r_in), np.array(r_out)
        acc = max(
            max(
                (np.mean(r_in >= t) + np.mean(r_out < t)) / 2.0,
                (np.mean(r_in < t) + np.mean(r_out >= t)) / 2.0,
            )
            for t in range(n_rungs + 1)
        )
        print(f"{pop:>10,}{r_in.mean():>15.2f}{r_out.mean():>15.2f}{acc:>12.1%}")
    print()
    print("50% = the rung carries no membership signal. The histogram counts have")
    print("sensitivity 1, so one PU moves a count by 1 against Laplace(1/eps_select).")
    print()


def _released_ladder(con, args, tcol, d_s):
    """Rung ladder + norm histogram for whatever is currently in `contrib`, using the
    metadata tables as they currently stand."""
    true, releases = rung_releases(con, args, tcol, d_s)
    hist = norm_histogram(con, args, tcol)
    return true, releases, hist


def _sample_release(releases, hist, args, d_s, n_groups, rng, trials):
    """Run the full fixed mechanism `trials` times: noisy histogram -> rung -> Laplace.
    Returns the released total per trial and the rungs chosen."""
    eps_sel = args.eps_select_frac * args.epsilon
    eps_val = args.epsilon - eps_sel
    levels = np.array([d_s / float(args.f) ** n for n in range(len(releases))])
    est_noise = n_groups * levels / eps_val
    bins = sorted(hist)
    counts = np.array([hist[b] for b in bins])
    mids = np.array([float(args.f) ** (b + 0.5) for b in bins])
    totals, rungs = [], []
    for _ in range(trials):
        noisy = np.maximum(counts + rng.laplace(0.0, 1.0 / eps_sel, size=counts.size), 0.0)
        n = choose_rung(noisy, mids, levels, est_noise)
        scale = levels[n] / eps_val
        totals.append(float(np.sum(releases[n] + rng.laplace(0.0, scale, size=releases[n].size))))
        rungs.append(n)
    return np.array(totals), np.array(rungs)


def _best_threshold_accuracy(a, b, n_steps=200):
    """Best-threshold classifier accuracy for telling sample `a` from sample `b`."""
    lo, hi = min(a.min(), b.min()), max(a.max(), b.max())
    if hi <= lo:
        return 0.5
    best = 0.5
    for t in np.linspace(lo, hi, n_steps):
        best = max(best, (np.mean(a >= t) + np.mean(b < t)) / 2.0,
                   (np.mean(a < t) + np.mean(b >= t)) / 2.0)
    return float(best)


def run_suite(con, args):
    """Attacks against the *fixed* mechanism (CROWD norm + l1 clip + dp-hist rung)."""
    rng = np.random.default_rng(args.seed)
    build_contributions(con, args, [("query", args.filter)])
    con.execute("CREATE OR REPLACE TABLE contrib_full AS SELECT * FROM contrib")
    d1_in, ds_in, nk_in = build_metadata(con, args)
    target, target_na = con.execute("SELECT pu, na FROM norms ORDER BY na DESC LIMIT 1").fetchone()

    # ---- A. does the frozen metadata itself leak the PU that defines it? -------------
    # Assumption 8.1 treats the metadata as fixed and public. If it is public, then any
    # metadata value that MOVES when one PU is removed reveals that PU's membership
    # outright, with no noise in the way. This is the test eq. (25) has to pass.
    true_in, rel_in, hist_in = _released_ladder(con, args, "t0", ds_in)
    con.execute(f"DELETE FROM contrib WHERE pu = {target}")
    d1_out, ds_out, nk_out = build_metadata(con, args)
    true_out, rel_out, hist_out = _released_ladder(con, args, "t0", ds_in)

    print()
    print(f"target = PU {target} (full-domain norm {target_na:,.0f})")
    print(f"filter = {args.filter}   group-by = {args.groupby}   eps = {args.epsilon}")
    print()
    print("A. metadata channel: does removing the target move a published bound?")
    print(f"{'quantity':<28}{'target in':>18}{'target out':>18}{'leaks?':>9}")
    print("-" * 73)
    for name, vin, vout in [
        ("Delta1 (eq.25, max_u)", d1_in, d1_out),
        ("D_s (CROWD norm, fix)", ds_in, ds_out),
        ("groups in G*", float(nk_in), float(nk_out)),
    ]:
        leaks = "YES" if vin != vout else "no"
        print(f"{name:<28}{vin:>18,.0f}{vout:>18,.0f}{leaks:>9}")
    print()

    # ---- B. end-to-end MIA on the released answer, metadata frozen -------------------
    n_groups = min(true_in.size, true_out.size)
    tot_in, rung_in = _sample_release(rel_in, hist_in, args, ds_in, n_groups, rng, args.trials)
    tot_out, rung_out = _sample_release(rel_out, hist_out, args, ds_in, n_groups, rng, args.trials)
    acc_val = _best_threshold_accuracy(tot_in, tot_out)
    acc_rung = _best_threshold_accuracy(rung_in.astype(float), rung_out.astype(float))
    print("B. end-to-end MIA on the released answer (metadata frozen, per Assumption 8.1)")
    print(f"{'statistic':<28}{'accuracy':>12}")
    print("-" * 40)
    print(f"{'released total':<28}{acc_val:>11.1%}")
    print(f"{'chosen rung':<28}{acc_rung:>11.1%}")
    print()

    # ---- C. repeated queries: the rung is re-sampled every time ---------------------
    # Each repetition draws a fresh noisy histogram, so an analyst who reruns the query
    # can average the channel down. Under honest accounting each repeat costs another
    # eps_select; this measures what an implementation that forgets to cache the rung
    # would give away.
    print("C. repeated queries, averaging the rung channel (each repeat re-samples it)")
    print(f"{'repeats':>10}{'accuracy':>12}{'eps_select spent':>20}")
    print("-" * 42)
    eps_sel = args.eps_select_frac * args.epsilon
    for r in args.repeat_ladder:
        r = int(r)
        m_in = np.array([rung_in[i:i + r].mean() for i in range(0, len(rung_in) - r + 1, max(r, 1))])
        m_out = np.array([rung_out[i:i + r].mean() for i in range(0, len(rung_out) - r + 1, max(r, 1))])
        if m_in.size < 8 or m_out.size < 8:
            print(f"{r:>10}{'(too few trials)':>12}")
            continue
        print(f"{r:>10}{_best_threshold_accuracy(m_in, m_out):>11.1%}{r * eps_sel:>20.1f}")
    print()

    # ---- D. can a small coalition force the rung back to the top? ------------------
    # The rung objective is count-weighted-by-mass, and the counts have sensitivity 1,
    # so they are cheap to move: k injected fat+wide PUs add k * (their mass) to the
    # estimated clipping cost of every low rung and push the choice back to rung 0 --
    # i.e. back to the pre-fix noise. This needs far fewer than s colluders.
    print("D. rung DoS: k injected fat+wide PUs (no s-sized coalition needed)")
    print(f"{'k':>8}{'rung chosen':>14}{'noise scale':>16}{'median rel err':>17}")
    print("-" * 55)
    for k in args.rung_dos_ladder:
        k = int(k)
        con.execute("CREATE OR REPLACE TABLE contrib AS SELECT * FROM contrib_full")
        if k > 0:
            con.execute(
                f"""
                INSERT INTO contrib
                SELECT -c.i, m.g, m.a * {args.rung_dos_scale}, m.t0 * {args.rung_dos_scale}
                FROM (SELECT unnest(range(1, {k} + 1)) AS i) c
                CROSS JOIN (SELECT g, median(a) AS a, median(t0) AS t0 FROM contrib GROUP BY g) m
                """
            )
        _, ds_k, nk_k = build_metadata(con, args)
        true_k, rel_k, hist_k = _released_ladder(con, args, "t0", ds_k)
        # score only the honest groups' error, against the honest truth
        tots, rungs = _sample_release(rel_k, hist_k, args, ds_k, true_k.size, rng, 50)
        mode = int(np.bincount(rungs, minlength=len(rel_k)).argmax())
        scale = (ds_k / float(args.f) ** mode) / (args.epsilon - eps_sel)
        err = score(rel_k[mode], true_k, scale, 50, rng)
        print(f"{k:>8}{mode:>14}{scale:>16,.0f}{err['total']:>16.1%}")
    print()


def run_knife(con, args):
    """The residual leak, and whether a noised threshold closes it.

    D_s = top of the highest bin whose distinct-PU count reaches s. That is a HARD
    threshold on a count, so if the deciding bin holds exactly s members, removing one PU
    drops D_s by a factor f. D_s is public under Assumption 8.1, so that is a
    deterministic membership test — accuracy 1.0, no noise in the way.

    Fix: decide the bin from NOISED counts, exactly the tau-thresholding the extension
    already does for partition selection (privacy_mechanisms.cpp:ComputeWilsonPartitionThreshold).
    Counts have sensitivity 1, so Laplace(1/eps_meta) plus a margin on tau makes the
    decision (eps_meta, delta_meta)-DP. Crucially this is paid ONCE for the whole session,
    not per query, because the metadata is frozen and shared by every query in the family.

    This builds the knife-edge deliberately: take the real norm histogram and add an edge
    bin holding exactly s members, then remove one of them.
    """
    build_contributions(con, args, [("query", args.filter)])
    _, d_s, _ = build_metadata(con, args)
    hist = norm_histogram(con, args, "t0")
    bins = sorted(hist)
    top = max(b for b in bins if hist[b] >= args.s)
    edge = top + 3  # an engineered bin well above the honest top
    rng = np.random.default_rng(args.seed)

    def hard_rule(counts):
        ok = [b for b, c in counts.items() if c >= args.s]
        return float(args.f) ** (max(ok) + 1) if ok else 0.0

    def noisy_rule(counts, eps_meta, tau):
        ok = [b for b, c in counts.items() if c + rng.laplace(0.0, 1.0 / eps_meta) >= tau]
        return float(args.f) ** (max(ok) + 1) if ok else 0.0

    c_in = dict(hist)
    c_in[edge] = float(args.s)          # edge bin exactly at the threshold
    c_out = dict(c_in)
    c_out[edge] = float(args.s - 1)     # one PU removed

    print()
    print("residual leak: the deciding bin sits exactly at s")
    print(f"honest top bin = {top} (D_s = {d_s:,.0f}), engineered edge bin = {edge} "
          f"(D_s would be {float(args.f) ** (edge + 1):,.0f})")
    print(f"s = {args.s}, f = {args.f}")
    print()
    print(f"{'rule':<34}{'D_s in':>16}{'D_s out':>16}{'MIA acc':>10}")
    print("-" * 76)
    hi, ho = hard_rule(c_in), hard_rule(c_out)
    print(f"{'hard count >= s (the note)':<34}{hi:>16,.0f}{ho:>16,.0f}"
          f"{(1.0 if hi != ho else 0.5):>9.1%}")
    for eps_meta in args.knife_eps:
        for margin in args.knife_margins:
            tau = args.s + margin / eps_meta
            a = np.array([noisy_rule(c_in, eps_meta, tau) for _ in range(args.trials)])
            b = np.array([noisy_rule(c_out, eps_meta, tau) for _ in range(args.trials)])
            acc = _best_threshold_accuracy(a, b)
            label = f"noisy tau, eps_meta={eps_meta}, m={margin}"
            print(f"{label:<34}{a.mean():>16,.0f}{b.mean():>16,.0f}{acc:>9.1%}")
    print()
    print("eps_meta is spent ONCE for the whole session, not per query: the metadata is")
    print("frozen and shared by every query in the family. Wilson pays for its bounds on")
    print("every query instead.")
    print()


def run_suite2(con, args):
    """Three further attacks: the group universe's threshold, rung composition across a
    crafted filter family, and MIA on a single small group (which the median-over-groups
    metric used everywhere else would hide)."""
    rng = np.random.default_rng(args.seed)
    eps_sel = args.eps_select_frac * args.epsilon
    eps_val = args.epsilon - eps_sel

    # ---- E. the group universe is the SAME hard threshold as D_s --------------------
    # G* = {g : distinct PUs >= s}. If a group sits exactly at s, removing one PU makes
    # the whole group vanish from the output. Group presence is directly observable, so
    # that is a deterministic membership test -- the same bug as §12 in a second place.
    build_contributions(con, args, [("query", args.filter)])
    counts = [
        float(c)
        for (c,) in con.execute(
            "SELECT count(*) FROM contrib WHERE a > 0 GROUP BY g ORDER BY 1"
        ).fetchall()
    ]
    print()
    print("E. group universe: engineered group sitting exactly at s")
    print(f"real per-group PU counts range {min(counts):,.0f}–{max(counts):,.0f}, s = {args.s}")
    print()
    print(f"{'rule':<38}{'P(released | in)':>18}{'P(rel | out)':>15}{'MIA acc':>10}")
    print("-" * 81)
    p_in, p_out = 1.0, 0.0  # hard rule: s >= s is released, s-1 is not
    print(f"{'hard count >= s (the note, §5)':<38}{p_in:>17.1%}{p_out:>14.1%}{1.0:>9.1%}")
    for eps_meta in args.knife_eps:
        for margin in args.knife_margins:
            tau = args.s + margin / eps_meta
            a = np.mean(args.s + rng.laplace(0.0, 1.0 / eps_meta, args.trials) >= tau)
            b = np.mean(args.s - 1 + rng.laplace(0.0, 1.0 / eps_meta, args.trials) >= tau)
            acc = 0.5 + abs(a - b) / 2.0
            label = f"noisy tau, eps_meta={eps_meta}, m={margin}"
            print(f"{label:<38}{a:>17.1%}{b:>14.1%}{acc:>9.1%}")
    print()

    # ---- F. rung composition across a crafted filter family ------------------------
    # The rung is chosen per query, so an analyst issuing Q filters observes Q rungs.
    # Each carries almost nothing, but they compose. Statistic = sum of the rungs.
    fam = [(f"q<{k}", f"{BASE} and l_quantity < {k}") for k in range(2, args.family_size + 2)]
    build_contributions(con, args, fam)
    _, d_s, n_kept = build_metadata(con, args)
    target = con.execute("SELECT pu FROM norms ORDER BY na DESC LIMIT 1").fetchone()[0]
    levels = np.array([d_s / float(args.f) ** n for n in range(16)])
    est_noise = n_kept * levels / eps_val

    hists_in, hists_out = [], []
    for i in range(len(fam)):
        h = norm_histogram(con, args, f"t{i}")
        if not h:
            continue
        tna = con.execute(
            f"""
            SELECT sum(least(t{i}, B)) FROM contrib JOIN bg USING (g)
            WHERE pu = {target} AND g IN (SELECT g FROM gstar)
            """
        ).fetchone()[0]
        bins = sorted(h)
        cin = np.array([h[b] for b in bins])
        cout = cin.copy()
        if tna and tna > 0:
            tb = int(np.floor(np.log(tna) / np.log(args.f)))
            if tb in bins:
                cout[bins.index(tb)] -= 1.0
        mids = np.array([float(args.f) ** (b + 0.5) for b in bins])
        hists_in.append((cin, mids))
        hists_out.append((cout, mids))

    print("F. rung composition: analyst issues Q queries and reads Q rungs")
    print(f"{'Q':>6}{'MIA acc':>12}{'eps_select spent':>20}")
    print("-" * 38)
    for q in args.family_ladder:
        q = int(min(q, len(hists_in)))
        if q < 1:
            continue
        s_in, s_out = [], []
        for _ in range(args.trials):
            tot_i = tot_o = 0
            for j in range(q):
                cin, mids = hists_in[j]
                cout, _ = hists_out[j]
                tot_i += choose_rung(
                    np.maximum(cin + rng.laplace(0.0, 1.0 / eps_sel, cin.size), 0.0),
                    mids, levels, est_noise)
                tot_o += choose_rung(
                    np.maximum(cout + rng.laplace(0.0, 1.0 / eps_sel, cout.size), 0.0),
                    mids, levels, est_noise)
            s_in.append(tot_i)
            s_out.append(tot_o)
        acc = _best_threshold_accuracy(np.array(s_in, float), np.array(s_out, float))
        print(f"{q:>6}{acc:>11.1%}{q * eps_sel:>20.1f}")
    print()

    # ---- G. MIA on the smallest released group -------------------------------------
    # Every other table reports the MEDIAN over groups, which hides small groups. Here
    # the target is the largest contributor to the smallest released group, and the
    # statistic is that one group's released value.
    build_contributions(con, args, [("query", args.filter)])
    _, d_s, n_kept = build_metadata(con, args)
    small_g = con.execute(
        """
        SELECT g FROM contrib WHERE t0 > 0 AND g IN (SELECT g FROM gstar)
        GROUP BY g ORDER BY sum(t0) ASC LIMIT 1
        """
    ).fetchone()[0]
    tgt = con.execute(
        f"SELECT pu FROM contrib WHERE g = '{small_g}' AND t0 > 0 ORDER BY t0 DESC LIMIT 1"
    ).fetchone()[0]
    rung = 0
    hist = norm_histogram(con, args, "t0")
    bins = sorted(hist)
    counts_h = np.array([hist[b] for b in bins])
    mids = np.array([float(args.f) ** (b + 0.5) for b in bins])
    levels = np.array([d_s / float(args.f) ** n for n in range(16)])
    est_noise = n_kept * levels / eps_val
    rung = choose_rung(counts_h, mids, levels, est_noise)
    scale = levels[rung] / eps_val

    def group_value(exclude):
        where = f" AND c.pu <> {exclude}" if exclude else ""
        return float(
            con.execute(
                f"""
                WITH kb AS (
                    SELECT pu, sum(least(t0, B)) AS nt FROM contrib JOIN bg USING (g)
                    WHERE t0 > 0 AND g IN (SELECT g FROM gstar) GROUP BY pu
                )
                SELECT sum(least(c.t0, bg.B) * least(1.0, {levels[rung]} / kb.nt))
                FROM contrib c JOIN bg USING (g) JOIN kb ON kb.pu = c.pu
                WHERE c.g = '{small_g}' AND c.t0 > 0{where}
                """
            ).fetchone()[0]
            or 0.0
        )

    v_in, v_out = group_value(None), group_value(tgt)
    a = v_in + rng.laplace(0.0, scale, args.trials)
    b = v_out + rng.laplace(0.0, scale, args.trials)
    print("G. MIA on the smallest released group (not the median)")
    print(f"group {small_g}, target PU {tgt} = its largest contributor, rung {rung}")
    print(f"{'quantity':<34}{'value':>18}")
    print("-" * 52)
    print(f"{'group value, target in':<34}{v_in:>18,.0f}")
    print(f"{'group value, target out':<34}{v_out:>18,.0f}")
    print(f"{'target contribution':<34}{v_in - v_out:>18,.0f}")
    print(f"{'noise scale':<34}{scale:>18,.0f}")
    print(f"{'MIA accuracy':<34}{_best_threshold_accuracy(a, b):>17.1%}")
    print()


def wilson_tau(eps_eta, delta_eta, cu):
    """Wilson et al.'s partition-selection threshold, as implemented in
    privacy_mechanisms.cpp:ComputeWilsonPartitionThreshold:
        tau = 1 - C_u * log(2 - 2(1 - delta_eta)^(1/C_u)) / eps_eta
    The claim it encodes: a group that exists only because of ONE privacy unit is
    released with probability at most delta_eta."""
    inner = 2.0 - 2.0 * (1.0 - delta_eta) ** (1.0 / cu)
    if inner <= 0.0:
        return float("inf")
    return 1.0 - (cu * np.log(inner)) / eps_eta


def run_partition(con, args):
    """Attacks on the partition-selection channel (which group keys get released)."""
    rng = np.random.default_rng(args.seed)
    eps_eta = args.partition_eps

    # ---- P1. where is the channel actually live? -------------------------------------
    # Release decision is `count + Laplace(1/eps_eta) >= tau`. A group only leaks when its
    # count is near tau; far below it is always suppressed, far above always released.
    tau = args.s + args.knife_margins[-1] / eps_eta
    print()
    print("P1. leak profile vs group size (release decision, one group)")
    print(f"rule: count + Laplace(1/{eps_eta}) >= tau = {tau:.1f}   (s = {args.s})")
    print()
    print(f"{'group size':>12}{'P(release)':>13}{'MIA acc for that PU':>22}")
    print("-" * 47)
    for k in args.partition_sizes:
        k = float(k)
        p_in = float(np.mean(k + rng.laplace(0.0, 1.0 / eps_eta, args.trials) >= tau))
        p_out = float(np.mean(k - 1 + rng.laplace(0.0, 1.0 / eps_eta, args.trials) >= tau))
        print(f"{k:>12,.0f}{p_in:>12.1%}{0.5 + abs(p_in - p_out) / 2.0:>21.1%}")
    print()

    # ---- P2. multi-group amplification: why C_u exists -------------------------------
    # One PU can sit in many groups. If several of them are near tau, removing the PU
    # flips several decisions at once and the analyst sees coordinated disappearances.
    # This is exactly what Wilson's C_u accounts for -- and the revised filterless note
    # drops C_u, because the *value* channel no longer needs it.
    print("P2. amplification: target sits in m borderline groups, statistic = #released")
    print(f"{'m':>6}{'Laplace(1/eps)':>18}{'Laplace(m/eps) + Wilson tau':>30}")
    print("-" * 54)
    for m in args.partition_groups:
        m = int(m)
        k = tau  # worst case: every one of the m groups sits right at the threshold
        naive_in, naive_out, safe_in, safe_out = [], [], [], []
        tau_w = wilson_tau(eps_eta, args.partition_delta, m)
        for _ in range(args.trials):
            naive_in.append(np.sum(k + rng.laplace(0.0, 1.0 / eps_eta, m) >= tau))
            naive_out.append(np.sum(k - 1 + rng.laplace(0.0, 1.0 / eps_eta, m) >= tau))
            safe_in.append(np.sum(k + rng.laplace(0.0, m / eps_eta, m) >= tau_w))
            safe_out.append(np.sum(k - 1 + rng.laplace(0.0, m / eps_eta, m) >= tau_w))
        a1 = _best_threshold_accuracy(np.array(naive_in, float), np.array(naive_out, float))
        a2 = _best_threshold_accuracy(np.array(safe_in, float), np.array(safe_out, float))
        print(f"{m:>6}{a1:>17.1%}{a2:>29.1%}")
    print()

    # ---- P3. does the shipped Wilson tau deliver its delta_eta? ---------------------
    # The guarantee: a group that exists only because of one PU is released with
    # probability at most delta_eta. Measured directly against the formula the extension
    # already uses. Large delta values so the event is observable at this trial count.
    print("P3. validating ComputeWilsonPartitionThreshold: P(release | count = 1) <= delta_eta")
    print(f"{'eps_eta':>9}{'delta_eta':>12}{'C_u':>6}{'tau':>12}{'measured P':>13}{'holds':>8}")
    print("-" * 60)
    big = max(args.trials * 50, 200000)
    for eps in args.partition_eps_ladder:
        for delta in args.partition_delta_ladder:
            for cu in args.partition_cu_ladder:
                cu = int(cu)
                t = wilson_tau(eps, delta, cu)
                p = float(np.mean(1.0 + rng.laplace(0.0, cu / eps, big) >= t))
                ok = "yes" if p <= delta * 1.25 else "NO"
                print(f"{eps:>9}{delta:>12.0e}{cu:>6}{t:>12.1f}{p:>12.2e}{ok:>8}")
    print()
    print("tau grows like C_u*log(1/delta)/eps_eta, so with no C_u cap and PUs that touch")
    print("many groups the threshold suppresses everything. Partition selection still needs")
    print("a cross-group cap even though the value channel no longer does.")
    print()


def smooth_es(mf, beta):
    """FLEX smooth elastic sensitivity, mirroring
    privacy_mechanisms.cpp:ComputeSmoothElasticSensitivity:
        SES_beta = max_{k>=0} prod_i(mf_i + k) * exp(-beta*k)"""
    mf = np.asarray(mf, dtype=float)
    best = float(np.prod(mf))
    k_max = int(len(mf) / beta + 200.0)
    for k in range(1, k_max + 1):
        decay = np.exp(-beta * k)
        if decay < 1e-15:
            break
        best = max(best, float(np.prod(mf + k) * decay))
    return best


def crowd_level(freqs, f, s, eps_meta, margin, rng, trials=1):
    """The CROWD rule applied to a frequency distribution: top of the highest power-of-f
    bin whose (noised) count clears tau. Returns one draw, or an array of `trials`."""
    freqs = np.asarray(freqs, dtype=float)
    pos = freqs[freqs > 0]
    bins = np.floor(np.log(pos) / np.log(f)).astype(int)
    uniq, counts = np.unique(bins, return_counts=True)
    tau = s + margin / eps_meta if eps_meta else s
    out = []
    for _ in range(trials):
        noisy = counts + (rng.laplace(0.0, 1.0 / eps_meta, counts.size) if eps_meta else 0.0)
        ok = uniq[noisy >= tau]
        out.append(float(f) ** (ok.max() + 1) if ok.size else 0.0)
    return np.array(out)


def run_elastic(con, args):
    """Does the CROWD-ladder recipe generalise to dp_elastic's join max-frequencies?

    dp_elastic derives its noise from mf_K = MAX(count) per FK hop (ComputeMfK,
    privacy_mechanisms.cpp:463) — a bare max over units, structurally the same shape as the
    note's eq. (25). It is repaired by smoothing (2*SES_beta), which is a sanctioned
    repair with its own theorem. The question here is whether the ladder recipe is a
    better repair on utility, and whether smoothing is actually robust to one fat unit
    setting the level.

    Three variants of the noise multiplier for COUNT over the FK chain:
      raw        prod(mf)          data-dependent, no repair
      smoothed   2 * SES_beta      as shipped, beta = eps/(2 ln(2/delta))
      crowd      prod(mf_crowd)    highest supported power-of-f bin per hop, noised tau
    """
    rng = np.random.default_rng(args.seed)
    hop1 = np.array(
        [r[0] for r in con.execute(
            "SELECT count(*) FROM tpch.orders GROUP BY o_custkey").fetchall()],
        dtype=float)
    hop2 = np.array(
        [r[0] for r in con.execute(
            "SELECT count(*) FROM tpch.lineitem GROUP BY l_orderkey").fetchall()],
        dtype=float)
    beta = args.epsilon / (2.0 * np.log(2.0 / args.elastic_delta))
    print()
    print("dp_elastic join max-frequencies: three repairs for the same parameter")
    print(f"chain lineitem -> orders -> customer, eps = {args.epsilon}, "
          f"delta = {args.elastic_delta:g}, beta = {beta:.4f}")
    print(f"CROWD: f = {args.f}, s = {args.s}, eps_meta = {args.knife_eps[-1]}, margin = 3")
    print()
    print(f"{'scenario':<26}{'mf hop1':>10}{'raw prod':>11}{'2*SES_b':>12}"
          f"{'crowd prod':>12}{'crowd/smooth':>14}")
    print("-" * 85)
    eps_meta = args.knife_eps[-1]
    for label, mult in args.elastic_outliers:
        h1 = hop1.copy()
        if mult > 1:
            h1 = np.append(h1, h1.max() * mult)  # one injected fat unit
        mf = [h1.max(), hop2.max()]
        raw = float(np.prod(mf))
        sm = 2.0 * smooth_es(mf, beta)
        cr = float(
            crowd_level(h1, args.f, args.s, eps_meta, 3.0, rng)[0]
            * crowd_level(hop2, args.f, args.s, eps_meta, 3.0, rng)[0]
        )
        ratio = cr / sm if sm else float("nan")
        print(f"{label:<26}{mf[0]:>10,.0f}{raw:>11,.0f}{sm:>12,.0f}{cr:>12,.0f}{ratio:>13.2f}x")
    print()

    # leak on the parameter itself: remove the single argmax unit
    top = hop1.max()
    without = hop1[hop1 < top] if (hop1 == top).sum() == 1 else hop1
    print("leak on the parameter (remove the single busiest customer)")
    print(f"{'variant':<22}{'value in':>14}{'value out':>14}{'MIA acc':>10}")
    print("-" * 60)
    print(f"{'raw max':<22}{top:>14,.0f}{without.max():>14,.0f}"
          f"{(1.0 if without.max() != top else 0.5):>9.1%}")
    sm_in = 2.0 * smooth_es([top, hop2.max()], beta)
    sm_out = 2.0 * smooth_es([without.max(), hop2.max()], beta)
    print(f"{'2*SES_beta (shipped)':<22}{sm_in:>14,.0f}{sm_out:>14,.0f}"
          f"{'(proof)':>10}")
    a = crowd_level(hop1, args.f, args.s, eps_meta, 3.0, rng, args.trials)
    b = crowd_level(without, args.f, args.s, eps_meta, 3.0, rng, args.trials)
    print(f"{'crowd + noisy tau':<22}{a.mean():>14,.0f}{b.mean():>14,.0f}"
          f"{_best_threshold_accuracy(a, b):>9.1%}")
    print()


def run_rank(con, args):
    """Dandan's rank-based bound selection (21:23), measured against the histogram rule.

    Her proposal: publish a coarse quantile table of full-domain PU contributions once.
    Per query, rewrite to filterless with a per-PU flag P = "this PU passes the filter",
    sort PUs by contribution descending, and privately release the minimum rank with
    P = 1. That rank gives the percentile of the largest passing PU, and the published
    bound at that percentile becomes the clipping bound. Release the rank with smooth
    sensitivity rather than global.

    Same shape as §9's rung selection -- a public ladder frozen from the full domain, plus
    a small per-query DP statistic saying where on the ladder to clip -- but with a rank
    instead of a histogram, and a quantile ladder instead of exponential bins.

    The concern this measures: R_1 (the MINIMUM passing rank) has local sensitivity
    comparable to its own value, because one added or modified PU that both contributes
    heavily and passes the filter forces R_1 to 1. Smooth sensitivity cannot rescue a
    statistic whose local sensitivity is its own magnitude. R_s -- the rank of the s-th
    highest passing PU -- does not have that problem, because one PU shifts it by one
    position in the passing order.
    """
    ladder = FILTER_LADDER
    if args.entity_filters:
        # Filters on the PU entity itself, not on fact rows. Here "which PUs pass" is the
        # natural notion and a passing PU's WHOLE full-domain contribution is in scope.
        ladder = [(f"c_acctbal>={t:g}", f"o_custkey IN (SELECT c_custkey FROM tpch.customer "
                                        f"WHERE c_acctbal >= {t})")
                  for t in [-1000, 0, 2000, 5000, 8000, 9500]]
    build_contributions(con, args, ladder)
    _, d_s, n_kept = build_metadata(con, args)
    con.execute(
        """
        CREATE OR REPLACE TABLE ranked AS
        SELECT pu, na, row_number() OVER (ORDER BY na DESC) AS rk
        FROM norms WHERE na > 0
        """
    )
    n_pu = con.execute("SELECT count(*) FROM ranked").fetchone()[0]
    print()
    print("rank-based bound selection vs the histogram rule")
    print(f"{n_pu:,} PUs ranked by full-domain contribution, s = {args.s}, D_s = {d_s:,.0f}")
    print()
    print(f"{'filter':<22}{'R_1':>9}{'bound@R_1':>13}{'R_s':>9}{'bound@R_s':>13}"
          f"{'oracle max':>13}{'LS(R_1)':>10}")
    print("-" * 89)
    for i, (label, _) in enumerate(ladder):
        row = con.execute(
            f"""
            WITH passing AS (
                SELECT r.pu, r.na, r.rk
                FROM ranked r
                JOIN (SELECT pu, sum(least(t{i}, B)) AS nt FROM contrib JOIN bg USING (g)
                      WHERE t{i} > 0 AND g IN (SELECT g FROM gstar) GROUP BY pu) f
                  ON f.pu = r.pu AND f.nt > 0
            )
            SELECT (SELECT min(rk) FROM passing),
                   (SELECT na FROM passing ORDER BY rk ASC LIMIT 1),
                   (SELECT rk FROM passing ORDER BY rk ASC LIMIT 1 OFFSET {args.s - 1}),
                   (SELECT na FROM passing ORDER BY rk ASC LIMIT 1 OFFSET {args.s - 1}),
                   (SELECT max(nt) FROM (
                        SELECT sum(least(t{i}, B)) AS nt FROM contrib JOIN bg USING (g)
                        WHERE t{i} > 0 AND g IN (SELECT g FROM gstar) GROUP BY pu))
            """
        ).fetchone()
        r1, b1, rs, bs, omax = row
        if r1 is None:
            print(f"{label:<22}{'(no passing PUs)':>60}")
            continue
        # LS(R_1): one added PU that contributes at the top and passes drives R_1 to 1.
        ls_r1 = float(r1) - 1.0
        print(f"{label:<22}{r1:>9,}{b1:>13,.0f}{(rs or 0):>9,}{(bs or 0):>13,.0f}"
              f"{omax:>13,.0f}{ls_r1:>10,.0f}")
    print()
    print("bound@R_1 must upper-bound every passing PU, so it tracks the single largest")
    print("passer. bound@R_s clips the top s-1 passers instead -- a crowd bound in rank")
    print("space rather than bin space. LS(R_1) is the noise smooth sensitivity must at")
    print("least cover, since one PU can force R_1 to 1.")
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
    p.add_argument("--groupby", default="month", choices=sorted(
        set(GROUPBYS) | {k for d in DATASETS.values() if d["groupbys"] for k in d["groupbys"]}))
    p.add_argument("--measure", default="price", choices=sorted(
        set(MEASURES) | {k for d in DATASETS.values() if d["measures"] for k in d["measures"]}))
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
    p.add_argument("--bucketed", action="store_true", help="add the bucketed-rung columns")
    p.add_argument("--eps-select-frac", type=float, default=0.1, help="fraction of eps spent picking the rung")
    p.add_argument("--clip-tolerance", type=float, default=0.01, help="fraction of PUs the chosen rung may clip")
    p.add_argument("--coalition", action="store_true", help="coalition-size ladder")
    p.add_argument("--attack", action="store_true", help="filter-construction attack")
    p.add_argument("--rung-attack", action="store_true", help="membership inference on the chosen rung")
    p.add_argument("--suite", action="store_true", help="attack suite against the fixed mechanism")
    p.add_argument("--knife", action="store_true", help="the s-threshold knife-edge leak and its fix")
    p.add_argument("--suite2", action="store_true", help="group-universe, rung composition, small-group MIA")
    p.add_argument("--partition", action="store_true", help="attacks on the partition-selection channel")
    p.add_argument("--elastic", action="store_true", help="CROWD ladder vs smoothing for dp_elastic mf")
    p.add_argument("--rank", action="store_true", help="Dandan's rank-based bound selection")
    p.add_argument("--bounds", action="store_true", help="APPROX_BOUNDS vs Dandan vs ours")
    p.add_argument("--pareto", type=float, default=0.0, help="heavy-tail the contributions, Pareto alpha")
    p.add_argument("--dataset", default="tpch", choices=sorted(DATASETS))
    p.add_argument("--sass", action="store_true", help="SASS smooth-median vs Laplace")
    p.add_argument("--auto-bounds", action="store_true", help="worked example of the three approaches")
    p.add_argument("--demo", action="store_true", help="step-by-step walkthrough on a toy dataset")
    p.add_argument("--matrix", action="store_true", help="all bound rules across many cells")
    p.add_argument("--nscale", type=float, nargs="*", default=None, help="population-size sweep")
    p.add_argument("--per-bin-budget", action="store_true",
                   help="Dandan (1): charge eps per bin instead of per histogram")
    p.add_argument("--sass-m", type=float, nargs="+", default=[64, 256, 1024, 4096, 16384])
    p.add_argument("--entity-filters", action="store_true", help="filter on the PU entity, not fact rows")
    p.add_argument("--elastic-delta", type=float, default=1e-6)
    p.add_argument("--partition-eps", type=float, default=0.1)
    p.add_argument("--partition-delta", type=float, default=1e-6)
    p.add_argument("--partition-sizes", type=float, nargs="+",
                   default=[10, 100, 300, 350, 375, 380, 400, 500, 1000])
    p.add_argument("--partition-groups", type=float, nargs="+", default=[1, 2, 5, 10, 20, 50])
    p.add_argument("--partition-eps-ladder", type=float, nargs="+", default=[0.1, 1.0])
    p.add_argument("--partition-delta-ladder", type=float, nargs="+", default=[1e-2, 1e-3])
    p.add_argument("--partition-cu-ladder", type=float, nargs="+", default=[1, 10])
    p.add_argument("--family-size", type=int, default=20, help="filters in the crafted family")
    p.add_argument("--family-ladder", type=float, nargs="+", default=[1, 5, 10, 20])
    p.add_argument("--knife-eps", type=float, nargs="+", default=[0.01, 0.1, 1.0])
    p.add_argument("--knife-margins", type=float, nargs="+", default=[0.0, 3.0])
    p.add_argument("--no-group-bound", action="store_true",
                   help="drop per-group clipping; l1-clip only (metadata = one scalar)")
    p.add_argument("--repeat-ladder", type=float, nargs="+", default=[1, 10, 50, 200])
    p.add_argument("--rung-dos-ladder", type=float, nargs="+", default=[0, 10, 30, 100, 300, 1000])
    p.add_argument("--rung-dos-scale", type=float, default=1000.0)
    p.add_argument("--rung-attack-pops", type=float, nargs="+",
                   default=[100000, 10000, 1000, 100, 10, 2])
    p.add_argument("--coalition-ladder", type=float, nargs="+", default=[0, 1, 10, 100, 349, 350, 700])
    p.add_argument(
        "--attack-thresholds", type=float, nargs="+", default=[-1000, 0, 5000, 9000, 9800, 9990, 9998]
    )
    args = p.parse_args()
    if args.demo:
        run_demo(args); return
    con = open_db(args.db, args.sf)
    if args.dataset in ("so", "cb"):
        con.execute("DETACH tpch")
        con.execute(f"ATTACH '{args.db}' AS {args.dataset} (READ_ONLY)")
    if args.nscale is not None:
        args.nscale = args.nscale or [10000, 30000, 100000, 300000, 1000000]
        run_nscale(con, args)
    elif args.matrix:
        run_matrix(con, args)
    elif args.auto_bounds:
        run_auto_bounds(con, args)
    elif args.sass:
        args.elastic_delta = 1e-6
        run_sass(con, args)
    elif args.bounds:
        args.elastic_delta = 1e-6
        run_bounds(con, args)
    elif args.rank:
        run_rank(con, args)
    elif args.elastic:
        args.elastic_outliers = [("benign", 1), ("one unit x2", 2), ("one unit x10", 10),
                                 ("one unit x100", 100), ("one unit x1000", 1000)]
        run_elastic(con, args)
    elif args.partition:
        run_partition(con, args)
    elif args.suite2:
        run_suite2(con, args)
    elif args.knife:
        run_knife(con, args)
    elif args.suite:
        run_suite(con, args)
    elif args.rung_attack:
        run_rung_attack(con, args)
    elif args.attack:
        run_attack(con, args)
    elif args.coalition:
        run_coalition(con, args)
    elif args.sweep:
        run_sweep(con, args)
    else:
        run_single(con, args)


# ---------------------------------------------------------------------------
# APPROX_BOUNDS vs Dandan's rank rule, head to head as bound selectors.
# Every mechanism picks ONE l1 norm bound; the release is then identical
# (l1-clip to it, sum per group, Laplace(bound/eps_value)), so the only thing
# being compared is the bound choice.
# ---------------------------------------------------------------------------
def run_bounds(con, args):
    rng = np.random.default_rng(args.seed)
    eps_sel = args.eps_select_frac * args.epsilon
    eps_val = args.epsilon - eps_sel
    _, _, _, full_ladder = dsconf(args)
    ladder = full_ladder[:5]
    if args.entity_filters:
        # Predicates on the PU entity: these shrink the POPULATION rather than the rows
        # per person. APPROX_BOUNDS histograms only the survivors, so its bins lose
        # support; Dandan's ladder is built on the full domain and keeps it.
        ladder = [(f"c_acctbal>={t:g}",
                   f"o_custkey IN (SELECT c_custkey FROM tpch.customer WHERE c_acctbal >= {t})")
                  for t in [-1000, 5000, 8000, 9000, 9500]]
    build_contributions(con, args, ladder)
    _, d_s, n_kept = build_metadata(con, args)
    n_pu = con.execute("SELECT count(*) FROM norms WHERE na > 0").fetchone()[0]

    # Dandan's published ladder: coarse upper-tail quantiles of the full-domain totals.
    pcts = [0.5, 0.6, 0.7, 0.8, 0.9, 0.95, 0.99]
    qs = con.execute(
        "SELECT " + ", ".join(f"quantile_cont(na, {p})" for p in pcts) + " FROM norms WHERE na > 0"
    ).fetchone()
    published = sorted(zip(pcts, [float(q) for q in qs]))

    def ladder_lookup(rank):
        """rank from the top -> percentile -> smallest published U that still bounds it."""
        need = 1.0 - float(rank) / n_pu
        for p, u in published:
            if p >= need:
                return u
        return d_s  # above the top published rung

    # candidate bounds: our exponential rungs + her published rungs
    cands = sorted({d_s / float(args.f) ** r for r in range(16)} | {u for _, u in published})
    print()
    print(f"bound selectors, {n_pu:,} PUs, eps={args.epsilon} "
          f"(eps_select={eps_sel:g}), s={args.s}, f={args.f}")
    print("APPROX_BOUNDS threshold = LaplaceQuantile(P^(1/2B)), P=1-1e-9, with the "
          "relaxation loop (approx-bounds.h)")
    print()
    hdr = (f"{'filter':<22}{'oracle':>11}{'approx_b':>11}{'dp-hist':>11}"
           f"{'dandan R1':>11}{'dandan Rs':>11}")
    for i, (label, _) in enumerate(ladder):
        tcol = f"t{i}"
        con.execute(
            f"""
            CREATE OR REPLACE TABLE pt AS
            WITH nt AS (SELECT pu, sum(least({tcol}, B)) AS nt FROM contrib JOIN bg USING (g)
                        WHERE {tcol} > 0 AND g IN (SELECT g FROM gstar) GROUP BY pu)
            SELECT c.pu, c.g, c.{tcol} AS t, least(c.{tcol}, bg.B) AS c, nt.nt
            FROM contrib c JOIN bg USING (g) JOIN nt ON nt.pu = c.pu
            WHERE c.{tcol} > 0 AND c.g IN (SELECT g FROM gstar) AND nt.nt > 0
            """
        )
        # release + error for every candidate bound, precomputed once
        err = {}
        for X in cands:
            rows = con.execute(
                f"SELECT sum(t), sum(c * least(1.0, {X}/nt)) FROM pt GROUP BY g"
            ).fetchall()
            if not rows:
                continue
            a = np.array(rows, dtype=float)
            err[X] = score(a[:, 1], a[:, 0], X / eps_val, args.trials, rng)["total"]
        if not err:
            print(f"{label:<22}  (no groups released)")
            continue

        # --- APPROX_BOUNDS: noised log2 histogram of the filtered totals, highest bin
        #     whose noisy count clears the false-positive threshold.
        h = con.execute(
            f"SELECT cast(floor(log2(nt)) AS INTEGER), count(*) FROM "
            f"(SELECT DISTINCT pu, nt FROM pt) GROUP BY 1"
        ).fetchall()
        hb = np.array([r[0] for r in h]);  hc = np.array([float(r[1]) for r in h])
        # Real google/differential-privacy rule (cc/algorithms/approx-bounds.h):
        #   threshold = LaplaceQuantile(P_success ** (1 / (2 * num_bins)))
        # with P_success = 1 - 1e-9 by default, base 2, and a relaxation loop that
        # multiplies the failure probability by 10 (max 30 tries) while
        # P_success >= 1 - 1e-6.
        n_bins = float(max(hb.max() - hb.min() + 1, 2))
        def ab_threshold(fail):
            p = (1.0 - fail) ** (1.0 / (2.0 * n_bins))
            return -np.log(2.0 * (1.0 - p)) / eps_sel
        ab = []
        for _ in range(200):
            noisy = hc + rng.laplace(0.0, 1.0 / eps_sel, hc.size)
            ok = np.array([], dtype=int)
            fail = 1e-9
            while fail <= 1e-6 and ok.size == 0:      # the relaxation loop
                ok = hb[noisy >= ab_threshold(fail)]
                fail *= 10.0
            ab.append(min(cands, key=lambda X: abs(X - 2.0 ** (ok.max() + 1))) if ok.size else min(cands))
        ab_err = float(np.mean([err[X] for X in ab]))

        # --- ours
        true, releases = rung_releases(con, args, tcol, d_s)
        hist = norm_histogram(con, args, tcol)
        dph = bucketed_dp_hist(true, releases, hist, args, d_s, rng)

        # --- Dandan: noisy rank -> percentile -> published bound
        r1, rs = con.execute(
            f"""
            WITH passing AS (SELECT DISTINCT r.rk FROM
                (SELECT pu, row_number() OVER (ORDER BY na DESC) AS rk FROM norms WHERE na > 0) r
                JOIN (SELECT DISTINCT pu FROM pt) p ON p.pu = r.pu)
            SELECT (SELECT min(rk) FROM passing),
                   (SELECT rk FROM passing ORDER BY rk LIMIT 1 OFFSET {args.s - 1})
            """
        ).fetchone()
        beta = args.epsilon / (2.0 * np.log(2.0 / args.elastic_delta))
        ss_r1 = max(float(r1) - 1.0, 1.0) * np.exp(-beta)      # smooth sens of R_1 ~ R_1
        d1 = [err[min(cands, key=lambda X: abs(X - ladder_lookup(
                 np.clip(r1 + rng.laplace(0.0, ss_r1 / eps_sel), 1, n_pu))))] for _ in range(200)]
        out_rs = "—"
        if rs:
            gap = max(float(rs) / args.s, 1.0)                  # optimistic: local, not smoothed
            ds_ = [err[min(cands, key=lambda X: abs(X - ladder_lookup(
                     np.clip(rs + rng.laplace(0.0, gap / eps_sel), 1, n_pu))))] for _ in range(200)]
            out_rs = f"{np.mean(ds_):.1%}"
        if i == 0:
            print(hdr); print("-" * 77)
        print(f"{label:<22}{min(err.values()):>10.1%}{ab_err:>11.1%}"
              f"{dph['total']:>11.1%}{np.mean(d1):>11.1%}{out_rs:>11}")
    print()
    print("dandan Rs is OPTIMISTIC: scaled by the local gap, not a smoothed sensitivity.")
    print()


def smooth_sens_median(x, lam, beta):
    """Exact smooth sensitivity of the median (Nissim-Raskhodnikova-Smith; the O(n log n)
    form Dandan sent on 9 June, evaluated directly since m is small).
        S*_beta = max_{k>=0} e^{-beta k} * max_{t=0..k+1} (x_{p+t} - x_{p+t-k-1})
    with x sorted, x_0 = 0 and x_{n+1} = lam (the public output domain)."""
    n = len(x)
    xs = np.concatenate(([0.0], np.sort(x), [lam]))
    p = int(np.ceil(n / 2.0))
    best = 0.0
    for k in range(0, n + 1):
        decay = np.exp(-beta * k)
        if decay * lam < best:
            break
        for t in range(0, k + 2):
            hi, lo = p + t, p + t - k - 1
            if 0 <= lo and hi <= n + 1:
                best = max(best, (xs[hi] - xs[lo]) * decay)
    return best


def private_range_intervals(est, hi, eps, q, rng):
    """GUPT-style DP quantile: exponential mechanism over the intervals between sorted
    lane estimates, weighted by interval width, on the public domain [0, hi]."""
    xs = np.concatenate(([0.0], np.sort(est), [hi]))
    widths = np.diff(xs)
    target = q * len(est)
    ranks = np.arange(len(widths))
    logits = eps * (-np.abs(ranks - target)) / 2.0
    logits -= logits.max()
    w = widths * np.exp(logits)
    if w.sum() <= 0:
        return hi
    i = rng.choice(len(w), p=w / w.sum())
    return float(rng.uniform(xs[i], xs[i + 1]))


def private_range_ladder(est, hi, f, eps, q, rng):
    """Same goal, but the exponential mechanism runs over a PUBLIC ladder of powers of f
    instead of over data intervals, so it cannot be dragged into the huge empty interval
    above the data. Utility = -|#{lanes above rung} - (1-q)*m|, sensitivity 1."""
    rungs = np.array([f ** k for k in range(1, int(np.ceil(np.log(hi) / np.log(f))) + 1)])
    want = (1.0 - q) * len(est)
    above = np.array([float((est > r).sum()) for r in rungs])
    logits = eps * (-np.abs(above - want)) / 2.0
    logits -= logits.max()
    w = np.exp(logits)
    return float(rungs[rng.choice(len(rungs), p=w / w.sum())])


def run_sass(con, args):
    """SASS smooth-median release vs Google-DP-style Laplace, with the domain bound Lambda
    derived rather than supplied. Sweeps m to find where Lambda stops dominating."""
    rng = np.random.default_rng(args.seed)
    _, _, _, full_ladder = dsconf(args)
    ladder = [full_ladder[1]]
    build_contributions(con, args, ladder)
    _, d_s, n_kept = build_metadata(con, args)
    n_pu = con.execute("SELECT count(*) FROM norms WHERE na > 0").fetchone()[0]
    beta = args.epsilon / (2.0 * np.log(2.0 / args.elastic_delta))
    lam = d_s * n_pu                     # lane estimates are scaled to the full population
    con.execute(
        """
        CREATE OR REPLACE TABLE pt AS
        WITH nt AS (SELECT pu, sum(least(t0, B)) AS nt FROM contrib JOIN bg USING (g)
                    WHERE t0 > 0 AND g IN (SELECT g FROM gstar) GROUP BY pu)
        SELECT c.pu, c.g, c.t0 AS t,
               least(c.t0, bg.B) * least(1.0, CAST(? AS DOUBLE) / nt.nt) AS c
        FROM contrib c JOIN bg USING (g) JOIN nt ON nt.pu = c.pu
        WHERE c.t0 > 0 AND c.g IN (SELECT g FROM gstar) AND nt.nt > 0
        """, [d_s]
    )
    truth = {g: float(v) for g, v in con.execute("SELECT g, sum(t) FROM pt GROUP BY g").fetchall()}
    print()
    print(f"SASS smooth-median vs Laplace.  filter = {ladder[0][0]}, group-by = {args.groupby}")
    print(f"eps={args.epsilon}, delta={args.elastic_delta:g}, beta={beta:.4f}")
    print(f"D_s={d_s:,.0f}, N_PU={n_pu:,}, Lambda = D_s*N_PU = {lam:.3g}")
    print(f"typical true group value = {np.median(list(truth.values())):.3g}")
    print()
    print(f"{'release':<34}{'noise scale':>14}{'median rel err':>16}")
    print("-" * 64)
    lap = d_s / args.epsilon
    errs = [abs(rng.laplace(0.0, lap)) / v for g, v in truth.items() for _ in range(20)]
    print(f"{'Laplace, l1 crowd bound':<34}{lap:>14,.0f}{np.median(errs):>15.1%}")
    for m in args.sass_m:
        m = int(m)
        rows = con.execute(
            f"SELECT g, abs(hash(pu)) % {m} AS lane, sum(c) FROM pt GROUP BY 1, 2"
        ).fetchall()
        by_g = {}
        for g, lane, v in rows:
            by_g.setdefault(g, np.zeros(m))[int(lane)] = float(v)
        # Two settings of the public output domain: the derived one (per-PU bound scaled to
        # the population, the relation currently used for dp_sass_*_output_bound), and an
        # ORACLE one taken from the actual lane spread -- not releasable, but it separates
        # "SASS is weak here" from "our Lambda is far too loose".
        oracle_lam = max(float((lanes * m).max()) for lanes in by_g.values())
        all_est = np.concatenate([lanes * m for lanes in by_g.values()])
        eps_range = 0.1 * args.epsilon
        piv = float(np.median([private_range_intervals(all_est, lam, eps_range, 0.99, rng)
                               for _ in range(9)]))
        plad = float(np.median([private_range_ladder(all_est, lam, args.f, eps_range, 0.99, rng)
                                for _ in range(9)]))
        for tag, L in (("Lambda=D_s*N_PU", lam), ("Lambda=private/intervals", piv),
                       ("Lambda=private/ladder", plad), ("Lambda=oracle", oracle_lam)):
            rel, scales = [], []
            for g, lanes in by_g.items():
                est = np.clip(lanes * m, 0.0, L)
                ss = smooth_sens_median(est, L, beta)
                scale = 2.0 * ss / args.epsilon
                med = float(np.median(est))
                scales.append(scale)
                rel += [abs(med + rng.laplace(0.0, scale) - truth[g]) / truth[g] for _ in range(20)]
            print(f"{'SASS median m=' + str(m) + ', ' + tag:<34}{np.median(scales):>14,.0f}"
                  f"{np.median(rel):>15.1%}")
    print()
    print("Lambda enters the smooth sensitivity as the endpoint x_{m+1}, so the endpoint term")
    print("is ~exp(-beta*m/2)*Lambda. Until that falls below the spread of the lane estimates,")
    print("the noise is set by Lambda and the bound-derivation question is moot.")
    print()


def _lap_quantile_threshold(n_bins, eps, p_success=1.0 - 1e-9):
    """approx-bounds.h: threshold = LaplaceQuantile(P^(1/(2*num_bins)))."""
    p = p_success ** (1.0 / (2.0 * n_bins))
    return -np.log(2.0 * (1.0 - p)) / eps


def run_auto_bounds(con, args):
    """Worked numeric example of the three automatic-bound approaches in Dandan's
    'DP automatic bounds' draft, on real TPC-H data.

    Budget follows the Google DP default: half of eps to bound selection, half to the
    aggregate. Count sensitivity is 2 throughout, per her note -- changing one PU can move
    it between two bins.
    """
    rng = np.random.default_rng(args.seed)
    eps_b = args.epsilon / 2.0
    eps_agg = args.epsilon - eps_b
    _, _, _, full_ladder = dsconf(args)
    ladder = [full_ladder[1], full_ladder[3]]
    build_contributions(con, args, ladder)
    _, d_s, n_kept = build_metadata(con, args)

    for fi, (label, _) in enumerate(ladder):
        tcol = f"t{fi}"
        con.execute(
            f"""
            CREATE OR REPLACE TABLE pt AS
            WITH nt AS (SELECT pu, sum({tcol}) AS t FROM contrib
                        WHERE g IN (SELECT g FROM gstar) GROUP BY pu)
            SELECT n.pu, n.t, m.a
            FROM nt n JOIN (SELECT pu, sum(a) AS a FROM contrib
                            WHERE g IN (SELECT g FROM gstar) GROUP BY pu) m ON m.pu = n.pu
            """
        )
        a = np.array([float(r[0]) for r in con.execute("SELECT a FROM pt WHERE a > 0").fetchall()])
        t = np.array([float(r[0]) for r in con.execute("SELECT t FROM pt WHERE t > 0").fetchall()])
        u_nf, u_true = a.max(), t.max()
        print()
        print("=" * 78)
        print(f"filter: {label}")
        print(f"  PUs with any contribution (filterless)   {a.size:,}")
        print(f"  PUs passing the filter                   {t.size:,}")
        print(f"  U_nf  = max filterless PU contribution    {u_nf:,.0f}")
        print(f"  U     = max FILTERED PU contribution      {u_true:,.0f}   <- the ideal bound")
        print(f"  eps = {args.epsilon} split {eps_b} bounds / {eps_agg} aggregate, count sensitivity 2")
        print("=" * 78)

        results = {}

        # ---------- Approach 1: ApproxBounds as shipped -------------------------
        bins = np.floor(np.log2(t)).astype(int)
        ub, cb = np.unique(bins, return_counts=True)
        noisy = cb + rng.laplace(0.0, 2.0 / eps_b, cb.size)
        thr64 = _lap_quantile_threshold(64, eps_b)     # Google: (0, 2^64] -> 64 bins
        ok = ub[noisy >= thr64]
        b1 = 2.0 ** (ok.max() + 1) if ok.size else 0.0
        results["1  ApproxBounds (threshold, 64 bins)"] = b1
        print()
        print(f"[1] ApproxBounds as shipped: 64 log2 bins over (0, 2^64], threshold = {thr64:,.1f}")
        print(f"    {'bin':>5}{'range':>26}{'PUs':>10}{'noisy':>12}{'clears?':>9}")
        for bb, cc, nn in list(zip(ub, cb, noisy))[-7:]:
            print(f"    {bb:>5}{f'[2^{bb}, 2^{bb+1})':>26}{cc:>10,}{nn:>12,.0f}"
                  f"{('yes' if nn >= thr64 else 'no'):>9}")
        print(f"    -> selected bin {ok.max() if ok.size else None}, bound = {b1:,.0f}")

        # ---------- Approach 1b: tail-mass variant ------------------------------
        tot = noisy.sum()
        print()
        print("[1b] tail-fraction variant: smallest k with sum_{i>=k} c_i / sum c_i >= alpha")
        for alpha in (0.10, 0.20, 0.30):
            tail = np.cumsum(noisy[::-1])[::-1] / tot
            idx = np.where(tail >= alpha)[0]
            k = ub[idx[-1]] if idx.size else ub[0]
            b = 2.0 ** (k + 1)
            results[f"1b tail-fraction alpha={alpha:.0%}"] = b
            print(f"     alpha={alpha:.0%} -> bin {k}, bound = {b:,.0f}")

        # ---------- Approach 2: frozen filterless maximum -----------------------
        draws = np.clip(u_true + rng.laplace(0.0, u_nf / eps_b, 2001), 0.0, None)
        b2 = float(np.median(draws))
        results["2  frozen filterless max U + Lap(U_nf/eps)"] = b2
        print()
        print(f"[2] U* = U + Lap(U_nf/eps_b) = {u_true:,.0f} + Lap({u_nf / eps_b:,.0f})")
        print(f"    median draw = {b2:,.0f}   (noise scale is {u_nf / eps_b / max(u_true,1):.1f}x the "
              f"quantity being released)")

        n_small = int(np.ceil(np.log2(u_nf))) + 1
        thr_s = _lap_quantile_threshold(n_small, eps_b)
        ok2 = ub[noisy >= thr_s]
        b2b = 2.0 ** (ok2.max() + 1) if ok2.size else 0.0
        results[f"2b ApproxBounds restricted to {n_small} bins"] = b2b
        print(f"[2b] using U_nf to cut 64 bins -> m = ceil(log2 U_nf)+1 = {n_small}: "
              f"threshold {thr64:,.1f} -> {thr_s:,.1f}, bound = {b2b:,.0f}")

        # ---------- Approach 3: frozen filterless metadata ----------------------
        deciles = [float(np.quantile(a, q)) for q in [0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1, 0.0]]
        edges = np.array(deciles)                       # U_1 .. U_10, descending
        # bin k (1-based) = (U_{k+1}, U_k];  #edges strictly greater than v gives k
        idx = np.clip(np.searchsorted(-edges, -t, side="left") - 1, 0, 9)
        c3 = np.array([(idx == i).sum() for i in range(10)], dtype=float)
        u3 = c3 + rng.laplace(0.0, 2.0 / eps_b, 10)
        print()
        print("[3] frozen filterless metadata: 10 bins at the deciles of the FILTERLESS")
        print("    distribution, filtered PUs assigned to them")
        print(f"    {'bin':>5}{'U_k':>14}{'PUs':>10}{'noisy':>10}{'cum share':>12}")
        cum = np.cumsum(u3) / u3.sum()
        for i in range(10):
            print(f"    {i+1:>5}{edges[i]:>14,.0f}{int(c3[i]):>10,}{u3[i]:>10,.0f}{cum[i]:>11.1%}")
        for alpha in (0.10, 0.20, 0.30):
            k = int(np.argmax(cum >= alpha))
            b = float(edges[k])
            results[f"3  frozen metadata alpha={alpha:.0%}"] = b
            print(f"     alpha={alpha:.0%} -> leftmost k = {k+1}, bound U_{k+1} = {b:,.0f}")

        # ---------- what each bound costs ---------------------------------------
        print()
        print(f"    {'approach':<44}{'bound':>14}{'vs ideal':>10}{'rel err':>10}")
        print("    " + "-" * 78)
        for name, B in sorted(results.items(), key=lambda kv: kv[0]):
            if B <= 0:
                print(f"    {name:<44}{'failed':>14}"); continue
            rows = con.execute(
                f"""
                SELECT sum(c.{tcol}), sum(least(c.{tcol}, {B} * c.{tcol} / n.t))
                FROM contrib c JOIN (SELECT pu, sum({tcol}) AS t FROM contrib
                                     WHERE g IN (SELECT g FROM gstar) GROUP BY pu) n ON n.pu = c.pu
                WHERE c.{tcol} > 0 AND c.g IN (SELECT g FROM gstar) AND n.t > 0
                GROUP BY c.g
                """
            ).fetchall()
            arr = np.array(rows, dtype=float)
            e = score(arr[:, 1], arr[:, 0], B / eps_agg, 200, rng)
            print(f"    {name:<44}{B:>14,.0f}{B / u_true:>9.2f}x{e['total']:>9.1%}")
    print()


def candidate_bounds(a, t, eps_b, rng, n_groups=1, eps_agg=0.5, med_group_share=None,
                     per_bin_budget=False):
    """Every automatic-bound rule, given filterless totals `a` and filtered totals `t`.
    Includes two variants that are NOT in the draft, marked (fix), testing the two
    diagnoses from the toy walkthrough: alpha on mass rather than counts, and rungs
    spaced by tail probability rather than by decile."""
    out = {}
    t = t[t > 0]
    if t.size == 0:
        return out
    bins = np.floor(np.log2(t)).astype(int)
    ub, cb = np.unique(bins, return_counts=True)
    # Dandan (1): charge per BIN rather than per histogram. A k-bin rule then gets
    # eps_b * k/64 and the rest goes to the query. (I believe the histogram actually
    # costs eps_b regardless of k by parallel composition -- this switch tests what her
    # accounting would imply if it held.)
    eps_ab = eps_b if not per_bin_budget else eps_b * min(ub.size, 64) / 64.0
    noisy = np.maximum(cb + rng.laplace(0.0, 2.0 / eps_ab, cb.size), 0.0)
    thr = _lap_quantile_threshold(64, eps_ab)
    ok = ub[noisy >= thr]
    out["1 approx_bounds"] = 2.0 ** (ok.max() + 1) if ok.size else 0.0

    tot = max(noisy.sum(), 1e-9)
    tail_c = np.cumsum(noisy[::-1])[::-1] / tot
    mass = noisy * (2.0 ** (ub + 0.5))
    tail_m = np.cumsum(mass[::-1])[::-1] / max(mass.sum(), 1e-9)
    for alpha in (0.10, 0.30):
        i = np.where(tail_c >= alpha)[0]
        out[f"1b tail-count {alpha:.0%}"] = 2.0 ** (ub[i[-1]] + 1) if i.size else 0.0
        j = np.where(tail_m >= alpha)[0]
        out[f"1b tail-mass {alpha:.0%} (fix)"] = 2.0 ** (ub[j[-1]] + 1) if j.size else 0.0

    out["2 frozen max"] = float(np.median(np.clip(
        t.max() + rng.laplace(0.0, a.max() / eps_b, 501), 0.0, None)))
    nb = max(int(np.ceil(np.log2(max(a.max(), 2.0)))) + 1, 2)
    ok2 = ub[noisy >= _lap_quantile_threshold(nb, eps_b)]
    out["2b approx, fewer bins"] = 2.0 ** (ok2.max() + 1) if ok2.size else 0.0

    # --- Approach 3 + Dandan's two-level refinement -------------------------------
    # Level 1: pick a coarse quantile bin with half the bound budget. Level 2: subdivide
    # THAT interval into log2 sub-bins and rerun the same alpha rule with the other half.
    # Two releases over the same data, so eps_b splits sequentially.
    qs0 = [0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1, 0.0]
    e2 = eps_b / 2.0
    edges0 = np.array([float(np.quantile(a, q)) for q in qs0])
    i0 = np.clip(np.searchsorted(-edges0, -t, side="left") - 1, 0, 9)
    c0 = np.array([(i0 == i).sum() for i in range(10)], dtype=float)
    n0 = np.maximum(c0 + rng.laplace(0.0, 2.0 / e2, 10), 0.0)
    cum0 = np.cumsum(n0) / max(n0.sum(), 1e-9)
    for alpha in (0.10, 0.30):
        k = int(np.argmax(cum0 >= alpha))
        hi = float(edges0[k])
        lo = float(edges0[k + 1]) if k + 1 < 10 else 0.0
        sub = t[(t > lo) & (t <= hi)]
        if sub.size < 2 or hi <= 0:
            out[f"3+refine {alpha:.0%}"] = hi
            continue
        lo_b = int(np.floor(np.log2(max(lo, 1.0))))
        hi_b = int(np.ceil(np.log2(hi)))
        sb = np.arange(lo_b, hi_b + 1)
        cs = np.array([float(((sub >= 2.0 ** b) & (sub < 2.0 ** (b + 1))).sum()) for b in sb])
        ns = np.maximum(cs + rng.laplace(0.0, 2.0 / e2, cs.size), 0.0)
        # The level-2 rule must target the RESIDUAL fraction, not alpha again: level 1 has
        # already accounted for everything above this bin. Applying alpha at both levels
        # compounds and over-clips.
        tot0 = max(n0.sum(), 1e-9)
        above = float(n0[:k].sum())
        want = max(alpha * tot0 - above, 0.0)
        cs_sub = np.cumsum(ns[::-1])[::-1]
        j = np.where(cs_sub >= want)[0]
        out[f"3+refine {alpha:.0%}"] = float(min(2.0 ** (sb[j[-1]] + 1), hi)) if j.size else hi

    # --- objective rule: pick the bound minimising estimated clipped mass + noise,
    # from the SAME noisy histogram. No fixed alpha; the target adapts to eps and to how
    # many groups the noise has to cover.
    # Score the objective on the SAME quantity we measure: median relative error.
    # Clip loss is a roughly constant FRACTION of every group, but the noise is the same
    # ABSOLUTE amount in each, so the median group -- not the mean -- sets the tradeoff.
    # med_group_share comes from the per-group PU counts, which partition selection
    # already releases, so it costs nothing extra.
    mids_o = 2.0 ** (ub + 0.5)
    cands_o = 2.0 ** (ub + 1)
    total_mass = max(float(np.sum(noisy * mids_o)), 1e-9)
    share = med_group_share if med_group_share else 1.0 / max(n_groups, 1)
    med_group_total = max(total_mass * share, 1e-9)
    est = [float(np.sum(noisy * np.maximum(mids_o - B, 0.0))) / total_mass
           + B / (eps_agg * med_group_total) for B in cands_o]
    out["0 objective (clip+noise)"] = float(cands_o[int(np.argmin(est))])

    for tag, qs in (("3 deciles", [0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1, 0.0]),
                    ("3 tail-prob (fix)",
                     [1.0, 0.9999, 0.999, 0.99, 0.95, 0.9, 0.75, 0.5, 0.25, 0.0])):
        edges = np.array([float(np.quantile(a, q)) for q in qs])
        idx = np.clip(np.searchsorted(-edges, -t, side="left") - 1, 0, len(edges) - 1)
        c3 = np.array([(idx == i).sum() for i in range(len(edges))], dtype=float)
        u3 = np.maximum(c3 + rng.laplace(0.0, 2.0 / eps_b, len(edges)), 0.0)
        cum = np.cumsum(u3) / max(u3.sum(), 1e-9)
        for alpha in (0.10, 0.30):
            out[f"{tag} {alpha:.0%}"] = float(edges[int(np.argmax(cum >= alpha))])
    return out


def run_matrix(con, args):
    """All bound rules across many (dataset, measure, grouping, filter) cells."""
    rng = np.random.default_rng(args.seed)
    eps_b, eps_agg = args.epsilon / 2.0, args.epsilon / 2.0
    cells = {"so": CELLS_SO, "cb": CELLS_CB, "supp": CELLS_SUPP}.get(args.dataset, CELLS_TPCH)
    names, rows = None, []
    for measure, groupby, pareto, entity in cells:
        args.measure, args.groupby, args.pareto, args.entity_filters = measure, groupby, pareto, entity
        _, _, _, fl = dsconf(args)
        ladder = ([(f"acctbal>={x:g}", f"o_custkey IN (SELECT c_custkey FROM tpch.customer "
                                      f"WHERE c_acctbal >= {x})") for x in [0, 8000, 9500]]
                  if entity else fl[:4])
        build_contributions(con, args, ladder)
        _, d_s, _ = build_metadata(con, args)
        for fi, (flab, _) in enumerate(ladder):
            tcol = f"t{fi}"
            con.execute(
                f"""CREATE OR REPLACE TABLE pt AS
                    SELECT pu, sum(a) AS a, sum({tcol}) AS t FROM contrib
                    WHERE g IN (SELECT g FROM gstar) GROUP BY pu""")
            a = np.array([float(r[0]) for r in con.execute("SELECT a FROM pt WHERE a>0").fetchall()])
            tt = np.array([float(r[0]) for r in con.execute("SELECT t FROM pt WHERE t>0").fetchall()])
            if tt.size < 50:
                continue
            ng = con.execute("SELECT count(*) FROM gstar").fetchone()[0]
            cands = candidate_bounds(a, tt, eps_b, rng, n_groups=ng, eps_agg=eps_agg)
            grid = sorted(set(list(cands.values()) + [tt.max() / 2.0 ** k for k in range(14)]))
            errs = {}
            for B in grid:
                if B <= 0:
                    continue
                r = con.execute(
                    f"""SELECT sum(c.{tcol}), sum(least(c.{tcol}, {B} * c.{tcol} / p.t))
                        FROM contrib c JOIN pt p ON p.pu = c.pu
                        WHERE c.{tcol} > 0 AND c.g IN (SELECT g FROM gstar) AND p.t > 0
                        GROUP BY c.g""").fetchall()
                if r:
                    arr = np.array(r, dtype=float)
                    errs[B] = score(arr[:, 1], arr[:, 0], B / eps_agg, 80, rng)["total"]
            if not errs:
                continue
            row = {"cell": f"{measure}/{groupby}{'/pareto' if pareto else ''}"
                           f"{'/entity' if entity else ''} | {flab}"[:40],
                   "oracle": min(errs.values())}
            for n, B in cands.items():
                row[n] = errs.get(B, float("nan")) if B > 0 else float("nan")
            names = names or ["oracle"] + list(cands.keys())
            rows.append(row)
    print()
    short = {n: (n[:12]) for n in names}
    hdr = f"{'cell':<42}" + "".join(f"{short[n]:>13}" for n in names)
    print(hdr); print("-" * len(hdr))
    for r in rows:
        print(f"{r['cell']:<42}" + "".join(
            (f"{r.get(n, float('nan')):>12.1%}" if r.get(n) == r.get(n) else f"{'-':>12}") + " "
            for n in names))
    print()
    print(f"{'approach':<26}{'wins':>7}{'mean rank':>11}{'median err':>12}{'vs oracle':>11}")
    print("-" * 67)
    stats = []
    for n in names[1:]:
        v = [r.get(n) for r in rows if r.get(n) == r.get(n)]
        wins = sum(1 for r in rows if r.get(n) == r.get(n) and
                   r[n] <= min(r.get(m, 9e9) for m in names[1:] if r.get(m) == r.get(m)) + 1e-12)
        ranks = []
        for r in rows:
            vals = sorted((r[m] for m in names[1:] if r.get(m) == r.get(m)))
            if r.get(n) == r.get(n):
                ranks.append(vals.index(r[n]) + 1)
        ratio = np.median([r[n] / max(r["oracle"], 1e-9) for r in rows if r.get(n) == r.get(n)])
        stats.append((np.mean(ranks) if ranks else 99, n, wins, np.median(v) if v else 9e9, ratio))
    for mr, n, wins, med, ratio in sorted(stats):
        print(f"{n:<26}{wins:>7}{mr:>11.1f}{med:>11.1%}{ratio:>10.1f}x")
    print()


# Nominal bin count each rule needs, for Dandan's per-bin accounting.
RULE_BINS = {"1 approx_bounds": 64, "1b tail-count 10%": 64, "1b tail-count 30%": 64,
             "1b tail-mass 10% (fix)": 64, "1b tail-mass 30% (fix)": 64,
             "0 objective (clip+noise)": 64, "2 frozen max": 1,
             "3 deciles 10%": 10, "3 deciles 30%": 10,
             "3 tail-prob (fix) 10%": 10, "3 tail-prob (fix) 30%": 10,
             "3+refine 10%": 20, "3+refine 30%": 20}


def run_nscale(con, args):
    """Isolate population size: same dataset, same measure/grouping/filter, only the
    number of PUs varies (hash-subsample of the PU set, so the contribution distribution
    keeps its shape). Reported as ratio-to-oracle, which controls for groups getting
    smaller as n shrinks."""
    rng = np.random.default_rng(args.seed)
    eps_b, eps_agg = args.epsilon / 2.0, args.epsilon / 2.0
    _, _, _, fl = dsconf(args)
    lad = [fl[1]]
    build_contributions(con, args, lad)
    build_metadata(con, args)
    ng = con.execute("SELECT count(*) FROM gstar").fetchone()[0]
    n_all = con.execute("SELECT count(DISTINCT pu) FROM contrib").fetchone()[0]
    M = 1000003
    print()
    print(f"population scaling: {args.dataset} {args.measure}/{args.groupby}, "
          f"filter = {lad[0][0]}, {ng} groups, eps={args.epsilon}")
    print(f"full population = {n_all:,} PUs; subsampled by hash so the shape is preserved")
    keys = ["1 approx_bounds", "1b tail-count 10%", "1b tail-mass 30% (fix)",
            "3 deciles 10%", "0 objective (clip+noise)"]
    print()
    print(f"{'n (PUs)':>10}{'thr as % of n':>15}{'mass clipped':>14}" +
          "".join(f"{k.split(' ',1)[1][:11]:>13}" for k in keys))
    print("-" * (39 + 13 * len(keys)))
    for n in args.nscale:
        n = int(n)
        frac = min(n / n_all, 1.0)
        cut = int(frac * M)
        con.execute(
            f"""CREATE OR REPLACE TABLE pt AS
                SELECT pu, sum(a) AS a, sum(t0) AS t FROM contrib
                WHERE g IN (SELECT g FROM gstar) AND abs(hash(pu)) % {M} < {cut}
                GROUP BY pu""")
        A_ = np.array([float(r[0]) for r in con.execute("SELECT a FROM pt WHERE a>0").fetchall()])
        T_ = np.array([float(r[0]) for r in con.execute("SELECT t FROM pt WHERE t>0").fetchall()])
        if T_.size < 200:
            continue
        gsz = np.array([float(r[0]) for r in con.execute(
            "SELECT count(*) FROM contrib c JOIN pt p ON p.pu=c.pu "
            "WHERE c.t0>0 AND c.g IN (SELECT g FROM gstar) GROUP BY c.g").fetchall()])
        mgs = float(np.median(gsz) / gsz.sum()) if gsz.size else None
        eb = eps_b if not args.per_bin_budget else eps_b
        cb = candidate_bounds(A_, T_, eb, rng, n_groups=ng, eps_agg=eps_agg,
                              med_group_share=mgs, per_bin_budget=args.per_bin_budget)
        grid = sorted(set(list(cb.values()) + [T_.max() / 2.0 ** k for k in range(16)]))
        errs = {}
        for B in grid:
            if B <= 0:
                continue
            r = con.execute(
                f"""SELECT sum(c.t0), sum(least(c.t0, {B} * c.t0 / p.t))
                    FROM contrib c JOIN pt p ON p.pu = c.pu
                    WHERE c.t0 > 0 AND c.g IN (SELECT g FROM gstar) AND p.t > 0
                    GROUP BY c.g""").fetchall()
            if r:
                arr = np.array(r, dtype=float)
                errs[B] = score(arr[:, 1], arr[:, 0], B / eps_agg, 120, rng)["total"]
        # Under Dandan (1) each rule pays bins * (eps/64) for bounds and keeps the rest for
        # the query, so the aggregate budget -- and therefore the noise -- differs per rule.
        errs_pb = {}
        if args.per_bin_budget:
            for k, B in cb.items():
                if B <= 0:
                    continue
                e_b = args.epsilon * RULE_BINS.get(k, 64) / 64.0
                e_a = max(2.0 * args.epsilon - e_b, 1e-3)
                r = con.execute(
                    f"""SELECT sum(c.t0), sum(least(c.t0, {B} * c.t0 / p.t))
                        FROM contrib c JOIN pt p ON p.pu = c.pu
                        WHERE c.t0 > 0 AND c.g IN (SELECT g FROM gstar) AND p.t > 0
                        GROUP BY c.g""").fetchall()
                arr = np.array(r, dtype=float)
                errs_pb[k] = score(arr[:, 1], arr[:, 0], B / e_a, 120, rng)["total"]
        oracle = min(errs.values())
        Bab = cb["1 approx_bounds"]
        above = T_ > Bab
        mclip = (T_[above] - Bab).sum() / T_.sum() if above.any() else 0.0
        thr = _lap_quantile_threshold(64, eps_b)
        line = f"{T_.size:>10,}{thr / T_.size:>14.4%}{mclip:>13.2%}"
        for k in keys:
            B = cb.get(k, 0.0)
            e = errs_pb.get(k) if args.per_bin_budget else (errs.get(B) if B > 0 else None)
            line += f"{(e / oracle if e else float('nan')):>12.1f}x"
        print(line)
    print()
    print("values are error / oracle error at that n (1.0x = best achievable bound)")
    print()


def run_demo(args):
    """Small worked example: build a toy sales table, then walk all three automatic-bound
    approaches through one query, printing every intermediate number."""
    rng = np.random.default_rng(args.seed)
    con = duckdb.connect()
    eps_b, eps_agg = 1.0, 1.0

    # ---- 1. the dataset ----------------------------------------------------
    # 1000 customers in three tiers. Whales spend a lot but almost entirely in-store,
    # so a 'web' filter leaves a much smaller maximum than the unfiltered one -- the
    # situation that separates the three approaches.
    rows = []
    for c in range(1, 1001):
        if c <= 900:      tier, n, lo, hi, web = "regular", 6, 50, 150, 0.5
        elif c <= 990:    tier, n, lo, hi, web = "large",   8, 200, 350, 0.5
        else:             tier, n, lo, hi, web = "whale",  20, 2000, 3000, 0.05
        for _ in range(n):
            rows.append((c, tier, int(rng.integers(1, 13)), float(rng.integers(lo, hi)),
                         "web" if rng.random() < web else "store"))
    con.execute("CREATE TABLE sales(custkey INT, tier TEXT, month INT, amount DOUBLE, channel TEXT)")
    con.executemany("INSERT INTO sales VALUES (?,?,?,?,?)", rows)

    print("\n" + "=" * 74)
    print("DATASET   sales(custkey, tier, month, amount, channel), PU = customer")
    print("=" * 74)
    print(f"  {'tier':<10}{'customers':>11}{'rows':>8}{'total spend':>14}{'web spend':>13}")
    for r in con.execute("""SELECT tier, count(DISTINCT custkey), count(*), sum(amount),
                                   sum(amount) FILTER (channel='web')
                            FROM sales GROUP BY tier ORDER BY sum(amount)""").fetchall():
        print(f"  {r[0]:<10}{r[1]:>11,}{r[2]:>8,}{r[3]:>14,.0f}{r[4]:>13,.0f}")
    print("\nQUERY:  SELECT month, SUM(amount) FROM sales WHERE channel='web' GROUP BY month")
    print(f"        eps = 2.0  ->  {eps_b} for choosing the bound, {eps_agg} for the noisy sum")
    print("        count sensitivity 2 (one PU can move between two bins)")

    # ---- 2. per-customer contributions ------------------------------------
    con.execute("""CREATE TABLE contrib AS
        SELECT custkey, any_value(tier) AS tier, sum(amount) AS a,
               coalesce(sum(amount) FILTER (channel='web'), 0) AS t
        FROM sales GROUP BY custkey""")
    a = np.array([r[0] for r in con.execute("SELECT a FROM contrib").fetchall()])
    t = np.array([r[0] for r in con.execute("SELECT t FROM contrib WHERE t > 0").fetchall()])
    u_nf, u_id = a.max(), t.max()
    print("\n" + "-" * 74)
    print("STEP 0  per-customer contributions (this is what all three methods bound)")
    print("-" * 74)
    print(f"  {'custkey':>9}{'tier':>10}{'filterless total':>19}{'web total':>12}")
    for r in con.execute("SELECT custkey, tier, a, t FROM contrib ORDER BY a DESC LIMIT 4").fetchall():
        print(f"  {r[0]:>9}{r[1]:>10}{r[2]:>19,.0f}{r[3]:>12,.0f}")
    for r in con.execute("SELECT custkey, tier, a, t FROM contrib ORDER BY a LIMIT 2").fetchall():
        print(f"  {r[0]:>9}{r[1]:>10}{r[2]:>19,.0f}{r[3]:>12,.0f}")
    print(f"  U_nf = max filterless contribution = {u_nf:,.0f}   (a whale)")
    print(f"  U    = max web contribution        = {u_id:,.0f}   <- the bound we WANT")
    print(f"  the filter cuts the maximum by {u_nf/u_id:.0f}x, because whales barely use web")

    out = {}
    # ---- 3. Approach 1 -----------------------------------------------------
    bins = np.floor(np.log2(t)).astype(int)
    ub, cb = np.unique(bins, return_counts=True)
    noisy = cb + rng.laplace(0.0, 2.0 / eps_b, cb.size)
    thr = _lap_quantile_threshold(64, eps_b)
    print("\n" + "-" * 74)
    print(f"APPROACH 1  ApproxBounds: log2 histogram of the WEB totals, 64 bins")
    print(f"            threshold = LaplaceQuantile(P^(1/128)) = {thr:.1f}")
    print("-" * 74)
    print(f"  {'bin':>4}{'range':>20}{'customers':>11}{'+Lap(2)':>10}{'>= thr?':>9}")
    for bb, cc, nn in zip(ub, cb, noisy):
        print(f"  {bb:>4}{f'[{2**bb:,}, {2**(bb+1):,})':>20}{cc:>11,}{nn:>10.1f}"
              f"{('YES' if nn >= thr else 'no'):>9}")
    ok = ub[noisy >= thr]
    out["1  ApproxBounds"] = 2.0 ** (ok.max() + 1)
    print(f"  -> rightmost bin clearing the threshold = {ok.max()}, bound = 2^{ok.max()+1} "
          f"= {out['1  ApproxBounds']:,.0f}")

    # ---- 4. Approach 1b ----------------------------------------------------
    print("\n" + "-" * 74)
    print("APPROACH 1b  tail-fraction variant: keep the bin where the top alpha of")
    print("             customers starts, instead of thresholding on the count")
    print("-" * 74)
    tail = np.cumsum(noisy[::-1])[::-1] / noisy.sum()
    print(f"  {'bin':>4}{'upper edge':>14}{'share at or above':>20}")
    for bb, tf in zip(ub, tail):
        print(f"  {bb:>4}{2**(bb+1):>14,}{tf:>19.1%}")
    for alpha in (0.10, 0.20, 0.30):
        idx = np.where(tail >= alpha)[0]
        k = ub[idx[-1]]
        out[f"1b tail alpha={alpha:.0%}"] = 2.0 ** (k + 1)
        print(f"  alpha={alpha:.0%} -> last bin with share >= alpha is {k}, "
              f"bound = {2.0**(k+1):,.0f}")

    # ---- 5. Approach 2 -----------------------------------------------------
    print("\n" + "-" * 74)
    print("APPROACH 2  frozen filterless maximum")
    print("-" * 74)
    print(f"  release the FILTERED max U = {u_id:,.0f}, using the FILTERLESS max as its")
    print(f"  sensitivity:  U* = U + Lap(U_nf / eps_b) = {u_id:,.0f} + Lap({u_nf/eps_b:,.0f})")
    d = np.clip(u_id + rng.laplace(0.0, u_nf / eps_b, 9), 0, None)
    print(f"  nine draws: " + ", ".join(f"{x:,.0f}" for x in d))
    out["2  frozen filterless max"] = float(np.median(np.clip(
        u_id + rng.laplace(0.0, u_nf / eps_b, 2001), 0, None)))
    print(f"  median over 2001 draws = {out['2  frozen filterless max']:,.0f}    "
          f"(noise is {u_nf/eps_b/u_id:.0f}x the quantity being released)")
    nb = int(np.ceil(np.log2(u_nf))) + 1
    thr2 = _lap_quantile_threshold(nb, eps_b)
    ok2 = ub[noisy >= thr2]
    out["2b ApproxBounds, fewer bins"] = 2.0 ** (ok2.max() + 1)
    print(f"  2b: use U_nf to cut 64 bins -> {nb}. threshold {thr:.1f} -> {thr2:.1f}, "
          f"bound = {out['2b ApproxBounds, fewer bins']:,.0f}")

    # ---- 6. Approach 3 -----------------------------------------------------
    edges = np.array([float(np.quantile(a, q)) for q in
                      [0.9, 0.8, 0.7, 0.6, 0.5, 0.4, 0.3, 0.2, 0.1, 0.0]])
    idx = np.clip(np.searchsorted(-edges, -t, side="left") - 1, 0, 9)
    c3 = np.array([(idx == i).sum() for i in range(10)], dtype=float)
    u3 = c3 + rng.laplace(0.0, 2.0 / eps_b, 10)
    cum = np.cumsum(u3) / u3.sum()
    print("\n" + "-" * 74)
    print("APPROACH 3  frozen filterless metadata: 10 bins at the DECILES of the")
    print("            filterless distribution, then bin the web totals into them")
    print("-" * 74)
    print(f"  {'bin':>4}{'U_k (decile)':>15}{'covers':>22}{'web custs':>11}{'+Lap(2)':>10}{'cum':>8}")
    for i in range(10):
        rangetxt = f"({edges[i+1]:,.0f}, {edges[i]:,.0f}]" if i < 9 else f"(0, {edges[9]:,.0f}]"
        print(f"  {i+1:>4}{edges[i]:>15,.0f}{rangetxt:>22}{int(c3[i]):>11,}{u3[i]:>10.1f}{cum[i]:>8.1%}")
    for alpha in (0.10, 0.20, 0.30):
        k = int(np.argmax(cum >= alpha))
        out[f"3  metadata alpha={alpha:.0%}"] = float(edges[k])
        print(f"  alpha={alpha:.0%} -> leftmost k with cum >= alpha is {k+1}, "
              f"bound U_{k+1} = {edges[k]:,.0f}")

    # ---- 7. what each bound does to the answer -----------------------------
    print("\n" + "=" * 74)
    print("RESULT   apply each bound to the query, one row per month")
    print("=" * 74)
    truth = {m: float(v) for m, v in con.execute(
        "SELECT month, sum(amount) FROM sales WHERE channel='web' GROUP BY month").fetchall()}
    print(f"  {'approach':<30}{'bound':>12}{'vs ideal':>10}{'clip loss':>11}{'noise':>10}{'error':>9}")
    print("  " + "-" * 72)
    for name, B in sorted(out.items()):
        rel, clip = [], []
        for m, tv in truth.items():
            cs = float(con.execute(
                f"""SELECT sum(least(x.w, {B} * x.w / x.tot)) FROM
                    (SELECT s.custkey, sum(s.amount) AS w, any_value(c.t) AS tot
                     FROM sales s JOIN contrib c ON c.custkey = s.custkey
                     WHERE s.channel='web' AND s.month={m} AND c.t > 0
                     GROUP BY s.custkey) x""").fetchone()[0] or 0.0)
            clip.append(1 - cs / tv)
            rel += [abs(cs + rng.laplace(0, B / eps_agg) - tv) / tv for _ in range(400)]
        print(f"  {name:<30}{B:>12,.0f}{B/u_id:>9.2f}x{np.mean(clip):>10.1%}"
              f"{B/eps_agg:>10,.0f}{np.median(rel):>8.1%}")
    print()


if __name__ == "__main__":
    main()
