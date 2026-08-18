"""Mechanism-level PoC for automatic cross-group contribution bounds.

This isolates four future-work questions without changing the SQL compiler:

  1. Does a public candidate domain break the original high-quantile selector?
  2. Do a fixed geometric lattice and a direct rank score avoid tail multiplicity?
  3. How much does separating the existence bound C_e from the value bound C_v buy?
  4. How much headroom is available from jointly choosing bounds and the budget split?

The joint loss-aware choices are selected on training simulations and evaluated on independent
simulations. They are explicitly ORACLES: their score reads the private truth and is not a DP
selector. The experiment measures whether deriving a private bounded-sensitivity score is worth
pursuing; it does not claim to have derived one.

The released aggregate is privacy-ID COUNT over the real (PU, group) incidence relation. This
keeps the experiment independent of value-bound estimation and isolates cross-group bounding.

    python3 attacks/automatic_cu_future.py
"""

import argparse
import itertools
import math

import numpy as np

from em_cu import load
from fineness_sweep import DELTA, GROUPINGS, tau


def geometric_candidates(public_k, ratio=1.6):
    """A fixed public lattice; it never examines the observed k_u values."""
    if public_k < 1:
        raise ValueError("public_k must be positive")
    candidates = [1]
    while candidates[-1] < public_k:
        next_value = max(candidates[-1] + 1, int(math.ceil(candidates[-1] * ratio)))
        candidates.append(min(public_k, next_value))
    return np.asarray(candidates, dtype=np.int64)


def hybrid_candidates(public_k, dense_prefix=128, ratio=1.6):
    """Dense where useful bounds are usually small, geometric in the public upper tail."""
    prefix_end = min(public_k, dense_prefix)
    candidates = list(range(1, prefix_end + 1))
    while candidates[-1] < public_k:
        next_value = max(candidates[-1] + 1, int(math.ceil(candidates[-1] * ratio)))
        candidates.append(min(public_k, next_value))
    return np.asarray(candidates, dtype=np.int64)


def rank_scores(k_u, candidates, p):
    """Rank-distance score using exact N internally; sensitivity is max(p, 1-p)."""
    counts = np.searchsorted(np.sort(k_u), candidates, side="right")
    return -np.abs(counts - p * len(k_u))


def em_probabilities(scores, epsilon, sensitivity):
    log_weights = epsilon * (scores - np.max(scores)) / (2.0 * sensitivity)
    weights = np.exp(log_weights)
    return weights / np.sum(weights)


def direct_rank_select(k_u, candidates, p, epsilon, rng):
    sensitivity = max(p, 1.0 - p)
    probabilities = em_probabilities(
        rank_scores(k_u, candidates, p), epsilon, sensitivity
    )
    return int(rng.choice(candidates, p=probabilities))


def pipelinedp_proxy_select(
    k_u,
    candidates,
    public_k,
    public_partitions,
    aggregation_epsilon,
    selection_epsilon,
    rng,
):
    """PipelineDP's COUNT bias/noise objective, reproduced as a comparison selector."""
    probabilities = pipelinedp_proxy_probabilities(
        k_u,
        candidates,
        public_k,
        public_partitions,
        aggregation_epsilon,
        selection_epsilon,
    )
    return int(rng.choice(candidates, p=probabilities))


def pipelinedp_proxy_probabilities(
    k_u,
    candidates,
    public_k,
    public_partitions,
    aggregation_epsilon,
    selection_epsilon,
):
    clipped = np.minimum(k_u, public_k)
    noise = public_partitions * np.sqrt(2.0) * candidates / aggregation_epsilon
    dropped = np.asarray(
        [np.maximum(clipped - candidate, 0).sum() for candidate in candidates]
    )
    scores = -0.5 * (noise + dropped)
    return em_probabilities(scores, selection_epsilon, public_k)


class CountSimulation:
    """Held-out simulation for privacy-ID COUNT with separate C_e and C_v."""

    def __init__(self, pi, gi, candidates, train_trials, eval_trials, seed):
        self.pi = pi
        self.gi = gi
        self.candidates = candidates
        self.groups = int(gi.max()) + 1
        self.units = int(pi.max()) + 1
        self.truth = np.bincount(gi, minlength=self.groups).astype(float)
        self.trials = train_trials + eval_trials
        self.train = np.arange(train_trials)
        self.evaluate = np.arange(train_trials, self.trials)

        counts = np.bincount(pi, minlength=self.units)
        starts = np.concatenate([[0], np.cumsum(counts)])
        rng = np.random.default_rng(seed)
        self.votes = {}
        for trial in range(self.trials):
            order = np.lexsort((rng.random(len(pi)), pi))
            rank = np.empty(len(pi), dtype=np.int64)
            rank[order] = np.arange(len(pi)) - starts[pi[order]]
            for candidate in candidates:
                keep = rank < candidate
                self.votes[(trial, int(candidate))] = np.bincount(
                    gi[keep], minlength=self.groups
                ).astype(float)
        self.existence_noise = rng.laplace(0, 1, (self.trials, self.groups))
        self.value_noise = rng.laplace(0, 1, (self.trials, self.groups))

    def loss(self, ce, cv, epsilon_eta, trials, epsilon=1.0):
        epsilon_value = epsilon - epsilon_eta
        threshold = tau(epsilon_eta, DELTA, ce)
        errors = []
        for trial in trials:
            vote = self.votes[(int(trial), int(ce))]
            value = self.votes[(int(trial), int(cv))]
            released = (
                vote + self.existence_noise[trial] * ce / epsilon_eta >= threshold
            ) & (vote > 0)
            out = np.where(
                released, value + self.value_noise[trial] * cv / epsilon_value, 0.0
            )
            errors.append(np.abs(out - self.truth).sum() / self.truth.sum())
        return float(np.mean(errors))

    def choose(self, separate, epsilon_grid):
        best = None
        for ce, cv, epsilon_eta in itertools.product(
            self.candidates, self.candidates if separate else [None], epsilon_grid
        ):
            cv = int(ce) if cv is None else int(cv)
            loss = self.loss(int(ce), cv, epsilon_eta, self.train)
            if best is None or loss < best[0]:
                best = (loss, int(ce), cv, epsilon_eta)
        return best


