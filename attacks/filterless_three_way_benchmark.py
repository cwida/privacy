#!/usr/bin/env python3
"""Compare the implemented sampled/full filterless designs with Google-style DP.

The benchmark follows the current dp_filterless execution shape:

* rows are pre-aggregated by (logical PU, SQL group), with separate filtered-answer and
  unfiltered-histogram partials;
* filterless p=0 uses every unfiltered PU partial, while p=6 uses a deterministic 1/64
  PU sample with support weight 64;
* the group cap is applied to logical (PU, SQL group) pairs before bound estimation;
* filterless chooses a factor-4 bound independently in each SQL group from an unnoised fixed
  histogram, under the explicit static-dataset assumption;
* Google-style DP chooses one query-local bound with ApproxBounds;
* grouped queries spend half of epsilon on private partition selection, matching the extension.

Every arm gets the same per-query epsilon/delta. Google is tuned over bound/value budget fractions;
filterless spends no per-query epsilon on its fixed histogram and spends its aggregate budget on
the released value. Missing/suppressed groups are scored as zero against the uncapped true result.
The Google arm is the repository's existing mechanism simulation, not a linked Google DP library
runtime.
"""

import argparse
import numpy as np

from filterless_encoded_sampling_eval import (CASES, bin_indices, bin_upper, execution_queries,
                                                load_partials, splitmix64, timed)
from fineness_sweep import approx_bounds, tau


DELTA = 1e-6
N_BINS = 30

SYNTHETIC_CASES = {
    "syn bounded/12": (
        "pu", "j % 12", "bounded", "pu % 5 = 0", "FROM synthetic_events",
    ),
    "syn log-skew/120": (
        "pu", "g120", "log_skew", "pu % 5 = 0", "FROM synthetic_events",
    ),
    "syn Pareto/1200": (
        "pu", "g1200", "pareto", "pu % 5 = 0", "FROM synthetic_events",
    ),
    "syn billionaire excluded": (
        "pu", "g120", "CASE WHEN pu % 1000 = 0 THEN 100 * bounded ELSE bounded END",
        "pu % 1000 <> 0", "FROM synthetic_events",
    ),
    "syn everyone+billionaires": (
        "pu", "g120", "CASE WHEN pu % 1000 = 0 THEN 100 * bounded ELSE bounded END",
        "true", "FROM synthetic_events",
    ),
    "syn COUNT/120": (
        "pu", "g120", "1.0", "pu % 5 = 0", "FROM synthetic_events",
    ),
}

AVG_CASES = {
    "AVG orders price/month": (
        "o_custkey", "strftime(o_orderdate, '%Y-%m')", "o_totalprice", "c_acctbal >= 8000",
        "FROM tpch.orders JOIN tpch.customer ON c_custkey = o_custkey",
    ),
    "AVG lineitem price/month": (
        "o_custkey", "strftime(l_shipdate, '%Y-%m')", "l_extendedprice", "l_quantity < 10",
        "FROM tpch.lineitem JOIN tpch.orders ON o_orderkey = l_orderkey",
    ),
    "AVG syn bounded/12": (
        "pu", "j % 12", "bounded", "pu % 5 = 0", "FROM synthetic_events",
    ),
    "AVG syn log-skew/120": (
        "pu", "g120", "log_skew", "pu % 5 = 0", "FROM synthetic_events",
    ),
}


def build_synthetic(con):
    con.execute("""CREATE TEMP TABLE synthetic_events AS
        SELECT pu, j, (pu*17+j*31)%120 g120, (pu*101+j*307)%1200 g1200,
               80.0+40.0*u bounded,
               exp(log(100.0)+3.0*(u-0.5)) log_skew,
               20.0/pow(greatest(1.0-u, 1e-6), 1.0/1.5) pareto
        FROM (SELECT pu, j, row_no,
                     (hash(pu*1009+j*97+row_no*17)%1000000)/1000001.0 u
              FROM range(10000) p(pu), range(20) q(j), range(3) r(row_no))""")


def dense_pus(pus):
    _, ids = np.unique(pus, return_inverse=True)
    return ids.astype(np.int64)


def capped_groups(pids, gids, eligible, cu):
    """Match the compiler's deterministic dense rank by SQL group within logical PU."""
    selected = np.flatnonzero(eligible)
    order = selected[np.lexsort((gids[selected], pids[selected]))]
    keep = np.zeros(len(pids), dtype=bool)
    if not len(order):
        return keep
    first = np.empty(len(order), dtype=bool)
    first[0] = True
    first[1:] = pids[order[1:]] != pids[order[:-1]]
    starts = np.maximum.accumulate(np.where(first, np.arange(len(order)), 0))
    rank = np.arange(len(order)) - starts
    keep[order[rank < cu]] = True
    return keep


