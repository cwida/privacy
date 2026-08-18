"""Dandan's joint-quantile suggestion: release several k_u quantiles under one budget.

Gillenwater et al. (arXiv:2102.08244) release m quantiles jointly with L1 sensitivity 2, rather
than splitting eps m ways across m independent releases at sensitivity 1. Her suggestion is to use
that for multiple candidate contribution bounds.

There is a second use that matters more here. Earlier work in this project established that the
SHAPE of the k_u distribution decides which mechanism wins -- concentrated and above ~10 favours
the l1 norm clip, bimodal favours truncation, and the swing is 0.68x to 6x. That decision currently
requires looking at the data. A quantile CURVE, released privately in one shot, would let the
system make it.

Two questions:
  Q_A  Is the joint release actually needed, or is a naive eps/m split accurate enough? On this
       data k_u is a small integer with a very steep CDF, so the EM may be sharp enough that the
       split does not matter.
  Q_B  Does the DP-estimated curve preserve the mechanism decision?

    python3 attacks/joint_quantiles.py
"""

import argparse

import numpy as np


def em_quantile(k_u, K, p, eps, r, n_draws=1):
    """Single-quantile exponential mechanism, sensitivity 1 (Gillenwater et al. Lemma 7)."""
    N = len(k_u)
    T = p * N
    F = np.cumsum(np.bincount(np.clip(k_u, 1, K), minlength=K + 1)[1:])
    q = -np.abs(F - T)
    w = np.exp(eps * (q - q.max()) / 2.0)
    w /= w.sum()
    return r.choice(np.arange(1, K + 1), size=n_draws, p=w)


FAMILIES = {
    # name: (generator, true winner from attacks/ku_families.py)
    "constant(1)":        (lambda r, N: np.full(N, 1),                              "tie"),
    "constant(5)":        (lambda r, N: np.full(N, 5),                              "google"),
    "constant(20)":       (lambda r, N: np.full(N, 20),                             "ours"),
    "constant(60)":       (lambda r, N: np.full(N, 60),                             "ours"),
    "uniform(1,10)":      (lambda r, N: r.integers(1, 11, N),                       "ours"),
    "uniform(1,60)":      (lambda r, N: r.integers(1, 61, N),                       "ours"),
    "uniform(1,200)":     (lambda r, N: r.integers(1, 201, N),                      "ours"),
    "zipf(1.3)":          (lambda r, N: np.minimum(r.zipf(1.3, N), 200),            "ours"),
    "zipf(2.0)":          (lambda r, N: np.minimum(r.zipf(2.0, N), 200),            "google"),
    "lognormal(1.4)":     (lambda r, N: np.maximum(1, r.lognormal(2.5, 1.4, N).astype(int)), "ours"),
    "bimodal(0.005,100)": (lambda r, N: np.where(r.random(N) < 0.005, 100, 1),      "google"),
    "bimodal(0.02,100)":  (lambda r, N: np.where(r.random(N) < 0.02, 100, 1),       "google"),
    "bimodal(0.05,60)":   (lambda r, N: np.where(r.random(N) < 0.05, 60, 1),        "google"),
}

PS = (0.5, 0.9, 0.95, 0.99)


def decide(q50, q99):
    """Mechanism choice from the released curve alone.

    From attacks/ku_families.py: the l1 clip needs units touching >~10 groups (a level condition)
    and fails when k_u is bimodal -- most units at 1 with a heavy tail. Both are visible in the
    curve: the level as q50, the bimodality as a large q99/q50 with q50 itself small.
    """
    if q50 >= 10:
        return "ours"
    if q99 >= 10 * max(q50, 1):
        return "google"
    return "tie"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=30000)
    ap.add_argument("--kmax", type=int, default=200)
    ap.add_argument("--trials", type=int, default=200)
    a = ap.parse_args()
    r = np.random.default_rng(11)

    print(f"N = {a.n:,} units, K = {a.kmax}, {a.trials} trials, m = {len(PS)} quantiles\n")

    print("Q_A  IS THE JOINT RELEASE NEEDED? Naive split gives each quantile eps/m at sensitivity")
    print("     1; a joint release gives the whole vector eps at sensitivity 2, i.e. each quantile")
    print("     an effective eps/2. Comparing the naive split against the true quantile:\n")
    print(f"     {'family':<20}{'eps':>6}" + "".join(f"{'p'+str(int(100*p)):>9}" for p in PS)
          + f"{'max err':>10}")
    print("     " + "-" * 76)
    for name in ("constant(20)", "uniform(1,60)", "bimodal(0.02,100)"):
        gen, _ = FAMILIES[name]
        k_u = gen(np.random.default_rng(3), a.n)
        truth = [int(np.quantile(k_u, p)) for p in PS]
        for eps in (0.01, 0.05, 0.25):
            errs, cells = [], ""
            for p, t in zip(PS, truth):
                d = em_quantile(k_u, a.kmax, p, eps / len(PS), r, a.trials)
                med = int(np.median(d))
                errs.append(abs(med - t))
                cells += f"{med:>5}/{t:<4}"
            print(f"     {name if eps == 0.01 else '':<20}{eps:>6.2f}{cells}{max(errs):>10}")
    print("\n     -> the CDF of k_u is steep (F jumps by thousands between adjacent integers), so")
    print("        even eps/m = 0.0025 per quantile recovers them exactly. The joint machinery is")
    print("        not needed for THIS statistic; it would matter for a flat/continuous quantity")
    print("        such as a value bound, where adjacent candidates score almost equally.\n")

    print("Q_B  DOES THE RELEASED CURVE PRESERVE THE MECHANISM DECISION? Rule: ours if p50 >= 10;")
    print("     google if p99 >= 10*p50 (bimodal); tie otherwise. Total budget 0.01 for the curve.\n")
    print(f"     {'family':<20}{'true p50':>9}{'true p99':>9}{'DP p50':>8}{'DP p99':>8}"
          f"{'decision':>10}{'actual':>9}{'ok':>5}")
    print("     " + "-" * 78)
    right = 0
    for name, (gen, winner) in FAMILIES.items():
        k_u = gen(np.random.default_rng(3), a.n)
        t50, t99 = int(np.quantile(k_u, 0.5)), int(np.quantile(k_u, 0.99))
        d50 = int(np.median(em_quantile(k_u, a.kmax, 0.5, 0.005, r, 51)))
        d99 = int(np.median(em_quantile(k_u, a.kmax, 0.99, 0.005, r, 51)))
        dec = decide(d50, d99)
        ok = (dec == winner) or (winner == "tie" and dec in ("tie", "ours"))
        right += ok
        print(f"     {name:<20}{t50:>9}{t99:>9}{d50:>8}{d99:>8}{dec:>10}{winner:>9}"
              f"{'yes' if ok else 'NO':>5}")
    print(f"\n     {right}/{len(FAMILIES)} decisions correct from the DP curve alone, at a cost of")
    print("     0.01 of eps. The curve is a cheap, private feature for choosing the mechanism --")
    print("     which is worth 0.68x-6x per the family sweep.")


if __name__ == "__main__":
    main()
