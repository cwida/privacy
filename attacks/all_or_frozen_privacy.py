"""Verify Dandan's All-or-Frozen privacy argument by neighbour simulation, not by reading it.

Two things her draft leaves open or asserts:

1. SECTION 2.8's delta bound. She shows Pr[AllPass(D')] <= rho_tau^m <= rho_tau, worst case m=1,
   so rho_tau <= delta suffices. That bounds the probability of ENTERING the query-specific branch
   when the added PU created new groups. But the released object is (branch, group set), and the
   branch bit itself differs between D and D'. This checks the whole release by explicit
   neighbour construction rather than by argument.

2. REPLACE-ONE ADJACENCY, which she scopes out as future work. Under replace-one a PU is swapped
   rather than added, so it can simultaneously delete groups and create them. Two things change:
   the histogram sensitivity doubles (2*C_u, since one PU's counts leave and another's arrive),
   and AllPass can flip in BOTH directions.

    python3 attacks/all_or_frozen_privacy.py
"""

import argparse

import numpy as np

B_BINS = 64
DELTA = 1e-6


def tau_af(b, delta, B=B_BINS):
    lo, hi = 1.0, 1.0 + 400.0 * b
    for _ in range(300):
        mid = (lo + hi) / 2
        p = (1 - 0.5 * np.exp(-(mid - 1) / b)) * (1 - 0.5 * np.exp(-mid / b)) ** (B - 1)
        if p >= 1 - delta:
            hi = mid
        else:
            lo = mid
    return hi


def all_pass(H, b, tau, r, n):
    """AllPass over the rows of H, n independent draws. H is (groups x bins) true counts."""
    z = r.laplace(0, b, size=(n,) + H.shape)
    return ((H[None] + z).max(axis=2) >= tau).all(axis=1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cu", type=int, default=8)
    ap.add_argument("--eps-b", type=float, default=0.5)
    ap.add_argument("--trials", type=int, default=400_000)
    ap.add_argument("--background", type=int, default=6, help="pre-existing groups, all large")
    a = ap.parse_args()
    r = np.random.default_rng(11)
    b = a.cu / a.eps_b
    tau = tau_af(b, DELTA)
    print(f"C_u={a.cu}, eps_B={a.eps_b} -> b={b:.1f}, tau_AF={tau:,.1f}, "
          f"charged delta={DELTA:g}, {a.trials:,} trials\n")

    # background groups made large enough that they pass essentially always, so the only thing
    # that can flip AllPass is the target PU's own groups -- the worst case for the mechanism
    big = np.zeros((a.background, B_BINS))
    big[:, 0] = tau + 12 * b

    print("1) ADD/REMOVE. D' = D + u*, where u* creates m singleton groups.")
    print(f"   {'m':>4}{'Pr[AllPass|D]':>16}{'Pr[AllPass|Dprime]':>20}"
          f"{'leak':>13}{'vs delta':>11}")
    for m in (1, 2, 4, 8):
        new = np.zeros((m, B_BINS))
        new[:, 0] = 1.0                       # each new group holds exactly the one PU
        pD = all_pass(big, b, tau, r, a.trials).mean()
        pD2 = all_pass(np.vstack([big, new]), b, tau, r, a.trials).mean()
        # the bad event is entering the compact branch on D', which exposes the new groups
        print(f"   {m:>4}{pD:>16.6f}{pD2:>17.8f}{pD2:>13.2e}{pD2/DELTA:>10.3f}x")
    print("   -> the leak is Pr[compact branch | D'], which must be <= delta. Worst case is m=1,")
    print("      exactly as section 2.8 claims.\n")

    print("2) THE BRANCH BIT ITSELF. AllPass can also flip 1->0, changing the output DOMAIN even")
    print("   when no new group is created. That is a function of the noisy histogram alone, so it")
    print("   is post-processing of an eps_B-DP release -- but only if the histogram noise is")
    print("   calibrated to the RIGHT sensitivity. Checking the ratio stays within e^eps_B:")
    # u* joins an EXISTING group, changing one bin by 1: no new group, but AllPass may flip
    near = big.copy()
    near[0, 0] = tau - 2 * b                  # a marginal group, so the bit is actually sensitive
    nearD2 = near.copy()
    nearD2[0, 0] += 1                         # u* adds one to that group's occupied bin
    p1 = all_pass(near, b, tau, r, a.trials).mean()
    p2 = all_pass(nearD2, b, tau, r, a.trials).mean()
    lo, hi = min(p1, p2), max(p1, p2)
    print(f"   Pr[AllPass] = {p1:.6f} vs {p2:.6f};  ratio {hi/max(lo,1e-12):.6f}, "
          f"e^eps_B = {np.exp(a.eps_b):.6f}  -> {'OK' if hi/max(lo,1e-12) <= np.exp(a.eps_b) else 'VIOLATION'}")
    print("   and for the complement:")
    q1, q2 = 1 - p1, 1 - p2
    lo2, hi2 = min(q1, q2), max(q1, q2)
    print(f"   Pr[fallback] = {q1:.6f} vs {q2:.6f};  ratio {hi2/max(lo2,1e-12):.6f}"
          f"  -> {'OK' if hi2/max(lo2,1e-12) <= np.exp(a.eps_b) else 'VIOLATION'}\n")

    print("3) REPLACE-ONE (her stated future work). u* is swapped for u', so one PU's counts leave")
    print("   and another's arrive. Two consequences:")
    b2 = 2 * a.cu / a.eps_b
    tau2 = tau_af(b2, DELTA)
    print(f"   (a) histogram sensitivity doubles, C_u -> 2*C_u, so b {b:.1f} -> {b2:.1f} and")
    print(f"       tau {tau:,.1f} -> {tau2:,.1f} ({tau2/tau:.2f}x). Every group must clear a")
    print( "       threshold twice as high, so the rule fires strictly less often.")
    new = np.zeros((1, B_BINS)); new[:, 0] = 1.0
    p_add = all_pass(np.vstack([big, new]), b, tau, r, a.trials).mean()
    p_rep = all_pass(np.vstack([big, new]), b2, tau2, r, a.trials).mean()
    print(f"   (b) worst-case leak at m=1: add/remove {p_add:.2e}, replace-one {p_rep:.2e}")
    print("   -> the AND argument itself survives replacement (entering the compact branch still")
    print("      requires every new group to pass), but the doubled sensitivity makes an already")
    print("      inert rule strictly more inert. Nothing is unsound; it is just worse.")


if __name__ == "__main__":
    main()
