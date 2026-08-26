#!/usr/bin/env python3
"""Filtered-membership attack on filterless versus Google-style implicit DP bounds.

Both worlds contain the same billionaire. The worlds differ in whether that customer satisfies the
query predicate. Every trial releases one noisy SUM. The attacker sees only that scalar and uses a
mean-shift test; an oracle-centered diagnostic separately isolates the noise-scale channel.
"""

import argparse

import numpy as np


def dp_hist_bounds(counts, epsilon, threshold, factor, trials, rng):
	"""Release a factor-spaced bound by post-processing one full Laplace histogram."""
	noisy = counts + rng.laplace(0.0, 1.0 / epsilon, size=(trials, len(counts)))
	passing = noisy >= threshold
	has_bound = passing.any(axis=1)
	level = len(counts) - 1 - np.argmax(passing[:, ::-1], axis=1)
	level = np.where(has_bound, level, 0)
	return factor ** (level + 1)


def magnitude_counts(values, factor, n_bins):
	levels = np.clip(np.floor(np.log(np.maximum(values, 1.0)) / np.log(factor)).astype(np.int64),
	                 0, n_bins - 1)
	return np.bincount(levels, minlength=n_bins).astype(np.float64)


def clipped_sums(values, bounds):
	out = np.empty(len(bounds), dtype=np.float64)
	for bound in np.unique(bounds):
		out[bounds == bound] = np.minimum(values, bound).sum()
	return out


def attack_accuracy(without, with_target):
	"""Best balanced accuracy of mean-shift and two-sided variance threshold attacks."""
	center = float(np.median(without))
	features = ((without, with_target),
	            (np.abs(without - center), np.abs(with_target - center)))
	best_adv = 0.0
	for neg, pos in features:
		cuts = np.quantile(np.concatenate([neg, pos]), np.linspace(0.001, 0.999, 1000))
		for cut in cuts:
			best_adv = max(best_adv, float((pos >= cut).mean() - (neg >= cut).mean()))
	return 0.5 + 0.5 * best_adv


def scale_accuracy(without, with_target, true_without, true_with):
	"""Oracle diagnostic after centering each hypothesis at its clipped query answer."""
	return attack_accuracy(without - true_without, with_target - true_with)


def release(values, bounds, epsilon, rng):
	return clipped_sums(values, bounds) + rng.laplace(0.0, bounds / epsilon)


