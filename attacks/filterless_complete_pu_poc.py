#!/usr/bin/env python3
"""PoC for a two-channel filterless SUM over a fixed sample of complete PUs.

The answer channel contains every row satisfying the SQL predicate. The bound channel contains
every row for a deterministic sample of complete privacy units and never incorporates unsampled
qualifying partials. This script checks the logical rewrite with DuckDB SQL, measures histogram
sampling error and wrong-bin rates, and compares end-to-end DP utility with exact filterless and
query-local exponential bounds.

Scope is deliberate: nonnegative SUM, one group per PU (C_u=1), add/remove PU adjacency, public
group keys, and disjoint factor-spaced bins. It is a mechanism PoC, not an extension implementation.
"""

import argparse
from pathlib import Path

import numpy as np

from filterless_one_lane_prototype import (
	bin_levels,
	clipped_group_sums,
	histogram,
	private_histogram_selection,
	score,
)


FILTERS = (
	("random20", "(hash(pu*65537+row_id*17+11)%10000)<2000"),
	("rare2", "(hash(pu*104729+row_id*97+19)%10000)<200"),
	("high_rows", "base_value>=800"),
)


def select_supported_bounds(counts, support, factor):
	passing = counts >= support
	gate = passing.any(axis=1)
	level = counts.shape[1] - 1 - np.argmax(passing[:, ::-1], axis=1)
	level = np.where(gate, level, 0)
	return factor ** (level + 1), gate


def lane_mask(lanes, offset, sampled_lanes):
	return ((lanes - offset) % 64) < sampled_lanes


def load_per_pu(con, groups, value_column, filter_sql):
	rows = con.execute(f"""SELECT pu, pu%{groups} gid,
		SUM({value_column}) unfiltered_contribution,
		COALESCE(SUM({value_column}) FILTER (WHERE {filter_sql}), 0.0) filtered_contribution,
		COUNT(*) FILTER (WHERE {filter_sql})>0 qualifies,
		hash(pu*1140071481932319849::UBIGINT)%64 lane
	FROM complete_pu_facts
	GROUP BY pu, gid ORDER BY pu""").fetchnumpy()
	return (rows["gid"].astype(np.int64), rows["unfiltered_contribution"].astype(np.float64),
	        rows["filtered_contribution"].astype(np.float64), rows["qualifies"].astype(bool),
	        rows["lane"].astype(np.int64))