def filterless_bounds(counts, support, rng, fuzzy_width):
    if fuzzy_width > 0:
        probability = np.clip((counts - (support - fuzzy_width)) / fuzzy_width, 0.0, 1.0)
        passing = rng.random(counts.shape) < probability
    else:
        passing = counts >= support
    found = passing.any(axis=1)
    levels = N_BINS - 1 - np.argmax(passing[:, ::-1], axis=1)
    levels = np.where(found, levels, -1)
    return np.where(found, bin_upper(levels), 0.0)


def score(output, truth):
    return float(np.abs(output - truth).sum() / max(np.abs(truth).sum(), 1.0))


def load_avg_partials(con, case):
    pu, group, value, predicate, from_sql = case
    rows = con.execute(
        f"""SELECT ({pu})::UBIGINT pu, ({group})::VARCHAR grp,
                   SUM({value}) FILTER (WHERE {predicate})::DOUBLE active_sum,
                   COUNT({value}) FILTER (WHERE {predicate})::DOUBLE active_count,
                   SUM({value}) FILTER (WHERE NOT ({predicate}))::DOUBLE inactive_sum,
                   COUNT({value}) FILTER (WHERE NOT ({predicate}))::DOUBLE inactive_count
            {from_sql}
            GROUP BY 1, 2"""
    ).fetchnumpy()
    _, group_ids = np.unique(rows["grp"].astype(str), return_inverse=True)
    return (
        rows["pu"].astype(np.uint64),
        group_ids.astype(np.int64),
        np.ma.filled(rows["active_sum"], np.nan).astype(np.float64),
        np.ma.filled(rows["active_count"], 0.0).astype(np.float64),
        np.ma.filled(rows["inactive_sum"], np.nan).astype(np.float64),
        np.ma.filled(rows["inactive_count"], 0.0).astype(np.float64),
        int(group_ids.max()) + 1,
    )


def arm_error(arm, gids, active, group_count, truth, cu, bound_fraction, epsilon, support,
              fuzzy_width, trial, state):
    grouped = group_count > 1
    budget_units = 2.0 if grouped else 1.0
    component_epsilon = epsilon / budget_units
    eta_epsilon = epsilon / budget_units if grouped else 0.0
    bounds_epsilon = component_epsilon * bound_fraction
    value_epsilon = (component_epsilon * (1.0 - bound_fraction)
                     if arm == "google" else component_epsilon)
    rng = np.random.default_rng(0xC0FFEE + trial * 1009 + {"google": 1, "full": 2, "sampled": 3}[arm])

    kept_active, active_support, counts, weight = state
    if arm == "google":
        vals = active[kept_active]
        if not len(vals):
            upper = 0.0
        else:
            upper = approx_bounds(vals, bounds_epsilon, rng, n_bins=46,
                                  p_success=1.0 - DELTA / budget_units,
                                  l1_sensitivity=float(cu), failure_bound=2.0 ** 46,
                                  max_failure=DELTA / budget_units)
        bounds = np.full(group_count, upper)
    else:
        bounds = filterless_bounds(counts, support, rng, fuzzy_width)

    clipped = np.bincount(gids[kept_active],
                          weights=np.minimum(active[kept_active], bounds[gids[kept_active]]),
                          minlength=group_count)
    output = clipped + rng.laplace(0.0, cu * bounds / value_epsilon)
    if grouped:
        threshold = tau(eta_epsilon, DELTA / budget_units, cu)
        released = active_support + rng.laplace(0.0, cu / eta_epsilon, group_count) >= threshold
        output = np.where(released, output, 0.0)
    return score(output, truth)


