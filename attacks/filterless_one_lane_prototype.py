#!/usr/bin/env python3
"""Prototype one-lane filterless metadata with conservative standard-DP calibration.

Scope is intentionally narrow: one nonnegative contribution and one group per PU, hence C_u=1.
Metadata is a joint (group, factor-4 value-bin) histogram. Qualifying PUs have weight 1; a
nonqualifying PU in the selected 1/64 AS lane has weight 64. The histogram's add/remove sensitivity
is therefore 64. Metadata built with the 20% predicate is reused by 100%, 20%, and 2% queries.

The query-local comparison uses the same joint histogram with unit weights and sensitivity 1.
Both arms spend eps_gate + eps_value = 1 per distinct query. Filterless additionally spends eps0
once for its reusable metadata; no amortisation assumption is used in the utility numbers.
"""

import argparse

import numpy as np

from filterless_dp_attack import attack_accuracy


def bin_levels(values, factor, n_bins):
	return np.clip(np.floor(np.log(np.maximum(values, 1.0)) / np.log(factor)).astype(np.int64),
	               0, n_bins - 1)


def histogram(gids, levels, weights, groups, n_bins):
	out = np.zeros((groups, n_bins), dtype=np.float64)
	np.add.at(out, (gids, levels), weights)
	return out


def private_histogram_selection(counts, sensitivity, epsilon, delta, factor, rng):
	"""Return per-group selected bounds and gate bits from one noised joint histogram.

	The threshold makes the probability that one maximum-weight PU creates any candidate cell at
	most delta by a union bound. This is conservative and explicit; it is not a tuned utility knob.
	"""
	if epsilon <= 0 or sensitivity <= 0 or not 0 < delta < 1:
		raise ValueError("epsilon and sensitivity must be positive; delta must be in (0,1)")
	scale = sensitivity / epsilon
	candidates = counts.size
	threshold = sensitivity + scale * np.log(candidates / (2.0 * delta))
	noisy = counts + rng.laplace(0.0, scale, size=counts.shape)
	passing = noisy >= threshold
	gate = passing.any(axis=1)
	level = counts.shape[1] - 1 - np.argmax(passing[:, ::-1], axis=1)
	level = np.where(gate, level, 0)
	return factor ** (level + 1), gate, threshold


def clipped_group_sums(gids, values, active, bounds, groups):
	weights = np.minimum(values[active], bounds[gids[active]])
	return np.bincount(gids[active], weights=weights, minlength=groups)


def score(output, truth):
	denominator = np.abs(truth).sum()
	return 0.0 if denominator == 0 else float(np.abs(output - truth).sum() / denominator)


def load_sql_workload(con, groups, value_column, selectivity):
	rows = con.execute(f"""SELECT pu, pu%{groups} gid, {value_column} contribution,
		(hash(pu*65537+17)%10000)<{int(selectivity * 10000)} active,
		hash(pu*1140071481932319849::UBIGINT)%64 lane
	FROM filterless_proto ORDER BY pu""").fetchnumpy()
	return (rows["gid"].astype(np.int64), rows["contribution"].astype(np.float64),
	        rows["active"].astype(bool), rows["lane"].astype(np.int64))