def assert_dual_channel_sql(con):
	"""Prove the OR scan gives an exact answer channel and a filter-independent sample channel."""
	filter_sql = FILTERS[0][1]
	con.execute(f"""CREATE OR REPLACE TEMP VIEW dual_channel AS
		WITH marked AS (
			SELECT *, {filter_sql} qualifies,
			       hash(pu*1140071481932319849::UBIGINT)%64=0 sampled
			FROM complete_pu_facts
		), candidates AS (
			SELECT * FROM marked WHERE qualifies OR sampled
		)
		SELECT pu, pu%12 gid,
		       SUM(bounded_value) FILTER (WHERE qualifies) filtered_contribution,
		       SUM(bounded_value) FILTER (WHERE sampled) sampled_unfiltered_contribution
		FROM candidates GROUP BY pu, gid""")

	answer_delta = con.execute(f"""WITH expected AS (
		SELECT pu, pu%12 gid, SUM(bounded_value) filtered_contribution
		FROM complete_pu_facts WHERE {filter_sql} GROUP BY pu, gid
	), actual AS (
		SELECT pu, gid, filtered_contribution FROM dual_channel
		WHERE filtered_contribution IS NOT NULL
	), differences AS (
		(SELECT * FROM actual EXCEPT ALL SELECT * FROM expected)
		UNION ALL
		(SELECT * FROM expected EXCEPT ALL SELECT * FROM actual)
	)
	SELECT COUNT(*) FROM differences""").fetchone()[0]
	if answer_delta != 0:
		raise AssertionError("dual-channel answer must equal the ordinary filtered SQL answer")

	sample_delta = con.execute("""WITH expected AS (
		SELECT pu, pu%12 gid, SUM(bounded_value) sampled_unfiltered_contribution
		FROM complete_pu_facts
		WHERE hash(pu*1140071481932319849::UBIGINT)%64=0 GROUP BY pu, gid
	), actual AS (
		SELECT pu, gid, sampled_unfiltered_contribution FROM dual_channel
		WHERE sampled_unfiltered_contribution IS NOT NULL
	), differences AS (
		(SELECT * FROM actual EXCEPT ALL SELECT * FROM expected)
		UNION ALL
		(SELECT * FROM expected EXCEPT ALL SELECT * FROM actual)
	)
	SELECT COUNT(*) FROM differences""").fetchone()[0]
	if sample_delta != 0:
		raise AssertionError("bound channel must contain complete contributions for sampled PUs")

	filter_histograms = []
	for _, alternate_filter in FILTERS:
		rows = con.execute(f"""WITH marked AS (
			SELECT *, {alternate_filter} qualifies,
			       hash(pu*1140071481932319849::UBIGINT)%64=0 sampled
			FROM complete_pu_facts
		), candidates AS (
			SELECT * FROM marked WHERE qualifies OR sampled
		), per_pu AS (
			SELECT pu, pu%12 gid,
			       SUM(bounded_value) FILTER (WHERE qualifies) filtered_contribution,
			       SUM(bounded_value) FILTER (WHERE sampled) sampled_unfiltered_contribution
			FROM candidates GROUP BY pu, gid
		)
		SELECT * FROM per_pu ORDER BY pu""").fetchnumpy()
		answer_delta = con.execute(f"""WITH marked AS (
			SELECT *, {alternate_filter} qualifies,
			       hash(pu*1140071481932319849::UBIGINT)%64=0 sampled
			FROM complete_pu_facts
		), candidates AS (
			SELECT * FROM marked WHERE qualifies OR sampled
		), actual AS (
			SELECT pu, pu%12 gid, SUM(bounded_value) FILTER (WHERE qualifies) filtered_contribution
			FROM candidates GROUP BY pu, gid
		), expected AS (
			SELECT pu, pu%12 gid, SUM(bounded_value) filtered_contribution
			FROM complete_pu_facts WHERE {alternate_filter} GROUP BY pu, gid
		), differences AS (
			(SELECT * FROM actual WHERE filtered_contribution IS NOT NULL EXCEPT ALL SELECT * FROM expected)
			UNION ALL
			(SELECT * FROM expected EXCEPT ALL SELECT * FROM actual WHERE filtered_contribution IS NOT NULL)
		)
		SELECT COUNT(*) FROM differences""").fetchone()[0]
		if answer_delta != 0:
			raise AssertionError("dual-channel answer must stay exact for every row filter")
		sample_mask = ~np.ma.getmaskarray(rows["sampled_unfiltered_contribution"])
		filter_histograms.append((rows["pu"][sample_mask], rows["gid"][sample_mask],
		                          rows["sampled_unfiltered_contribution"][sample_mask]))
	for candidate in filter_histograms[1:]:
		if not all(np.array_equal(reference, value)
		           for reference, value in zip(filter_histograms[0], candidate)):
			raise AssertionError("complete-PU bound channel must be identical across row filters")

	# The old mixed estimator would place an unsampled qualifying partial into the bound channel.
	# The corrected design must either contain a sampled PU's full 1000 or no bound value at all.
	con.execute("""CREATE OR REPLACE TEMP TABLE partial_pu_case AS
		SELECT * FROM (VALUES (7, 1.0, true), (7, 999.0, false)) t(pu, value, qualifies)""")
	partial = con.execute("""SELECT
		SUM(value) FILTER (WHERE qualifies) answer_value,
		SUM(value) FILTER (WHERE false) bound_value
	FROM partial_pu_case WHERE qualifies OR false""").fetchone()
	if partial != (1.0, None):
		raise AssertionError("an unsampled PU's partial contribution must not enter the bound channel")
	complete = con.execute("""SELECT
		SUM(value) FILTER (WHERE qualifies) answer_value,
		SUM(value) FILTER (WHERE true) bound_value
	FROM partial_pu_case WHERE qualifies OR true""").fetchone()
	if complete != (1.0, 1000.0):
		raise AssertionError("a sampled PU's complete contribution must enter the bound channel")