def selector_summary(k_u, observed_max, public_k, p, trials, seed):
    dense = np.arange(1, public_k + 1)
    lattice = geometric_candidates(public_k)
    hybrid = hybrid_candidates(public_k)
    rng = np.random.default_rng(seed)
    sorted_k = np.sort(k_u)

    def draw_original(candidates):
        counts = np.searchsorted(sorted_k, candidates, side="right")
        selected = []
        for _ in range(trials):
            n_tilde = len(k_u) + rng.laplace(0, 1.0 / 0.002)
            target = p * n_tilde + (p / 0.002) * np.log(1.0 / 0.02)
            probabilities = em_probabilities(-np.abs(counts - target), 0.008, 1.0)
            selected.append(rng.choice(candidates, p=probabilities))
        return np.asarray(selected)

    def draw_direct(candidates):
        scores = -np.abs(
            np.searchsorted(sorted_k, candidates, side="right") - p * len(k_u)
        )
        probabilities = em_probabilities(scores, 0.008, max(p, 1.0 - p))
        return rng.choice(candidates, size=trials, p=probabilities)

    selectors = {
        "original+dense": draw_original(dense),
        "original+lattice": draw_original(lattice),
        "direct+dense": draw_direct(dense),
        "direct+lattice": draw_direct(lattice),
        "direct+hybrid": draw_direct(hybrid),
    }

    rows = []
    for name, selected in selectors.items():
        rows.append(
            (
                name,
                int(np.median(selected)),
                int(np.quantile(selected, 0.1)),
                int(np.quantile(selected, 0.9)),
                float(np.mean(selected > observed_max)),
            )
        )
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    parser.add_argument("--grouping", default="month|nation")
    parser.add_argument("--filter", default="c_acctbal>=8000")
    parser.add_argument("--p", type=float, default=0.99)
    parser.add_argument("--selector-trials", type=int, default=2000)
    parser.add_argument("--train-trials", type=int, default=8)
    parser.add_argument("--eval-trials", type=int, default=32)
    args = parser.parse_args()

    import duckdb

    connection = duckdb.connect(config={"threads": 2})
    connection.execute("SET enable_progress_bar=false")
    connection.execute(f"ATTACH '{args.db}' AS tpch (READ_ONLY)")
    cells = load(connection, GROUPINGS[args.grouping][0], args.filter)
    public_k = GROUPINGS[args.grouping][1]
    observed_max = int(cells.k_u.max())

    print(
        f"{args.grouping}, PUs={cells.P:,}, groups={cells.K:,}, observed max={observed_max}, "
        f"public K={public_k}, p={args.p}"
    )
    print("\nA. PUBLIC-DOMAIN ROBUSTNESS")
    print(f"{'selector':<20}{'median':>9}{'p10':>8}{'p90':>8}{'Pr[>max]':>12}")
    for name, median, p10, p90, over in selector_summary(
        cells.k_u, observed_max, public_k, args.p, args.selector_trials, 123
    ):
        print(f"{name:<20}{median:>9}{p10:>8}{p90:>8}{over:>11.1%}")

    # Privacy-ID COUNT: one value per observed (PU, group) cell.
    candidates = geometric_candidates(public_k)
    simulation = CountSimulation(
        cells.pi, cells.gi, candidates, args.train_trials, args.eval_trials, 321
    )
    epsilon_grid = (0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7)
    shared = simulation.choose(False, epsilon_grid)
    separate = simulation.choose(True, epsilon_grid)

    rng = np.random.default_rng(456)
    rank_bound = direct_rank_select(cells.k_u, candidates, 0.95, 0.01, rng)
    pipeline_probabilities = pipelinedp_proxy_probabilities(
        cells.k_u, candidates, public_k, public_k, 0.6, 0.01
    )
    pipeline_draws = rng.choice(
        candidates, size=args.selector_trials, p=pipeline_probabilities
    )
    pipeline_bound = int(np.median(pipeline_draws))
    baselines = [
        ("rank p95, shared", rank_bound, rank_bound, 0.4),
        ("PipelineDP proxy", pipeline_bound, pipeline_bound, 0.4),
        ("loss oracle, shared", shared[1], shared[2], shared[3]),
        ("loss oracle, separate", separate[1], separate[2], separate[3]),
    ]

    print("\nB. HELD-OUT COUNT UTILITY")
    print("Loss oracles tune on independent simulations and are not private selectors.")
    print(
        f"{'strategy':<24}{'C_e':>7}{'C_v':>7}{'eps_eta':>10}{'train':>10}{'held-out':>11}"
    )
    for name, ce, cv, epsilon_eta in baselines:
        train_loss = simulation.loss(ce, cv, epsilon_eta, simulation.train)
        eval_loss = simulation.loss(ce, cv, epsilon_eta, simulation.evaluate)
        print(
            f"{name:<24}{ce:>7}{cv:>7}{epsilon_eta:>10.2f}{100*train_loss:>9.2f}%"
            f"{100*eval_loss:>10.2f}%"
        )


if __name__ == "__main__":
    main()