def run_utility(con, args):
	rng = np.random.default_rng(args.seed)
	print(f"one-lane prototype: PUs={args.pus:,}, trials={args.trials}, eps0={args.eps0}, "
	      f"query eps={args.eps_gate}+{args.eps_value}, delta={args.delta:g}")
	print("metadata uses factor-4 bins, active weight 1, sampled-inactive weight 64, sensitivity 64")
	print("metadata is built with the 20% filter and reused by every selectivity below\n")
	hdr = (f"{'distribution / query':<35}{'groups':>8}{'active':>9}{'meta groups':>13}"
	       f"{'Google':>10}{'filterless':>12}{'gain':>8}")
	print(hdr)
	print("-" * len(hdr))

	for distribution, value_column in (("bounded", "bounded"), ("spread", "spread")):
		for groups in args.groups:
			gids, values, build_active, lanes = load_sql_workload(con, groups, value_column, 0.20)
			levels = bin_levels(values, args.factor, args.bins)
			metadata = []
			for trial in range(args.trials):
				selected_lane = trial % 64
				weights = np.where(build_active, 1.0, np.where(lanes == selected_lane, 64.0, 0.0))
				counts = histogram(gids, levels, weights, groups, args.bins)
				metadata.append(private_histogram_selection(
					counts, 64.0, args.eps0, args.delta, args.factor, rng)[:2])

			# Across all 64 lanes the inverse-probability estimator must recover the full
			# histogram exactly. This catches lane, weighting, and active/inactive mistakes.
			lane_sum = np.zeros((groups, args.bins), dtype=np.float64)
			for selected_lane in range(64):
				weights = np.where(build_active, 1.0,
				                   np.where(lanes == selected_lane, 64.0, 0.0))
				lane_sum += histogram(gids, levels, weights, groups, args.bins)
			lane_mean = lane_sum / 64.0
			full_counts = histogram(gids, levels, np.ones(len(gids)), groups, args.bins)
			if not np.array_equal(lane_mean, full_counts):
				raise AssertionError("mean across the 64 one-lane estimators must equal the full histogram")

			for selectivity in (1.0, 0.20, 0.02):
				_, _, active, _ = load_sql_workload(con, groups, value_column, selectivity)
				truth = np.bincount(gids[active], weights=values[active], minlength=groups)
				google_errors = []
				filterless_errors = []
				meta_group_counts = []
				for trial in range(args.trials):
					query_counts = histogram(gids[active], levels[active], np.ones(active.sum()),
					                         groups, args.bins)
					query_u, query_gate, _ = private_histogram_selection(
						query_counts, 1.0, args.eps_gate, args.delta, args.factor, rng)
					meta_u, meta_gate = metadata[trial]
					filterless_gate = meta_gate | query_gate
					filterless_u = np.where(meta_gate, meta_u, query_u)
					common_noise = rng.laplace(0.0, 1.0, size=groups)

					google_totals = clipped_group_sums(gids, values, active, query_u, groups)
					google_out = np.where(
						query_gate,
						google_totals + common_noise * query_u / args.eps_value,
						0.0)
					filterless_totals = clipped_group_sums(
						gids, values, active, filterless_u, groups)
					filterless_out = np.where(
						filterless_gate,
						filterless_totals + common_noise * filterless_u / args.eps_value,
						0.0)
					google_errors.append(score(google_out, truth))
					filterless_errors.append(score(filterless_out, truth))
					meta_group_counts.append(int(meta_gate.sum()))

				google = float(np.mean(google_errors))
				filterless = float(np.mean(filterless_errors))
				label = f"{distribution} / {100*selectivity:.0f}% filter"
				print(f"{label:<35}{groups:>8,}{int(active.sum()):>9,}"
				      f"{np.mean(meta_group_counts):>12.1f}{100*google:>9.2f}%"
				      f"{100*filterless:>11.2f}%{google/max(filterless,1e-30):>7.2f}x",
				      flush=True)


def run_attack(args):
	"""Worst-lane filter-membership attack on one metadata cell."""
	rng = np.random.default_rng(args.seed + 1)
	trials = max(args.attack_trials, 1000)
	background = 5000.0
	# The target is in the selected lane. Failing the filter has weight 64; passing has weight 1.
	unsafe_fail = background + 64.0 + rng.laplace(0.0, 1.0, size=trials)
	unsafe_pass = background + 1.0 + rng.laplace(0.0, 1.0, size=trials)
	dp_fail = background + 64.0 + rng.laplace(0.0, 64.0 / args.eps0, size=trials)
	dp_pass = background + 1.0 + rng.laplace(0.0, 64.0 / args.eps0, size=trials)
	ceiling = np.exp(args.eps0) / (1.0 + np.exp(args.eps0))
	print("\nworst-lane filter-membership attack on one metadata cell")
	# `attack_accuracy` searches for larger values under its second hypothesis. Pass the
	# lower-weight world first so this is the intended mean-shift attack, not a one-sided miss.
	print(f"  unsafe noise calibrated to 1:  {100*attack_accuracy(unsafe_pass, unsafe_fail):.2f}%")
	print(f"  DP noise calibrated to 64:    {100*attack_accuracy(dp_pass, dp_fail):.2f}%")
	print(f"  pure-eps equal-prior ceiling: {100*ceiling:.2f}%")