def sampling_statistics(gids, values, lanes, groups, args):
	levels = bin_levels(values, args.factor, args.bins)
	full = histogram(gids, levels, np.ones(len(gids)), groups, args.bins)
	exact_u, exact_gate = select_supported_bounds(full, args.support, args.factor)
	rows = []
	for sampled_lanes in args.sampled_lanes:
		errors = []
		matches = []
		under = []
		over = []
		lane_sum = np.zeros_like(full)
		for offset in range(64):
			selected = lane_mask(lanes, offset, sampled_lanes)
			weight = 64.0 / sampled_lanes
			counts = histogram(gids[selected], levels[selected], np.full(selected.sum(), weight),
			                   groups, args.bins)
			lane_sum += counts
			errors.append(score(counts, full))
			u, gate = select_supported_bounds(counts, args.support, args.factor)
			matches.append(np.mean((u == exact_u) & (gate == exact_gate)))
			under.append(np.mean(gate & exact_gate & (u < exact_u)))
			over.append(np.mean(gate & ((~exact_gate) | (u > exact_u))))
		if not np.array_equal(lane_sum / 64.0, full):
			raise AssertionError("mean across rotated deterministic samples must recover full histogram")
		rows.append((sampled_lanes, np.median(errors), np.percentile(errors, 95),
		             np.mean(matches), np.mean(under), np.mean(over)))
	return rows


def private_utility(gids, unfiltered, filtered, qualifies, lanes, groups, args):
	full_levels = bin_levels(unfiltered, args.factor, args.bins)
	query_levels = bin_levels(filtered[qualifies], args.factor, args.bins)
	full_counts = histogram(gids, full_levels, np.ones(len(gids)), groups, args.bins)
	query_counts = histogram(gids[qualifies], query_levels, np.ones(qualifies.sum()),
	                         groups, args.bins)
	truth = np.bincount(gids[qualifies], weights=filtered[qualifies], minlength=groups)
	rng = np.random.default_rng(args.seed + groups + int(unfiltered.sum()) % 1000003)
	methods = {"full": {"errors": [], "gates": [], "bounds": []},
	           "query": {"errors": [], "gates": [], "bounds": []}}
	for sampled_lanes in args.sampled_lanes:
		methods[f"sample{sampled_lanes}"] = {"errors": [], "gates": [], "bounds": []}

	for trial in range(args.trials):
		full_u, full_gate, _ = private_histogram_selection(
			full_counts, 1.0, args.eps_bounds, args.delta, args.factor, rng)
		query_u, query_gate, _ = private_histogram_selection(
			query_counts, 1.0, args.eps_bounds, args.delta, args.factor, rng)
		base_noise = rng.laplace(0.0, 1.0, groups)
		for name, bounds, gate in (("full", full_u, full_gate), ("query", query_u, query_gate)):
			totals = clipped_group_sums(gids, filtered, qualifies, bounds, groups)
			output = np.where(gate, totals + base_noise * bounds / args.eps_value, 0.0)
			methods[name]["errors"].append(score(output, truth))
			methods[name]["gates"].append(np.mean(gate))
			methods[name]["bounds"].extend(bounds[gate].tolist())

		for sampled_lanes in args.sampled_lanes:
			selected = lane_mask(lanes, trial % 64, sampled_lanes)
			weight = 64.0 / sampled_lanes
			sampled_counts = histogram(
				gids[selected], full_levels[selected], np.full(selected.sum(), weight),
				groups, args.bins)
			u, gate, _ = private_histogram_selection(
				sampled_counts, weight, args.eps_bounds, args.delta, args.factor, rng)
			totals = clipped_group_sums(gids, filtered, qualifies, u, groups)
			output = np.where(gate, totals + base_noise * u / args.eps_value, 0.0)
			methods[f"sample{sampled_lanes}"]["errors"].append(score(output, truth))
			methods[f"sample{sampled_lanes}"]["gates"].append(np.mean(gate))
			methods[f"sample{sampled_lanes}"]["bounds"].extend(u[gate].tolist())
	return {name: {metric: float(np.mean(values)) if values else 0.0
	               for metric, values in measurements.items()}
	        for name, measurements in methods.items()}


