#!/usr/bin/env python3
"""Unified privacy attacks for the fixed-sample filterless mechanism.

This harness reproduces the two knife-edge experiments documented in
``docs/dp/filterless_encoded_pu.md``:

1. A filter-change diagnostic on one fixed database. This is not a neighboring-
   database DP test; it checks whether changing only the predicate changes the
   bound/noise-scale channel.
2. An add/remove attack in which the added privacy unit is in the fixed sample.
   This is a DP adjacency test. It compares the removed raw-histogram prototype
   with the current mechanism's Laplace-noised histogram support.

The simulation is deliberately reduced to the two magnitude bins at the support
knife edge. It audits that channel; it is not a proof of the full SQL mechanism.
"""

import argparse

import numpy as np


LOW_BOUND = 128.0
HIGH_BOUND = 134_217_728.0
BASE_FILTERED_ANSWER = 10_000.0
SEED = 20260828


def held_out_threshold_accuracy(world_0, world_1):
	"""Fit a one-dimensional threshold attack and score it on held-out draws."""
	split_0 = len(world_0) // 2
	split_1 = len(world_1) // 2
	train_0, test_0 = world_0[:split_0], world_0[split_0:]
	train_1, test_1 = world_1[:split_1], world_1[split_1:]
	center = float(np.median(np.concatenate((train_0, train_1))))

	best = None
	for transform in (lambda x: x, lambda x: np.abs(x - center)):
		x0 = transform(train_0)
		x1 = transform(train_1)
		cuts = np.quantile(np.concatenate((x0, x1)), np.linspace(0.001, 0.999, 1000))
		cdf_0 = np.searchsorted(np.sort(x0), cuts, side="left") / len(x0)
		cdf_1 = np.searchsorted(np.sort(x1), cuts, side="left") / len(x1)
		accuracy_above = 0.5 * (cdf_0 + 1.0 - cdf_1)
		accuracy_below = 1.0 - accuracy_above
		for accuracies, predicts_world_1_above in ((accuracy_below, False), (accuracy_above, True)):
			index = int(np.argmax(accuracies))
			accuracy = float(accuracies[index])
			if best is None or accuracy > best[0]:
				best = (accuracy, transform, float(cuts[index]), predicts_world_1_above)

	_, transform, cut, predicts_world_1_above = best
	x0 = transform(test_0)
	x1 = transform(test_1)
	if predicts_world_1_above:
		return 0.5 * (np.mean(x0 < cut) + np.mean(x1 >= cut))
	return 0.5 * (np.mean(x0 >= cut) + np.mean(x1 < cut))


def select_bounds(base_support, target_support, support, histogram_scale, trials, rng):
	"""Select HIGH when the knife-edge bin reaches support, otherwise LOW."""
	observed = np.full(trials, base_support + target_support, dtype=np.float64)
	if histogram_scale > 0.0:
		observed += rng.laplace(0.0, histogram_scale, size=trials)
	return np.where(observed >= support, HIGH_BOUND, LOW_BOUND)


def release(bounds, target_present, target_value, value_epsilon, max_groups, rng):
	"""Release a clipped per-PU SUM with Laplace sensitivity C_u * bound."""
	truth = np.full(len(bounds), BASE_FILTERED_ANSWER, dtype=np.float64)
	if target_present:
		truth += np.minimum(target_value, bounds)
	scale = bounds * max_groups / value_epsilon
	return truth + rng.laplace(0.0, scale), truth


def scale_attack_accuracy(release_0, release_1, truth_0, truth_1):
	"""Oracle-centred diagnostic that isolates the observable noise-scale channel."""
	return held_out_threshold_accuracy(release_0 - truth_0, release_1 - truth_1)


def run_filter_change(args, sample_bits, rng):
	"""Same database, two predicates: histogram inputs remain identical."""
	weight = float(1 << sample_bits)
	base_support = args.support - weight
	histogram_scale = weight * args.max_groups / args.bounds_epsilon

	bound_0 = select_bounds(base_support, weight, args.support, histogram_scale, args.trials, rng)
	bound_1 = select_bounds(base_support, weight, args.support, histogram_scale, args.trials, rng)
	release_0, truth_0 = release(
		bound_0, False, args.target, args.value_epsilon, args.max_groups, rng
	)
	release_1, truth_1 = release(
		bound_1, True, args.target, args.value_epsilon, args.max_groups, rng
	)
	return scale_attack_accuracy(release_0, release_1, truth_0, truth_1)


