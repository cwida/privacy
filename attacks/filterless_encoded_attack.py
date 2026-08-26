#!/usr/bin/env python3
"""Knife-edge attacks on fixed-sample filterless bound selection.

Changing a predicate does not change the histogram: a target PU contributes its complete
unfiltered value in both filter worlds if it belongs to the fixed sample, and in neither world
otherwise. Adding or removing a sampled PU can still change the raw data-dependent bound.
"""

import argparse

import numpy as np


def attack_accuracy(without, with_target):
    center = float(np.median(without))
    features = ((without, with_target),
                (np.abs(without - center), np.abs(with_target - center)))
    best_advantage = 0.0
    for negative, positive in features:
        cuts = np.quantile(np.concatenate([negative, positive]), np.linspace(0.001, 0.999, 1000))
        for cut in cuts:
            best_advantage = max(
                best_advantage,
                float((positive >= cut).mean() - (negative >= cut).mean()),
            )
    return 0.5 + 0.5 * best_advantage


def centered_scale_accuracy(y0, y1, true0, true1):
    return attack_accuracy(y0 - true0, y1 - true1)


def release(bounds, base_answer, target_active, target, epsilon, rng):
    truth = np.minimum(base_answer, bounds)
    if target_active:
        truth += np.minimum(target, bounds)
    return truth + rng.laplace(0.0, bounds / epsilon), truth


def bounds_for_world(base_support, target_support, support, low, high, trials):
    observed = np.full(trials, base_support + target_support, dtype=np.float64)
    return np.where(observed >= support, high, low)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--trials", type=int, default=100000)
    parser.add_argument("--support", type=float, default=40.0)
    parser.add_argument("--epsilon-value", type=float, default=0.7)
    parser.add_argument("--target", type=float, default=100_000_000.0)
    args = parser.parse_args()
    rng = np.random.default_rng(20260825)
    low = 128.0
    high = 134_217_728.0
    base_answer = 10_000.0
    print("fixed-sample tail-bin knife edges; histogram is not noised")
    print("scale diagnostic centers each world at its clipped answer")
    header = (f"{'p / target lane':<24}{'filter attack':>15}{'update attack':>15}"
              f"{'filter bounds':>28}{'update bounds':>28}")
    print(header)
    print("-" * len(header))

    for p in (0, 2, 4, 6):
        weight = float(1 << p)
        lane_cases = ("sampled",) if p == 0 else ("sampled", "unsampled")
        for lane in lane_cases:
            target_support = weight if lane == "sampled" else 0.0

            # Same database, different filter: the target's histogram support is unchanged.
            filter_base = args.support - target_support if lane == "sampled" else args.support - 1.0
            filter_u0 = bounds_for_world(
                filter_base, target_support, args.support, low, high, args.trials)
            filter_u1 = bounds_for_world(
                filter_base, target_support, args.support, low, high, args.trials)
            filter_y0, filter_true0 = release(
                filter_u0, base_answer, False, args.target, args.epsilon_value, rng)
            filter_y1, filter_true1 = release(
                filter_u1, base_answer, True, args.target, args.epsilon_value, rng)
            filter_accuracy = centered_scale_accuracy(filter_y0, filter_y1, filter_true0, filter_true1)

            # Neighboring databases: an added sampled target moves support from T-w to T.
            update_base = args.support - weight
            update_u0 = bounds_for_world(update_base, 0.0, args.support, low, high, args.trials)
            update_u1 = bounds_for_world(update_base, target_support, args.support, low, high, args.trials)
            update_y0, update_true0 = release(
                update_u0, base_answer, False, args.target, args.epsilon_value, rng)
            update_y1, update_true1 = release(
                update_u1, base_answer, True, args.target, args.epsilon_value, rng)
            update_accuracy = centered_scale_accuracy(update_y0, update_y1, update_true0, update_true1)

            filter_bounds = f"{np.median(filter_u0):,.0f}->{np.median(filter_u1):,.0f}"
            update_bounds = f"{np.median(update_u0):,.0f}->{np.median(update_u1):,.0f}"
            print(f"p={p} {lane:<16}{100*filter_accuracy:>14.2f}%{100*update_accuracy:>14.2f}%"
                  f"{filter_bounds:>28}{update_bounds:>28}")


if __name__ == "__main__":
    main()