def run_synthetic(con, args):
	assert_dual_channel_sql(con)
	print("SQL invariants: exact filtered answer, complete sampled PUs, and filter-independent bound channel: PASS")
	print(f"\nsampling-only histogram error and support-{args.support:g} bound disagreement")
	header = (f"{'distribution':<12}{'groups':>8}{'lanes':>8}{'hist MdAE':>12}{'hist p95':>11}"
	          f"{'bound match':>13}{'under':>9}{'over':>9}")
	print(header)
	print("-" * len(header))
	for distribution, value_column in (("bounded", "bounded_value"), ("skewed", "skewed_value")):
		for groups in args.groups:
			gids, unfiltered, _, _, lanes = load_per_pu(con, groups, value_column, FILTERS[0][1])
			for sampled_lanes, median, p95, match, under, over in sampling_statistics(
			        gids, unfiltered, lanes, groups, args):
				print(f"{distribution:<12}{groups:>8}{sampled_lanes:>8}"
				      f"{100*median:>11.2f}%{100*p95:>10.2f}%{100*match:>12.2f}%"
				      f"{100*under:>8.2f}%{100*over:>8.2f}%")

	print("\nend-to-end relative L1 error; eps_bounds + eps_value = "
	      f"{args.eps_bounds}+{args.eps_value}")
	header = (f"{'distribution/filter':<28}{'groups':>8}{'active':>9}{'full':>10}{'query':>10}" +
	          "".join(f"{str(k) + '/64':>10}" for k in args.sampled_lanes))
	print(header)
	print("-" * len(header))
	for distribution, value_column in (("bounded", "bounded_value"), ("skewed", "skewed_value")):
		for filter_name, filter_sql in FILTERS:
			for groups in args.groups:
				gids, unfiltered, filtered, qualifies, lanes = load_per_pu(
					con, groups, value_column, filter_sql)
				utility = private_utility(gids, unfiltered, filtered, qualifies, lanes, groups, args)
				label = f"{distribution}/{filter_name}"
				print(f"{label:<28}{groups:>8}{qualifies.sum():>9,}"
				      f"{100*utility['full']['errors']:>9.2f}%{100*utility['query']['errors']:>9.2f}%" +
				      "".join(f"{100*utility[f'sample{k}']['errors']:>9.2f}%"
				                for k in args.sampled_lanes),
				      flush=True)