def bit_attack_accuracy(without, with_target):
	"""Optimal balanced accuracy when the observable is one release/suppress bit."""
	p0 = float(np.mean(without))
	p1 = float(np.mean(with_target))
	return 0.5 + 0.5 * abs(p1 - p0)


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--trials", type=int, default=200000)
	parser.add_argument("--regular", type=int, default=10000)
	parser.add_argument("--billionaire", type=float, default=100_000_000.0)
	parser.add_argument("--epsilon", type=float, default=1.0)
	parser.add_argument("--epsilon-bounds", type=float, default=0.3)
	parser.add_argument("--support", type=float, default=40.0)
	args = parser.parse_args()

	rng = np.random.default_rng(20260824)
	regular = rng.integers(50, 151, size=args.regular).astype(np.float64)
	with_billionaire = np.r_[regular, args.billionaire]
	eps_value = args.epsilon - args.epsilon_bounds
	if eps_value <= 0:
		raise ValueError("epsilon must exceed epsilon-bounds")

	# Filterless DP selects reusable bounds from the same unfiltered population in both worlds.
	# Independent releases are sampled because each world is an independent run of the mechanism.
	f_population = magnitude_counts(with_billionaire, 4.0, 30)
	uf0 = dp_hist_bounds(f_population, args.epsilon_bounds, args.support, 4.0, args.trials, rng)
	uf1 = dp_hist_bounds(f_population, args.epsilon_bounds, args.support, 4.0, args.trials, rng)
	yf0 = release(regular, uf0, eps_value, rng)
	yf1 = release(with_billionaire, uf1, eps_value, rng)

	# Google-style implicit bounds: base-2 ApproxBounds threshold over the query contributions.
	n_bins = 46
	delta_bounds = 1e-6
	threshold = -np.log(2.0 * (1.0 - (1.0 - delta_bounds) ** (1.0 / (2.0 * n_bins))))
	threshold /= args.epsilon_bounds
	g0 = magnitude_counts(regular, 2.0, n_bins)
	g1 = magnitude_counts(with_billionaire, 2.0, n_bins)
	ug0 = dp_hist_bounds(g0, args.epsilon_bounds, threshold, 2.0, args.trials, rng)
	ug1 = dp_hist_bounds(g1, args.epsilon_bounds, threshold, 2.0, args.trials, rng)
	yg0 = release(regular, ug0, eps_value, rng)
	yg1 = release(with_billionaire, ug1, eps_value, rng)

	# Unsafe control: exact observed maximum determines both clipping and noise scale.
	uu0 = np.full(args.trials, regular.max())
	uu1 = np.full(args.trials, args.billionaire)
	yu0 = release(regular, uu0, args.epsilon, rng)
	yu1 = release(with_billionaire, uu1, args.epsilon, rng)

	print(f"one filtered SUM release; billionaire fails/passes the predicate; epsilon={args.epsilon}")
	print(f"regular PUs={args.regular:,}, billionaire contribution={args.billionaire:,.0f}")

	print(f"{'mechanism':<28}{'median U fails':>18}{'median U passes':>19}"
	      f"{'raw output':>15}{'scale diagnostic':>19}")
	print("-" * 97)
	for name, u0, u1, y0, y1 in (
	    ("filterless DP", uf0, uf1, yf0, yf1),
	    ("Google implicit DP", ug0, ug1, yg0, yg1),
	    ("unsafe exact maximum", uu0, uu1, yu0, yu1),
	):
		true0 = clipped_sums(regular, u0)
		true1 = clipped_sums(with_billionaire, u1)
		print(f"{name:<28}{np.median(u0):>18,.0f}{np.median(u1):>19,.0f}"
		      f"{100 * attack_accuracy(y0, y1):>17.2f}%"
		      f"{100 * scale_accuracy(y0, y1, true0, true1):>18.2f}%")
	print(f"epsilon-DP equal-prior accuracy ceiling (pure DP): "
	      f"{100 * np.exp(args.epsilon) / (1 + np.exp(args.epsilon)):.2f}%")

	# Group-existence knife edge. The target changes one joint (group, magnitude-bin) count from
	# support-1 to support. A DP query-local exponential-bin gate is safe; an exact count or an
	# unnoised HLL estimate used as a gate is not. A frozen group universe is identical across the
	# two filter-membership worlds because it was selected from the same unfiltered population.
	n0 = args.support - 1.0
	n1 = args.support
	dp_gate0 = n0 + rng.laplace(0.0, 1.0 / args.epsilon, args.trials) >= args.support
	dp_gate1 = n1 + rng.laplace(0.0, 1.0 / args.epsilon, args.trials) >= args.support
	unsafe_gate0 = np.zeros(args.trials, dtype=bool)
	unsafe_gate1 = np.ones(args.trials, dtype=bool)
	frozen_population_count = args.support + 100.0
	frozen_gate0 = (frozen_population_count + rng.laplace(
		0.0, 1.0 / args.epsilon, args.trials) >= args.support)
	frozen_gate1 = (frozen_population_count + rng.laplace(
		0.0, 1.0 / args.epsilon, args.trials) >= args.support)

	print("\ngroup-existence knife edge: target changes one bin count from support-1 to support")
	print(f"{'gate':<34}{'attack accuracy':>18}")
	print("-" * 52)
	for name, z0, z1 in (
	    ("DP query-local exponential bin", dp_gate0, dp_gate1),
	    ("DP frozen filterless group set", frozen_gate0, frozen_gate1),
	    ("unnoised exact/HLL support gate", unsafe_gate0, unsafe_gate1),
	):
		print(f"{name:<34}{100 * bit_attack_accuracy(z0, z1):>17.2f}%")


if __name__ == "__main__":
	main()
