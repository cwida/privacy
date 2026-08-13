"""Below which k_u should partition selection use LAPLACE votes? The adversarial case, derived.

For one group, write eff(C) = sum_{u in g} min(C,k_u)/k_u for the expected vote count when votes
are truncated to C. Both arms share it; only the threshold differs:

    Laplace   releases iff  eff(C) >= tau_L(C) = 1 + C*ln(1/2 delta_eta)/eps_eta   [C=1 optimal]
    Gaussian  releases iff  eff(C) >= thr_G(C) = 1 + sqrt(C)*sqrt(2 ln(1.25/(d/2)))*z / eps_eta

Both thresholds are 1 + (const)/eps_eta, so the eps a group NEEDS is proportional to that const
divided by eff. Define the eps-COST RATIO

    f  =  min_C [ (thr_G(C)-1) / eff(C) ]  /  min_C [ (tau_L(C)-1) / eff(C) ]

= the factor by which Gaussian's eps must exceed Laplace's to release the same group. f < 1 means
Gaussian wins, f > 1 means Gaussian LOSES by that factor. f depends only on the k_u profile of the
group's members and on (eps-free) constants, so it is a pure function of data shape and delta.

For a homogeneous group (every member has k_u = k) this collapses to f = rho/sqrt(k) with
rho = (thr_G(1)-1)/(tau_L(1)-1) ~ 2, i.e. the crossover is k ~ rho^2 ~ 4 and the best possible
Gaussian advantage is sqrt(k)/rho. For a heavy-tailed k_u it does not collapse, and Gaussian can
lose even when max k_u is in the thousands.

    python3 attacks/vote_crossover.py
"""

import argparse

import numpy as np
from scipy.stats import norm

CE = np.array([1, 2, 3, 4, 5, 6, 8, 10, 13, 16, 21, 27, 34, 44, 55, 72, 89, 116, 144, 200,
               300, 500, 1000, 2126])


def lap_const(delta_eta, ce):
    """tau_L(ce) - 1, in units of 1/eps_eta."""
    inner = 2.0 - 2.0 * (1.0 - delta_eta) ** (1.0 / ce)
    return np.inf if inner <= 0 else -ce * np.log(inner)


def gauss_const(delta, ce):
    """thr_G(ce) - 1, in units of 1/eps_eta. delta is split: half mechanism, half threshold."""
    return (np.sqrt(ce) * np.sqrt(2.0 * np.log(1.25 / (delta / 2)))
            * norm.ppf(1.0 - (delta / 2) / max(ce, 1)))


def eps_cost(eff, delta):
    """(gaussian eps needed, laplace eps needed) up to the shared 1/eff factor; and best C each."""
    g = [(gauss_const(delta, c) / max(e, 1e-12), c) for c, e in zip(CE, eff)]
    l = [(lap_const(delta, c) / max(e, 1e-12), c) for c, e in zip(CE, eff)]
    return min(g), min(l)


