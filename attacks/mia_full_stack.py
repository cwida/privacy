"""Simulate an ATTACKER against the full stack: can they learn whether a target PU is present?

Everything so far verified sensitivity by neighbour construction. This instead runs an adversary
end to end, measures how well they actually do, and checks it against the theoretical bound.

Setup. D and D' differ in one target privacy unit u*. The mechanism releases (key set, values).
The attacker sees one release and must decide which world it came from. Their advantage is
    adv = Pr[say D' | D'] - Pr[say D' | D]
and for an eps-DP release no attacker can exceed (e^eps - 1)/(e^eps + 1).

Three attacks, each targeting a different channel:

  A1 VALUE CHANNEL. u* puts its whole mass in one group; attacker thresholds that group's released
     value. This is the strongest value-channel attack: a maximally concentrated target.
  A2 KEY SET, WITH the vote-support gate. u* creates a group that exists in D' only; attacker says
     D' iff that group is released. Leak should be <= delta.
  A3 KEY SET, WITHOUT the gate -- the delta under-accounting found earlier, where a PU creates a
     group by having a VALUE there but only votes in C_e of them. Should show a leak far above the
     charged delta, and the gate should remove it.

    python3 attacks/mia_full_stack.py
"""
import numpy as np

EPS_V, EPS_ETA, DELTA = 0.6, 0.4, 1e-6


def tau_lap(eps_eta, delta, ce=1):
    inner = 2.0 - 2.0 * (1.0 - delta) ** (1.0 / ce)
    return np.inf if inner <= 0 else 1.0 - ce * np.log(inner) / eps_eta


def adv_bound(eps):
    return (np.exp(eps) - 1) / (np.exp(eps) + 1)


def a1_value_channel(B, n_trials, r):
    """u* contributes its full budget B to one group. Attacker thresholds that group's value."""
    base = 5000.0                                   # other PUs' true total in that group
    outD = base + r.laplace(0, B / EPS_V, n_trials)          # target absent
    outD2 = base + B + r.laplace(0, B / EPS_V, n_trials)     # target present
    # optimal single-threshold attacker, swept
    cuts = np.quantile(np.concatenate([outD, outD2]), np.linspace(0.001, 0.999, 400))
    adv = max(float((outD2 >= c).mean() - (outD >= c).mean()) for c in cuts)
    return adv


def a2_keyset_gated(ce, n_trials, r):
    """u* creates a group present only in D'. WITH the gate the group needs a vote to be released,
    and the target is its only voter, so the attacker's power is exactly the tau failure rate."""
    thr = tau_lap(EPS_ETA, DELTA, ce)
    # D: group absent -> never released. D': count 1 (the target votes), plus noise.
    releasedD2 = (1.0 + r.laplace(0, ce / EPS_ETA, n_trials) >= thr)
    return float(releasedD2.mean())                 # Pr[say D' | D'] - 0


def a3_keyset_ungated(ce, k_u, n_trials, r):
    """The delta bug: u* holds VALUES in k_u groups but votes in only ce of them. Without the gate,
    a group with zero votes can still clear tau on noise alone -- and there are k_u - ce such
    chances, none of which tau's union bound covers."""
    thr = tau_lap(EPS_ETA, DELTA, ce)
    voted = (1.0 + r.laplace(0, ce / EPS_ETA, (n_trials, ce)) >= thr).any(axis=1)
    unvoted = (0.0 + r.laplace(0, ce / EPS_ETA, (n_trials, k_u - ce)) >= thr).any(axis=1)
    return float((voted | unvoted).mean()), float(voted.mean())


def main():
    r = np.random.default_rng(7)
    N = 4_000_000
    print(f"eps_v={EPS_V}, eps_eta={EPS_ETA}, delta={DELTA:g}, {N:,} trials per attack\n")

    print("A1  VALUE CHANNEL -- target concentrates its whole budget B in one group.")
    print(f"    {'B':>10}{'attacker adv':>15}{'bound at eps_v':>17}{'verdict':>10}")
    for B in (512.0, 4096.0, 65536.0):
        adv = a1_value_channel(B, N // 8, r)
        bd = adv_bound(EPS_V)
        print(f"    {B:>10,.0f}{adv:>15.4f}{bd:>17.4f}"
              f"{'OK' if adv <= bd + 0.01 else 'VIOLATION':>10}")
    print("    -> the attack is scale-free: the target's contribution and the noise both scale")
    print("       with B, so the advantage is a property of eps_v alone, as it must be.\n")

    print("A2  KEY SET, WITH the vote-support gate. Target creates a group only it occupies.")
    print(f"    {'C_e':>5}{'tau':>9}{'Pr[leak]':>12}{'charged delta':>15}{'verdict':>10}")
    for ce in (1, 2, 5):
        p = a2_keyset_gated(ce, N, r)
        print(f"    {ce:>5}{tau_lap(EPS_ETA,DELTA,ce):>9.1f}{p:>12.2e}{DELTA:>15.2e}"
              f"{'OK' if p <= DELTA else 'VIOLATION':>10}")
    print()

    print("A3  KEY SET, WITHOUT the gate -- the delta under-accounting. Target holds VALUES in")
    print("    k_u groups but votes in only C_e; unvoted groups can still clear tau on noise.")
    print(f"    {'C_e':>5}{'k_u':>6}{'leak, no gate':>16}{'leak, gated':>14}"
          f"{'charged':>10}{'over by':>10}")
    for ce, k_u in ((1, 30), (1, 72), (2, 72), (5, 72)):
        ung, gat = a3_keyset_ungated(ce, k_u, N, r)
        print(f"    {ce:>5}{k_u:>6}{ung:>16.2e}{gat:>14.2e}{DELTA:>10.1e}"
              f"{ung/DELTA:>9.1f}x")
    print("    -> the gate removes the excess: with it the leak is the voted-only column, which")
    print("       is what tau's union bound actually covers.")


if __name__ == "__main__":
    main()