def benchmark_case(partials, epsilon, support, fuzzy_width, trials, sample_salts, sample_bits):
    pus, gids, active, inactive, group_count = partials
    pids = dense_pus(pus)
    active_mask = np.isfinite(active)
    unfiltered_mask = active_mask | np.isfinite(inactive)
    unfiltered = np.nansum(np.column_stack((active, inactive)), axis=1)
    truth = np.bincount(gids[active_mask], weights=active[active_mask], minlength=group_count)
    active_groups_per_pu = np.bincount(pids[active_mask], minlength=int(pids.max()) + 1)
    max_groups = max(int(active_groups_per_pu.max()), 1)
    candidates = sorted({1, 2, 4, 8, 16, 32, max_groups} & set(range(1, max_groups + 1)))
    fractions = (0.05, 0.10, 0.25, 0.50)
    sampled_masks = []
    for salt in range(sample_salts):
        sampled_pu = ((splitmix64(np.arange(int(pids.max()) + 1, dtype=np.uint64), salt)
                       >> np.uint64(64 - sample_bits)) == 0)
        sampled_masks.append(unfiltered_mask & sampled_pu[pids])

    states = {}
    for arm in ("google", "full", "sampled"):
        salt_range = range(sample_salts) if arm == "sampled" else range(1)
        for salt in salt_range:
            histogram_members = (np.zeros(len(active), dtype=bool) if arm == "google" else
                                 unfiltered_mask if arm == "full" else sampled_masks[salt])
            eligible = active_mask | histogram_members
            weight = {"google": 0.0, "full": 1.0, "sampled": float(1 << sample_bits)}[arm]
            for cu in candidates:
                keep = capped_groups(pids, gids, eligible, cu)
                kept_active = active_mask & keep
                active_support = np.bincount(
                    gids[kept_active], minlength=group_count).astype(np.float64)
                counts = None
                if arm != "google":
                    counts = np.zeros((group_count, N_BINS), dtype=np.float64)
                    kept_histogram = histogram_members & keep
                    np.add.at(counts,
                              (gids[kept_histogram], bin_indices(unfiltered[kept_histogram])), weight)
                states[(arm, salt, cu)] = (kept_active, active_support, counts, weight)

    result = {}
    for arm in ("google", "full", "sampled"):
        best = None
        arm_fractions = fractions if arm == "google" else (0.0,)
        for cu in candidates:
            for fraction in arm_fractions:
                errors = [arm_error(arm, gids, active, group_count, truth, cu, fraction,
                                    epsilon, support, fuzzy_width, t,
                                    states[(arm, t % sample_salts if arm == "sampled" else 0, cu)])
                          for t in range(trials)]
                candidate = (float(np.median(errors)), cu, fraction,
                             float(np.quantile(errors, 0.9)))
                if best is None or candidate[0] < best[0]:
                    best = candidate
        result[arm] = best
    return result


def avg_arm_error(arm, gids, active_sum, active_count, group_count, truth, cu, bound_fraction,
                  epsilon, support, fuzzy_width, trial, state):
    grouped = group_count > 1
    budget_units = 2.0 if grouped else 1.0
    visible_epsilon = epsilon / budget_units
    component_epsilon = visible_epsilon / 2.0
    eta_epsilon = epsilon / budget_units if grouped else 0.0
    bounds_epsilon = component_epsilon * bound_fraction
    value_epsilon = (component_epsilon * (1.0 - bound_fraction)
                     if arm == "google" else component_epsilon)
    rng = np.random.default_rng(0xA66A6E + trial * 1009 +
                                {"google": 1, "full": 2, "sampled": 3}[arm])
    kept_active, active_support, sum_counts, count_counts = state
    if arm == "google":
        if kept_active.any():
            sum_upper = approx_bounds(active_sum[kept_active], bounds_epsilon, rng, n_bins=46,
                                      p_success=1.0 - DELTA / budget_units,
                                      l1_sensitivity=float(cu), failure_bound=2.0 ** 46,
                                      max_failure=DELTA / budget_units)
            count_upper = approx_bounds(active_count[kept_active], bounds_epsilon, rng, n_bins=46,
                                        p_success=1.0 - DELTA / budget_units,
                                        l1_sensitivity=float(cu), failure_bound=2.0 ** 46,
                                        max_failure=DELTA / budget_units)
        else:
            sum_upper = count_upper = 0.0
        sum_bounds = np.full(group_count, sum_upper)
        count_bounds = np.full(group_count, count_upper)
    else:
        sum_bounds = filterless_bounds(sum_counts, support, rng, fuzzy_width)
        count_bounds = filterless_bounds(count_counts, support, rng, fuzzy_width)

    sum_result = np.bincount(
        gids[kept_active],
        weights=np.minimum(active_sum[kept_active], sum_bounds[gids[kept_active]]),
        minlength=group_count,
    )
    count_result = np.bincount(
        gids[kept_active],
        weights=np.minimum(active_count[kept_active], count_bounds[gids[kept_active]]),
        minlength=group_count,
    )
    sum_result += rng.laplace(0.0, cu * sum_bounds / value_epsilon)
    count_result += rng.laplace(0.0, cu * count_bounds / value_epsilon)
    output = np.divide(sum_result, count_result, out=np.zeros_like(sum_result), where=count_result > 0)
    if grouped:
        threshold = tau(eta_epsilon, DELTA / budget_units, cu)
        released = active_support + rng.laplace(0.0, cu / eta_epsilon, group_count) >= threshold
        output = np.where(released, output, 0.0)
    return score(output, truth)