def homogeneous(k, delta):
    """A group whose members all have k_u = k: eff(C) = n_g * min(C,k)/k (n_g cancels in f)."""
    return np.minimum(CE, k) / k


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--deltas", default="1e-3,1e-6,1e-9")
    a = ap.parse_args()
    deltas = [float(x) for x in a.deltas.split(",")]

    print("A. HOMOGENEOUS k_u -- the crossover. f = eps Gaussian needs / eps Laplace needs.")
    print("   f < 1: Gaussian wins.  f > 1: Gaussian loses by that factor. eps cancels entirely.\n")
    ks = [1, 2, 3, 4, 5, 6, 8, 12, 20, 50, 130]
    print(f"{'delta':<10}{'rho':>6}{'crossover k':>13}   " +
          "".join(f"{('k=%d' % k):>8}" for k in ks))
    print("-" * (29 + 8 * len(ks)))
    for d in deltas:
        rho = gauss_const(d, 1) / lap_const(d, 1)
        fs = []
        for k in ks:
            (gv, _), (lv, _) = eps_cost(homogeneous(k, d), d)
            fs.append(gv / lv)
        kx = next((k for k in range(1, 400)
                   if eps_cost(homogeneous(k, d), d)[0][0] < eps_cost(homogeneous(k, d), d)[1][0]),
                  None)
        print(f"{d:<10.0e}{rho:>6.2f}{kx:>13}   " + "".join(f"{f:>8.2f}" for f in fs))
    print("\n   f = rho/sqrt(k) exactly, so crossover k = ceil(rho^2) and the BEST Gaussian can")
    print("   ever do is sqrt(k)/rho -- a 4x eps advantage needs k_u ~ 64 members-wide groups.")

    print("\nB. HEAVY-TAILED k_u -- max k_u is the wrong statistic. Mixture: a fraction p of PUs")
    print("   have k_u = K_hi, the rest have k_u = 1 (the StackOverflow/ClickBench shape).\n")
    d = 1e-6
    print(f"{'p (spread PUs)':<16}" + "".join(f"{('K_hi=%d' % K):>11}" for K in
                                              [10, 50, 200, 1000, 2126]))
    print("-" * (16 + 11 * 5))
    for p in [1.0, 0.5, 0.2, 0.1, 0.05, 0.01]:
        row = []
        for K in [10, 50, 200, 1000, 2126]:
            eff = p * np.minimum(CE, K) / K + (1 - p) * 1.0      # per PU, averaged
            (gv, gc), (lv, lc) = eps_cost(eff, d)
            row.append(f"{gv/lv:>7.2f}(C{gc})")
        print(f"{p:<16.2f}" + "".join(f"{c:>11}" for c in row))
    print("\n   Read: with only 5% of PUs spreading over 1000 groups, Gaussian costs 1.6x the eps")
    print("   of Laplace no matter how large the tail is -- the 95% with k_u=1 already vote at")
    print("   full weight under Laplace truncation, so there is nothing for sqrt(C) to buy.")

    print("\nC. HARMONIC MEAN is the right statistic. Same mean k_u, different shapes, delta=1e-6:")
    print(f"   {'shape':<44}{'mean k_u':>9}{'harmonic':>10}{'f':>7}")
    shapes = {
        "all PUs k_u=20": np.full(1000, 20.0),
        "half k_u=39, half k_u=1": np.r_[np.full(500, 39.0), np.full(500, 1.0)],
        "5% k_u=381, 95% k_u=1": np.r_[np.full(50, 381.0), np.full(950, 1.0)],
        "1% k_u=1901, 99% k_u=1": np.r_[np.full(10, 1901.0), np.full(990, 1.0)],
    }
    for name, ku in shapes.items():
        eff = np.array([np.mean(np.minimum(c, ku) / ku) for c in CE])
        (gv, _), (lv, _) = eps_cost(eff, d)
        print(f"   {name:<44}{ku.mean():>9.1f}{len(ku)/np.sum(1/ku):>10.1f}{gv/lv:>7.2f}")
    print("   -> all four have mean k_u = 20; only the first is above the crossover.")

    print("\nD. MONTE-CARLO CONFIRMATION of the homogeneous crossover (with real noise).")
    print("   For each k, the smallest n_g at which each arm releases a group >=50% of the time.\n")
    rng = np.random.default_rng(0)
    eps_eta, dd, T = 0.4, 1e-6, 20000
    print(f"   {'k_u':>5}{'n_g* Laplace':>15}{'n_g* Gaussian':>15}{'ratio':>8}{'predicted':>11}")
    for k in [1, 2, 3, 4, 5, 6, 8, 20, 50]:
        out = []
        for arm in ("lap", "gau"):
            best = np.inf
            for c in CE[CE <= max(k, 1)]:
                if arm == "lap":
                    thr, sc = 1 + lap_const(dd, c) / eps_eta, c / eps_eta
                else:
                    sig = np.sqrt(c) * np.sqrt(2 * np.log(1.25 / (dd / 2))) / eps_eta
                    thr = 1 + gauss_const(dd, c) / eps_eta
                lo, hi = 1.0, 1e7
                for _ in range(40):                       # bisect on n_g
                    mid = np.sqrt(lo * hi)
                    v = mid * min(c, k) / k
                    n = (rng.laplace(0, sc, T) if arm == "lap" else rng.normal(0, sig, T))
                    if np.mean(v + n >= thr) >= 0.5:
                        hi = mid
                    else:
                        lo = mid
                best = min(best, hi)
            out.append(best)
        (gv, _), (lv, _) = eps_cost(homogeneous(k, dd), dd)
        print(f"   {k:>5}{out[0]:>15,.0f}{out[1]:>15,.0f}{out[1]/out[0]:>8.2f}"
              f"{gv/lv:>11.2f}")
    print("\n   ratio > 1 => Gaussian needs a bigger group to be released => Gaussian is worse.")


if __name__ == "__main__":
    main()