def run_tpch(args):
	database = Path(args.tpch)
	if not database.exists():
		print(f"\nTPC-H cross-check skipped: {database} does not exist")
		return
	import duckdb
	con = duckdb.connect(str(database), read_only=True, config={"threads": 2})
	channel_delta = con.execute("""WITH marked AS (
		SELECT c.c_custkey pu, c.c_nationkey gid, o.o_totalprice contribution,
		       o.o_orderdate>=DATE '1998-01-01' qualifies,
		       hash(c.c_custkey*1140071481932319849::UBIGINT)%64=0 sampled
		FROM customer c JOIN orders o ON o.o_custkey=c.c_custkey
	), candidates AS (
		SELECT * FROM marked WHERE qualifies OR sampled
	), dual AS (
		SELECT pu, gid,
		       SUM(contribution) FILTER (WHERE qualifies) filtered_contribution,
		       SUM(contribution) FILTER (WHERE sampled) sampled_unfiltered_contribution
		FROM candidates GROUP BY pu, gid
	), expected_answer AS (
		SELECT c.c_custkey pu, c.c_nationkey gid, SUM(o.o_totalprice) filtered_contribution
		FROM customer c JOIN orders o ON o.o_custkey=c.c_custkey
		WHERE o.o_orderdate>=DATE '1998-01-01' GROUP BY c.c_custkey, c.c_nationkey
	), expected_sample AS (
		SELECT c.c_custkey pu, c.c_nationkey gid, SUM(o.o_totalprice) sampled_unfiltered_contribution
		FROM customer c JOIN orders o ON o.o_custkey=c.c_custkey
		WHERE hash(c.c_custkey*1140071481932319849::UBIGINT)%64=0
		GROUP BY c.c_custkey, c.c_nationkey
	), differences AS (
		(SELECT pu, gid, filtered_contribution FROM dual WHERE filtered_contribution IS NOT NULL
		 EXCEPT ALL SELECT * FROM expected_answer)
		UNION ALL
		(SELECT * FROM expected_answer EXCEPT ALL
		 SELECT pu, gid, filtered_contribution FROM dual WHERE filtered_contribution IS NOT NULL)
		UNION ALL
		(SELECT pu, gid, sampled_unfiltered_contribution FROM dual
		 WHERE sampled_unfiltered_contribution IS NOT NULL EXCEPT ALL SELECT * FROM expected_sample)
		UNION ALL
		(SELECT * FROM expected_sample EXCEPT ALL
		 SELECT pu, gid, sampled_unfiltered_contribution FROM dual
		 WHERE sampled_unfiltered_contribution IS NOT NULL)
	)
	SELECT COUNT(*) FROM differences""").fetchone()[0]
	if channel_delta != 0:
		raise AssertionError("TPC-H dual channels must equal direct filtered and sampled queries")
	rows = con.execute("""SELECT c.c_custkey pu, c.c_nationkey gid,
		SUM(o.o_totalprice) unfiltered_contribution,
		COALESCE(SUM(o.o_totalprice) FILTER (WHERE o.o_orderdate>=DATE '1998-01-01'), 0.0)
			filtered_contribution,
		COUNT(*) FILTER (WHERE o.o_orderdate>=DATE '1998-01-01')>0 qualifies,
		hash(c.c_custkey*1140071481932319849::UBIGINT)%64 lane
	FROM customer c JOIN orders o ON o.o_custkey=c.c_custkey
	GROUP BY c.c_custkey, c.c_nationkey ORDER BY c.c_custkey""").fetchnumpy()
	gids = rows["gid"].astype(np.int64)
	unfiltered = rows["unfiltered_contribution"].astype(np.float64)
	filtered = rows["filtered_contribution"].astype(np.float64)
	qualifies = rows["qualifies"].astype(bool)
	lanes = rows["lane"].astype(np.int64)
	print("\nTPC-H SQL channel equivalence: PASS")
	print("TPC-H SF1: customer lifetime order total bounds; row filter o_orderdate>=1998-01-01")
	for sampled_lanes, median, p95, match, under, over in sampling_statistics(
	        gids, unfiltered, lanes, 25, args):
		print(f"  {sampled_lanes}/64 lanes: histogram MdAE={100*median:.2f}%, p95={100*p95:.2f}%, "
		      f"bound match={100*match:.2f}%, under={100*under:.2f}%, over={100*over:.2f}%")
	utility = private_utility(gids, unfiltered, filtered, qualifies, lanes, 25, args)
	print("  private relative L1 / groups with a bound: " + ", ".join(
		f"{name}={100*value['errors']:.2f}%/{100*value['gates']:.1f}%"
		for name, value in utility.items()))
	print("  mean selected bound: " + ", ".join(
		f"{name}={value['bounds']:,.0f}" for name, value in utility.items()))


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--pus", type=int, default=65536)
	parser.add_argument("--groups", type=int, nargs="+", default=[1, 12, 120])
	parser.add_argument("--sampled-lanes", type=int, nargs="+", default=[1, 2, 4, 8])
	parser.add_argument("--trials", type=int, default=30)
	parser.add_argument("--support", type=float, default=40.0)
	parser.add_argument("--eps-bounds", type=float, default=0.3)
	parser.add_argument("--eps-value", type=float, default=0.7)
	parser.add_argument("--delta", type=float, default=1e-6)
	parser.add_argument("--factor", type=float, default=4.0)
	parser.add_argument("--bins", type=int, default=16)
	parser.add_argument("--seed", type=int, default=20260824)
	parser.add_argument("--tpch", default="/tmp/privacy_filterless_tpch_sf1_v2.db")
	args = parser.parse_args()
	if args.pus <= 0 or args.trials <= 0 or args.bins <= 0 or args.support <= 0:
		raise ValueError("pus, trials, bins, and support must be positive")
	if any(k <= 0 or k > 64 for k in args.sampled_lanes):
		raise ValueError("sampled lanes must be between 1 and 64")
	if any(k & (k - 1) for k in args.sampled_lanes):
		raise ValueError("sampled lanes must be powers of two")
	if abs(args.eps_bounds + args.eps_value - 1.0) > 1e-12:
		raise ValueError("eps-bounds + eps-value must equal one")

	import duckdb
	con = duckdb.connect(config={"threads": 2})
	con.execute("SET enable_progress_bar=false")
	con.execute(f"""CREATE TEMP TABLE complete_pu_facts AS
		SELECT pu, row_id,
		       100 + hash(pu*8191+row_id*23+7)%900 bounded_value,
		       (100 + hash(pu*8191+row_id*23+7)%900) *
		       CASE WHEN hash(pu*131071+47)%1000=0 THEN 10000
		            WHEN hash(pu*131071+47)%100<5 THEN 100 ELSE 1 END skewed_value,
		       100 + hash(pu*8191+row_id*23+7)%900 base_value
		FROM range({args.pus}) p(pu), range(8) r(row_id)
		WHERE row_id < 1 + hash(pu*524287+29)%8""")
	run_synthetic(con, args)
	run_tpch(args)


if __name__ == "__main__":
	main()
