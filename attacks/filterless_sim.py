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
        f"{'filter':<22}{'sel.':>7}{'grp':>5}{'fless':>8}{'l1crowd':>9}"
        f"{'oracle':>8}{'rung':>5}{'dp-mass':>9}{"dp-hist":>9}{'rung':>5}{'google':>8}"
    )
    print("-" * 93)
    for i, (label, filt) in enumerate(FILTER_LADDER):
        sel = selectivity(con, filt)
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
    con = open_db(args.db, args.sf)
    if args.bounds:
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
    ladder = FILTER_LADDER[:5]
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
    print(f"APPROX_BOUNDS threshold = ln(B/2P)/eps_select with B=40 bins, P=1e-6")
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
        thresh = np.log(40.0 / (2.0 * 1e-6)) / eps_sel
        ab = []
        for _ in range(200):
            noisy = hc + rng.laplace(0.0, 1.0 / eps_sel, hc.size)
            ok = hb[noisy >= thresh]
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


if __name__ == "__main__":
    main()
