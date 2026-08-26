#!/usr/bin/env python3
"""Evaluate sampling error for the implemented fixed-sample filterless design.

The histogram contains only deterministic fixed-sample PUs. Their complete unfiltered per-PU
contributions receive Horvitz-Thompson weight 2^p. The answer separately contains every
qualifying PU's filtered contribution.

This is a utility and execution-cost benchmark, not a privacy proof. The unnoised p=0
histogram is the reference. Different SplitMix salts model different fixed hash functions.
"""

import argparse
import time

import numpy as np
from scipy.stats import binom


CASES = {
    "orders/month balance": (
        "o_custkey",
        "strftime(o_orderdate, '%Y-%m')",
        "o_totalprice",
        "c_acctbal >= 8000",
        "FROM tpch.orders JOIN tpch.customer ON c_custkey = o_custkey",
    ),
    "orders/month price": (
        "o_custkey",
        "strftime(o_orderdate, '%Y-%m')",
        "o_totalprice",
        "o_totalprice >= 200000",
        "FROM tpch.orders JOIN tpch.customer ON c_custkey = o_custkey",
    ),
    "lineitem/month quantity": (
        "o_custkey",
        "strftime(l_shipdate, '%Y-%m')",
        "l_extendedprice",
        "l_quantity < 10",
        "FROM tpch.lineitem JOIN tpch.orders ON o_orderkey = l_orderkey",
    ),
    "lineitem/month|nation": (
        "o_custkey",
        "strftime(l_shipdate, '%Y-%m') || '|' || CAST(c_nationkey AS VARCHAR)",
        "l_extendedprice",
        "l_quantity < 10",
        "FROM tpch.lineitem JOIN tpch.orders ON o_orderkey = l_orderkey "
        "JOIN tpch.customer ON c_custkey = o_custkey",
    ),
}