def benchmark_avg_case(partials, epsilon, support, fuzzy_width, trials, sample_salts, sample_bits):
    pus, gids, active_sum, active_count, inactive_sum, inactive_count, group_count = partials
    pids = dense_pus(pus)
    active_mask = np.isfinite(active_sum) & (active_count > 0)
    unfiltered_count = active_count + inactive_count
    unfiltered_mask = unfiltered_count > 0
    unfiltered_sum = np.nansum(np.column_stack((active_sum, inactive_sum)), axis=1)
    truth_sum = np.bincount(gids[active_mask], weights=active_sum[active_mask], minlength=group_count)
    truth_count = np.bincount(gids[active_mask], weights=active_count[active_mask], minlength=group_count)
    truth = np.divide(truth_sum, truth_count, out=np.zeros_like(truth_sum), where=truth_count > 0)
    active_groups_per_pu = np.bincount(pids[active_mask], minlength=int(pids.max()) + 1)
    max_groups = max(int(active_groups_per_pu.max()), 1)
    candidates = sorted({1, 2, 4, 8, 16, 32, max_groups} & set(range(1, max_groups + 1)))
    fractions = (0.05, 0.10, 0.25, 0.50)
    sampled_masks = []
    for salt in range(sample_salts):
        sampled_pu = ((splitmix64(np.arange(int(pids.max()) + 1, dtype=np.uint64), salt)
                       >> np.uint64(64 - sample_bits)) == 0)
        sampled_masks.append(unfiltered_mask & sampled_pu[pids])

    states = {}
    for arm in ("google", "full", "sampled"):
        salt_range = range(sample_salts) if arm == "sampled" else range(1)
        for salt in salt_range:
            histogram_members = (np.zeros(len(active_sum), dtype=bool) if arm == "google" else
                                 unfiltered_mask if arm == "full" else sampled_masks[salt])
            eligible = active_mask | histogram_members
            weight = {"google": 0.0, "full": 1.0, "sampled": float(1 << sample_bits)}[arm]
            for cu in candidates:
                keep = capped_groups(pids, gids, eligible, cu)
                kept_active = active_mask & keep
                active_support = np.bincount(gids[kept_active], minlength=group_count).astype(np.float64)
                sum_counts = count_counts = None
                if arm != "google":
                    kept_histogram = histogram_members & keep
                    sum_counts = np.zeros((group_count, N_BINS), dtype=np.float64)
                    count_counts = np.zeros((group_count, N_BINS), dtype=np.float64)
                    weights = np.full(kept_histogram.sum(), weight)
                    np.add.at(sum_counts,
                              (gids[kept_histogram], bin_indices(unfiltered_sum[kept_histogram])), weights)
                    np.add.at(count_counts,
                              (gids[kept_histogram], bin_indices(unfiltered_count[kept_histogram])), weights)
                states[(arm, salt, cu)] = (kept_active, active_support, sum_counts, count_counts)

    result = {}
    for arm in ("google", "full", "sampled"):
        best = None
        arm_fractions = fractions if arm == "google" else (0.0,)
        for cu in candidates:
            for fraction in arm_fractions:
                errors = [avg_arm_error(
                    arm, gids, active_sum, active_count, group_count, truth, cu, fraction, epsilon,
                    support, fuzzy_width, trial,
                    states[(arm, trial % sample_salts if arm == "sampled" else 0, cu)])
                    for trial in range(trials)]
                candidate = (float(np.median(errors)), cu, fraction, float(np.quantile(errors, 0.9)))
                if best is None or candidate[0] < best[0]:
                    best = candidate
        result[arm] = best
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--db", default="/tmp/privacy_filterless_tpch_sf1_v2.db")
    parser.add_argument("--epsilon", type=float, default=1.0)
    parser.add_argument("--support", type=float, default=500.0)
    parser.add_argument("--fuzzy-width", type=float, default=0.0,
                        help="linear acceptance interval ending at support (zero is a hard threshold)")
    parser.add_argument("--trials", type=int, default=32)
    parser.add_argument("--sample-salts", type=int, default=8)
    parser.add_argument("--sample-bits", type=int, default=6)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--skip-timing", action="store_true")
    parser.add_argument("--include-synthetic", action="store_true")
    parser.add_argument("--only-synthetic", action="store_true")
    parser.add_argument("--include-avg", action="store_true")
    parser.add_argument("--only-avg", action="store_true")
    args = parser.parse_args()
    if args.epsilon <= 0 or args.support <= 0 or args.trials <= 0 or args.sample_salts <= 0:
        raise ValueError("epsilon, support, trials, and sample-salts must be positive")
    if args.sample_bits <= 0 or args.sample_bits > 63:
        raise ValueError("sample-bits must be between 1 and 63")
    if args.fuzzy_width < 0:
        raise ValueError("fuzzy-width cannot be negative")

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{args.db}' AS tpch (READ_ONLY)")
    cases = {} if args.only_synthetic or args.only_avg else dict(CASES)
    if args.include_synthetic or args.only_synthetic or args.include_avg or args.only_avg:
        build_synthetic(con)
    if args.include_synthetic or args.only_synthetic:
        cases.update(SYNTHETIC_CASES)

    print(f"utility: static dataset; epsilon={args.epsilon:g}, delta={DELTA:g}, "
          f"support={args.support:g}, "
          f"trials={args.trials}, sampled=1/{1 << args.sample_bits} "
          f"({args.sample_salts} fixed hash salts)")
    if args.fuzzy_width:
        print(f"filterless bin acceptance rises linearly from support "
              f"{args.support - args.fuzzy_width:g} to {args.support:g}")
    print("Google tunes C_u and bound budget; filterless tunes C_u and reuses an unnoised bound")
    print("relative L1 vs uncapped truth")
    header = (f"{'query':<28}{'Google':>12}{'full p=0':>12}{'sampled':>12}  "
              f"{'best Google cfg':<18}{'best full cfg':<18}{'best sampled cfg':<18}")
    print(header)
    print("-" * len(header))
    for name, case in cases.items():
        partials = load_partials(con, case)
        result = benchmark_case(partials, args.epsilon, args.support, args.fuzzy_width, args.trials,
                                args.sample_salts, args.sample_bits)
        def metric(arm):
            return f"{100 * result[arm][0]:.2f}%"
        def cfg(arm):
            return f"C_u={result[arm][1]}, f_b={result[arm][2]:.2f}"
        print(f"{name:<28}{metric('google'):>12}{metric('full'):>12}{metric('sampled'):>12}  "
              f"{cfg('google'):<18}{cfg('full'):<18}{cfg('sampled'):<18}", flush=True)

    if args.include_avg or args.only_avg:
        print("\nAVG utility (independently bounded/noised SUM and COUNT, followed by division)")
        print(header)
        print("-" * len(header))
        for name, case in AVG_CASES.items():
            partials = load_avg_partials(con, case)
            result = benchmark_avg_case(partials, args.epsilon, args.support, args.fuzzy_width,
                                        args.trials, args.sample_salts, args.sample_bits)

            def avg_metric(arm):
                return f"{100 * result[arm][0]:.2f}%"

            def avg_cfg(arm):
                return f"C_u={result[arm][1]}, f_b={result[arm][2]:.2f}"

            print(f"{name:<28}{avg_metric('google'):>12}{avg_metric('full'):>12}"
                  f"{avg_metric('sampled'):>12}  {avg_cfg('google'):<18}{avg_cfg('full'):<18}"
                  f"{avg_cfg('sampled'):<18}", flush=True)

    if args.skip_timing:
        return

    print("\nrelational execution time (median seconds; Google column is its filtered pre-aggregation)")
    header = (f"{'query':<28}{'Google input':>13}{'full p=0':>12}{'sampled':>12}"
              f"{'sample/full':>14}{'sample/Google':>15}")
    print(header)
    print("-" * len(header))
    for name, case in cases.items():
        google_sql, full_sql, sampled_sql = execution_queries(case, args.sample_bits)
        google_time = timed(con, google_sql, args.repeats)
        full_time = timed(con, full_sql, args.repeats)
        sampled_time = timed(con, sampled_sql, args.repeats)
        print(f"{name:<28}{google_time:>13.3f}{full_time:>12.3f}{sampled_time:>12.3f}"
              f"{sampled_time/full_time:>13.2f}x{sampled_time/google_time:>14.2f}x")


if __name__ == "__main__":
    main()