def run_filter_dependence(con, args):
	"""Change only WHERE while holding the AS lane and DP noise draw fixed."""
	groups = 4
	rows = con.execute("""SELECT pu, pu%4 gid, spread contribution,
		(hash(pu*65537+17)%10000)<2000 filter_a,
		(hash(pu*104729+101)%10000)<2000 filter_b,
		hash(pu*1140071481932319849::UBIGINT)%64 lane
	FROM filterless_proto ORDER BY pu""").fetchnumpy()
	gids = rows["gid"].astype(np.int64)
	values = rows["contribution"].astype(np.float64)
	filter_a = rows["filter_a"].astype(bool)
	filter_b = rows["filter_b"].astype(bool)
	lanes = rows["lane"].astype(np.int64)
	levels = bin_levels(values, args.factor, args.bins)
	full = histogram(gids, levels, np.ones(len(gids)), groups, args.bins)

	estimator_differences = []
	filter_a_errors = []
	filter_b_errors = []
	bound_group_differences = []
	for selected_lane in range(64):
		weights_a = np.where(filter_a, 1.0, np.where(lanes == selected_lane, 64.0, 0.0))
		weights_b = np.where(filter_b, 1.0, np.where(lanes == selected_lane, 64.0, 0.0))
		counts_a = histogram(gids, levels, weights_a, groups, args.bins)
		counts_b = histogram(gids, levels, weights_b, groups, args.bins)
		estimator_differences.append(score(counts_a, counts_b))
		filter_a_errors.append(score(counts_a, full))
		filter_b_errors.append(score(counts_b, full))

		# Identical seeds deliberately give both filters the same Laplace noise vector. Any
		# different private bound below is therefore caused by the sampled histogram alone.
		noise_seed = args.seed + 1000 + selected_lane
		bound_a, gate_a, _ = private_histogram_selection(
			counts_a, 64.0, args.eps0, args.delta, args.factor,
			np.random.default_rng(noise_seed))
		bound_b, gate_b, _ = private_histogram_selection(
			counts_b, 64.0, args.eps0, args.delta, args.factor,
			np.random.default_rng(noise_seed))
		bound_group_differences.append(np.mean((gate_a != gate_b) | (bound_a != bound_b)))

	def percentile(values, q):
		return 100.0 * float(np.percentile(values, q))

	print("\nfilter-dependence test: two different 20% predicates; table and lane assignment fixed")
	print(f"  qualifying PUs: filter A={filter_a.sum():,}, filter B={filter_b.sum():,}, "
	      f"overlap={(filter_a & filter_b).sum():,}")
	print("  DP histogram noise scale: 64/epsilon for both filters (identical)")
	print(f"  sampled histogram A-vs-B relative L1: median={percentile(estimator_differences, 50):.2f}%, "
	      f"p95={percentile(estimator_differences, 95):.2f}%")
	print(f"  sampling error vs full histogram: A median={percentile(filter_a_errors, 50):.2f}%, "
	      f"B median={percentile(filter_b_errors, 50):.2f}%")
	print(f"  groups choosing a different private bound with the same DP noise draw: "
	      f"median={percentile(bound_group_differences, 50):.2f}%, "
	      f"p95={percentile(bound_group_differences, 95):.2f}%")


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--pus", type=int, default=65536)
	parser.add_argument("--groups", type=int, nargs="+", default=[1, 4, 12, 120])
	parser.add_argument("--trials", type=int, default=30)
	parser.add_argument("--attack-trials", type=int, default=200000)
	parser.add_argument("--eps0", type=float, default=1.0)
	parser.add_argument("--eps-gate", type=float, default=0.3)
	parser.add_argument("--eps-value", type=float, default=0.7)
	parser.add_argument("--delta", type=float, default=1e-6)
	parser.add_argument("--factor", type=float, default=4.0)
	parser.add_argument("--bins", type=int, default=16)
	parser.add_argument("--seed", type=int, default=20260824)
	args = parser.parse_args()
	if args.pus <= 0 or args.trials <= 0 or args.bins <= 0:
		raise ValueError("pus, trials, and bins must be positive")
	if args.eps0 <= 0 or args.eps_gate <= 0 or args.eps_value <= 0:
		raise ValueError("epsilon values must be positive")
	if abs(args.eps_gate + args.eps_value - 1.0) > 1e-12:
		raise ValueError("eps-gate + eps-value must equal one")

	import duckdb
	con = duckdb.connect(config={"threads": 2})
	con.execute("SET enable_progress_bar=false")
	con.execute(f"""CREATE TEMP TABLE filterless_proto AS
		SELECT pu,
		       80.0 + (hash(pu*8191+23)%4000)/100.0 bounded,
		       pow(4.0, cast(hash(pu*131071+47)%10 AS INTEGER)) spread
		FROM range({args.pus}) p(pu)""")
	run_utility(con, args)
	run_attack(args)
	run_filter_dependence(con, args)


if __name__ == "__main__":
	main()