def run_update(args, sample_bits, private_histogram, rng):
	"""Add/remove one sampled PU at the high-bin support boundary."""
	weight = float(1 << sample_bits)
	base_support = args.support - weight
	histogram_scale = 0.0
	if private_histogram:
		histogram_scale = weight * args.max_groups / args.bounds_epsilon

	bound_0 = select_bounds(base_support, 0.0, args.support, histogram_scale, args.trials, rng)
	bound_1 = select_bounds(base_support, weight, args.support, histogram_scale, args.trials, rng)
	release_0, truth_0 = release(
		bound_0, False, args.target, args.value_epsilon, args.max_groups, rng
	)
	release_1, truth_1 = release(
		bound_1, True, args.target, args.value_epsilon, args.max_groups, rng
	)
	return (
		held_out_threshold_accuracy(release_0, release_1),
		scale_attack_accuracy(release_0, release_1, truth_0, truth_1),
		float(np.mean(bound_0 == HIGH_BOUND)),
		float(np.mean(bound_1 == HIGH_BOUND)),
	)


def parse_args():
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--trials", type=int, default=100_000)
	parser.add_argument("--epsilon", type=float, default=1.0)
	parser.add_argument("--bounds-fraction", type=float, default=0.3)
	parser.add_argument("--support", type=float, default=128.0)
	parser.add_argument("--max-groups", type=float, default=1.0)
	parser.add_argument("--target", type=float, default=100_000_000.0)
	parser.add_argument("--sample-bits", type=int, nargs="+", default=(0, 2, 4, 6))
	args = parser.parse_args()
	if args.trials < 2:
		parser.error("--trials must be at least 2")
	if args.epsilon <= 0.0:
		parser.error("--epsilon must be positive")
	if not 0.0 < args.bounds_fraction < 1.0:
		parser.error("--bounds-fraction must be strictly between 0 and 1")
	if args.support <= 0.0 or args.max_groups <= 0.0:
		parser.error("--support and --max-groups must be positive")
	if any(bits < 0 or bits > 63 for bits in args.sample_bits):
		parser.error("--sample-bits values must be between 0 and 63")
	if any(float(1 << bits) > args.support for bits in args.sample_bits):
		parser.error("--support must be at least 2^p for every --sample-bits value")
	args.bounds_epsilon = args.epsilon * args.bounds_fraction
	args.value_epsilon = args.epsilon - args.bounds_epsilon
	return args


def main():
	args = parse_args()
	rng = np.random.default_rng(SEED)
	ceiling = np.exp(args.epsilon) / (1.0 + np.exp(args.epsilon))

	print("Fixed-sample filterless knife-edge attacks")
	print(f"trials={args.trials:,}, epsilon={args.epsilon:g}, "
	      f"bounds/value split={args.bounds_fraction:g}/{1.0 - args.bounds_fraction:g}")
	print("Filter change is a channel diagnostic, not a neighboring-database DP claim.")
	print(f"For the add/remove test, the pure-DP equal-prior ceiling is {100 * ceiling:.2f}%.")

	header = (
		f"{'p':>3}  {'filter scale':>13}  {'unsafe update':>14}  {'private update':>14}  "
		f"{'private scale':>14}  {'unsafe Pr[high]':>18}  {'private Pr[high]':>18}"
	)
	print("\n" + header)
	print("-" * len(header))
	for sample_bits in args.sample_bits:
		filter_accuracy = run_filter_change(args, sample_bits, rng)
		unsafe = run_update(args, sample_bits, False, rng)
		private = run_update(args, sample_bits, True, rng)
		unsafe_bounds = f"{100 * unsafe[2]:.1f}%->{100 * unsafe[3]:.1f}%"
		private_bounds = f"{100 * private[2]:.1f}%->{100 * private[3]:.1f}%"
		print(
			f"{sample_bits:>3}  {100 * filter_accuracy:>12.2f}%  {100 * unsafe[0]:>13.2f}%  "
			f"{100 * private[0]:>13.2f}%  {100 * private[1]:>13.2f}%  "
			f"{unsafe_bounds:>18}  {private_bounds:>18}"
		)

	print("\nInterpretation:")
	print("- filter scale: predicate-only scale classification; chance is 50%")
	print("- unsafe update: removed raw-histogram control; it should be nearly perfectly distinguishable")
	print("- private update: current noised-histogram model; compare only this column with the DP ceiling")
	print("- private scale: oracle-centred diagnostic, reported separately from the observable raw release")
	print("- Pr[high]: probability of selecting the 134,217,728 bound in absent->present worlds")


if __name__ == "__main__":
	main()