def splitmix64(values, salt):
    with np.errstate(over="ignore"):
        salt_value = (0x9E3779B97F4A7C15 * (salt + 1)) & ((1 << 64) - 1)
        x = values.astype(np.uint64) + np.uint64(salt_value)
        x = (x ^ (x >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
        x = (x ^ (x >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
        return x ^ (x >> np.uint64(31))


def bin_indices(values):
    scaled = np.minimum(np.abs(values) * float(1 << 27), np.iinfo(np.int64).max)
    bits = np.where(scaled > 0, np.floor(np.log2(np.maximum(scaled, 1.0))).astype(np.int64), 0)
    return np.minimum(bits // 2, 29)


def bin_upper(indices):
    return np.exp2((indices + 1) * 2) / float(1 << 27)


def histogram(groups, levels, weights, group_count):
    result = np.zeros((group_count, 30), dtype=np.float64)
    np.add.at(result, (groups, levels), weights)
    return result


def select_bounds(counts, support):
    passing = counts >= support
    has_bound = passing.any(axis=1)
    selected = counts.shape[1] - 1 - np.argmax(passing[:, ::-1], axis=1)
    selected = np.where(has_bound, selected, -1)
    return selected, np.where(has_bound, bin_upper(selected), 0.0)


def expected_bound_match(counts, support, sample_bits):
    """Exact per-group match probability under independent fixed-rate PU sampling."""
    weight = 1 << sample_bits
    required_samples = int(np.ceil(support / weight))
    acceptance = binom.sf(required_samples - 1, counts.astype(np.int64), 1.0 / weight)
    reference_level, _ = select_bounds(counts, support)
    probabilities = np.empty(len(counts), dtype=np.float64)
    for group, level in enumerate(reference_level):
        if level < 0:
            probabilities[group] = np.prod(1.0 - acceptance[group])
        else:
            probabilities[group] = (acceptance[group, level] *
                                    np.prod(1.0 - acceptance[group, level + 1:]))
    return float(np.mean(probabilities))


def load_partials(con, case):
    pu, group, value, predicate, from_sql = case
    rows = con.execute(
        f"""SELECT ({pu})::UBIGINT pu, ({group})::VARCHAR grp,
                   SUM({value}) FILTER (WHERE {predicate})::DOUBLE active_value,
                   SUM({value}) FILTER (WHERE NOT ({predicate}))::DOUBLE inactive_value
            {from_sql}
            GROUP BY 1, 2"""
    ).fetchnumpy()
    _, group_ids = np.unique(rows["grp"].astype(str), return_inverse=True)
    return (
        rows["pu"].astype(np.uint64),
        group_ids.astype(np.int64),
        np.ma.filled(rows["active_value"], np.nan).astype(np.float64),
        np.ma.filled(rows["inactive_value"], np.nan).astype(np.float64),
        int(group_ids.max()) + 1,
    )


def evaluate_sampling(pus, groups, active, inactive, group_count, sample_bits, salts, support):
    active_mask = np.isfinite(active)
    unfiltered_mask = np.isfinite(active) | np.isfinite(inactive)
    unfiltered = np.nansum(np.column_stack((active, inactive)), axis=1)
    full = histogram(groups[unfiltered_mask], bin_indices(unfiltered[unfiltered_mask]),
                     np.ones(unfiltered_mask.sum()), group_count)
    reference_level, reference_bound = select_bounds(full, support)
    reference_answer = np.bincount(
        groups[active_mask],
        weights=np.minimum(active[active_mask], reference_bound[groups[active_mask]]),
        minlength=group_count,
    )

    matches = []
    under = []
    over = []
    hist_errors = []
    answer_l1_errors = []
    answer_changed = []
    supported_matches = []
    false_bounds = []
    lost_bounds = []
    weight = float(1 << sample_bits)
    for salt in range(salts):
        selected_pus = (splitmix64(pus, salt) >> np.uint64(64 - sample_bits)) == 0
        sampled = unfiltered_mask & selected_pus
        estimate = histogram(groups[sampled], bin_indices(unfiltered[sampled]),
                             np.full(sampled.sum(), weight), group_count)
        level, bound = select_bounds(estimate, support)
        answer = np.bincount(
            groups[active_mask],
            weights=np.minimum(active[active_mask], bound[groups[active_mask]]),
            minlength=group_count,
        )
        matches.append(np.mean(level == reference_level))
        under.append(np.mean((level >= 0) & (reference_level >= 0) & (level < reference_level)))
        over.append(np.mean(level > reference_level))
        hist_errors.append(np.median(np.abs(estimate - full).sum(axis=1) / np.maximum(full.sum(axis=1), 1.0)))
        answer_delta = np.abs(answer - reference_answer)
        answer_l1_errors.append(answer_delta.sum() / max(np.abs(reference_answer).sum(), 1.0))
        answer_changed.append(np.mean(answer_delta > 1e-9))
        reference_supported = reference_level >= 0
        supported_matches.append(
            np.mean(level[reference_supported] == reference_level[reference_supported])
            if reference_supported.any() else np.nan
        )
        false_bounds.append(np.mean((level >= 0) & ~reference_supported))
        lost_bounds.append(np.mean((level < 0) & reference_supported))
    return tuple(float(np.mean(x)) for x in
                 (matches, under, over, hist_errors, answer_l1_errors, answer_changed,
                 supported_matches, false_bounds, lost_bounds)) + (float(np.mean(reference_level >= 0)),)


def evaluate_supports(pus, groups, active, inactive, group_count, sample_bits, salts, supports):
    """Evaluate several thresholds while reusing each expensive sampled histogram."""
    active_mask = np.isfinite(active)
    unfiltered_mask = np.isfinite(active) | np.isfinite(inactive)
    unfiltered = np.nansum(np.column_stack((active, inactive)), axis=1)
    full = histogram(groups[unfiltered_mask], bin_indices(unfiltered[unfiltered_mask]),
                     np.ones(unfiltered_mask.sum()), group_count)
    references = {}
    for support in supports:
        level, bound = select_bounds(full, support)
        answer = np.bincount(
            groups[active_mask], weights=np.minimum(active[active_mask], bound[groups[active_mask]]),
            minlength=group_count)
        references[support] = (level, answer)

    metrics = {support: [[] for _ in range(9)] for support in supports}
    weight = float(1 << sample_bits)
    for salt in range(salts):
        selected_pus = (splitmix64(pus, salt) >> np.uint64(64 - sample_bits)) == 0
        sampled = unfiltered_mask & selected_pus
        estimate = histogram(groups[sampled], bin_indices(unfiltered[sampled]),
                             np.full(sampled.sum(), weight), group_count)
        for support in supports:
            reference_level, reference_answer = references[support]
            level, bound = select_bounds(estimate, support)
            answer = np.bincount(
                groups[active_mask], weights=np.minimum(active[active_mask], bound[groups[active_mask]]),
                minlength=group_count)
            reference_supported = reference_level >= 0
            delta = np.abs(answer - reference_answer)
            values = (
                np.mean(level == reference_level),
                np.mean((level >= 0) & (reference_level >= 0) & (level < reference_level)),
                np.mean(level > reference_level),
                np.median(np.abs(estimate - full).sum(axis=1) / np.maximum(full.sum(axis=1), 1.0)),
                delta.sum() / max(np.abs(reference_answer).sum(), 1.0),
                np.mean(delta > 1e-9),
                (np.mean(level[reference_supported] == reference_level[reference_supported])
                 if reference_supported.any() else np.nan),
                np.mean((level >= 0) & ~reference_supported),
                np.mean((level < 0) & reference_supported),
            )
            for bucket, value in zip(metrics[support], values):
                bucket.append(value)
    return {
        support: tuple(float(np.mean(x)) for x in metrics[support]) +
                 (float(np.mean(references[support][0] >= 0)),
                  expected_bound_match(full, support, sample_bits))
        for support in supports
    }


def timed(con, sql, repeats):
    con.execute(sql).fetchall()
    samples = []
    for _ in range(repeats):
        start = time.perf_counter()
        con.execute(sql).fetchall()
        samples.append(time.perf_counter() - start)
    return float(np.median(samples))


def execution_queries(case, sample_bits):
    pu, group, value, predicate, from_sql = case
    sample = f"(hash({pu}) >> {64 - sample_bits}) = 0"
    filtered = f"""SELECT {pu}, {group}, SUM({value}) {from_sql}
                   WHERE {predicate} GROUP BY 1, 2"""
    full = f"""SELECT {pu}, {group}, SUM({value}) FILTER (WHERE {predicate}), SUM({value})
               {from_sql} GROUP BY 1, 2"""
    sampled = f"""SELECT {pu}, {group}, SUM({value}) FILTER (WHERE {predicate}), SUM({value})
                  {from_sql} WHERE ({predicate}) OR {sample} GROUP BY 1, 2"""
    return filtered, full, sampled


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--db", default="/tmp/privacy_filterless_tpch_sf1_v2.db")
    parser.add_argument("--sample-bits", type=int, nargs="+", default=[2, 4, 6])
    parser.add_argument("--salts", type=int, default=64)
    parser.add_argument("--support", type=float, default=40.0)
    parser.add_argument("--supports", type=float, nargs="+",
                        help="evaluate several support thresholds without reloading the data")
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--skip-timing", action="store_true")
    args = parser.parse_args()
    if any(bits <= 0 or bits > 63 for bits in args.sample_bits):
        raise ValueError("sample bits must be in [1, 63]")

    import duckdb

    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{args.db}' AS tpch (READ_ONLY)")

    supports = args.supports or [args.support]
    print(f"supports={','.join(f'{x:g}' for x in supports)}; "
          f"{args.salts} deterministic hash salts; histograms are not noised")
    header = (f"{'case':<28}{'T':>7}{'p':>4}{'T/2^p':>9}{'ref':>8}{'match':>9}"
              f"{'math':>9}{'match|ref':>11}{'false':>9}{'lost':>9}{'answer L1':>12}")
    print(header)
    print("-" * len(header))
    for name, case in CASES.items():
        partials = load_partials(con, case)
        results = {bits: evaluate_supports(*partials, bits, args.salts, supports)
                   for bits in args.sample_bits}
        for support in supports:
            for bits in args.sample_bits:
                (match, _, _, _, answer_error, _, supported_match, false_bound, lost_bound,
                 reference_rate, mathematical_match) = results[bits][support]
                print(f"{name:<28}{support:>7.0f}{bits:>4}{support/(1 << bits):>9.1f}"
                      f"{100*reference_rate:>7.1f}%{100*match:>8.2f}%{100*mathematical_match:>8.2f}%"
                      f"{100*supported_match:>10.2f}%"
                      f"{100*false_bound:>8.2f}%{100*lost_bound:>8.2f}%"
                      f"{100*answer_error:>11.3f}%")

    if args.skip_timing:
        return
    print("\nrelational execution time (median seconds)")
    header = (f"{'case':<28}{'p':>4}{'rate':>9}{'filtered':>11}{'p=0 full':>11}"
              f"{'sampled':>11}{'sample/full':>13}{'sample/filter':>15}")
    print(header)
    print("-" * len(header))
    for name, case in CASES.items():
        filtered_sql, full_sql, _ = execution_queries(case, args.sample_bits[0])
        filtered_time = timed(con, filtered_sql, args.repeats)
        full_time = timed(con, full_sql, args.repeats)
        for bits in args.sample_bits:
            _, _, sampled_sql = execution_queries(case, bits)
            sampled_time = timed(con, sampled_sql, args.repeats)
            print(f"{name:<28}{bits:>4}{f'1/{1 << bits}':>9}{filtered_time:>11.3f}"
                  f"{full_time:>11.3f}{sampled_time:>11.3f}{sampled_time/full_time:>12.2f}x"
                  f"{sampled_time/filtered_time:>14.2f}x")


if __name__ == "__main__":
    main()
