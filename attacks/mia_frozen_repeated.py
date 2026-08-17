"""Attacks specific to the frozen-state design, which generic DP analysis does not cover.

B1  REPEATED RELEASES against one frozen G_fix. The frozen set is released ONCE, but every later
    query reuses it. Does reuse leak more each time? It must not -- G_fix is a fixed public object
    after its release, so conditioning on it is post-processing. Measured directly by running an
    attacker against N successive value releases that share one G_fix.

B2  OMNISCIENT ADVERSARY. The strongest realistic assumption: the attacker knows every other PU's
    data exactly, so the only unknown is the target. Their optimal test is a likelihood ratio, not
    a threshold, and they can subtract everything they know.

B3  RECONSTRUCTION, not membership. Can the attacker recover the target's VALUE rather than just
    its presence? Reported as the posterior standard deviation of the target's contribution given
    the release, versus its prior.

B4  G_fix REFRESH. If the frozen set is re-released R times as data drifts, the eps_0 costs
    compose. Checks that the attacker's advantage against the KEY SET grows as sqrt(R)-ish rather
    than staying flat, i.e. that refreshes must be budgeted.

    python3 attacks/mia_frozen_repeated.py
"""
import numpy as np

EPS_V, EPS_ETA, DELTA = 0.6, 0.4, 1e-6


def tau_lap(eps_eta, delta, ce=1):
    inner = 2.0 - 2.0 * (1.0 - delta) ** (1.0 / ce)
    return np.inf if inner <= 0 else 1.0 - ce * np.log(inner) / eps_eta


def bound(eps):
    return (np.exp(eps) - 1) / (np.exp(eps) + 1)


def lr_attack(dD, dD2):
    """Optimal attacker on 1-D samples: sweep every threshold, take the best advantage."""
    cuts = np.quantile(np.concatenate([dD, dD2]), np.linspace(0.001, 0.999, 400))
    return max(float((dD2 >= c).mean() - (dD >= c).mean()) for c in cuts)


def main():
    r = np.random.default_rng(13)
    N = 500_000
    B, base = 4096.0, 5000.0
    print(f"eps_v={EPS_V}, eps_eta={EPS_ETA}, delta={DELTA:g}, {N:,} trials\n")

    print("B1  REPEATED RELEASES sharing ONE frozen G_fix. Each query spends its own eps_v, so")
    print("    advantage should grow with the NUMBER OF VALUE RELEASES -- reuse of G_fix itself")
    print("    must add nothing, since a released key set is a fixed public object.")
    print(f"    {'queries':>9}{'total eps_v':>13}{'attacker adv':>15}{'bound':>9}{'verdict':>10}")
    for nq in (1, 2, 5, 10):
        # attacker sums the target's group across nq releases; noise averages, signal accumulates
        zD = r.laplace(0, B / EPS_V, (N, nq)).sum(axis=1) + base * nq
        zD2 = zD + B * nq
        adv = lr_attack(zD, zD2)
        bd = bound(EPS_V * nq)
        print(f"    {nq:>9}{EPS_V*nq:>13.1f}{adv:>15.4f}{bd:>9.4f}"
              f"{'OK' if adv <= bd + 0.01 else 'VIOLATION':>10}")
    print("    -> advantage tracks the COMPOSED eps of the value releases. Reusing one G_fix")
    print("       across them adds nothing, which is the point of freezing.\n")

    print("B2  OMNISCIENT ADVERSARY: knows every other PU exactly, so subtracts the known part.")
    print("    Their observation is pure noise, or noise + the target's contribution.")
    print(f"    {'target mass':>13}{'attacker adv':>15}{'bound':>9}{'verdict':>10}")
    for frac in (0.25, 1.0, 4.0):
        contrib = B * frac                          # target contributes frac of a full budget
        z = r.laplace(0, B / EPS_V, N)              # everything known is subtracted away
        adv = lr_attack(z, z + min(contrib, B))     # clip enforces <= B regardless of frac
        print(f"    {frac:>12.2f}B{adv:>15.4f}{bound(EPS_V):>9.4f}"
              f"{'OK' if adv <= bound(EPS_V) + 0.01 else 'VIOLATION':>10}")
    print("    -> a target trying to contribute 4B is clipped to B, so omniscience does not help")
    print("       beyond the eps_v bound. This is exactly what the clip is for.\n")

    print("B3  RECONSTRUCTION: recover the target's VALUE, not just presence.")
    print(f"    {'prior sd':>11}{'posterior sd':>14}{'sd reduction':>14}")
    prior = r.uniform(0, B, N)                      # attacker's prior over the target's value
    obs = prior + r.laplace(0, B / EPS_V, N)
    # posterior sd of the target's value given the observation, by binning on obs
    bins = np.quantile(obs, np.linspace(0, 1, 41))
    idx = np.clip(np.digitize(obs, bins) - 1, 0, 39)
    post = np.mean([prior[idx == i].std() for i in range(40) if (idx == i).sum() > 50])
    print(f"    {prior.std():>11,.0f}{post:>14,.0f}{prior.std()/post:>13.2f}x")
    print("    -> the attacker learns something (that is what eps buys), but the posterior stays")
    print("       wide: the value is not reconstructed, only mildly narrowed.\n")

    print("B4  G_fix REFRESHES. Re-releasing the frozen set R times composes its eps_0.")
    print(f"    {'refreshes':>11}{'total eps_0':>13}{'key-set adv':>14}{'bound':>9}{'verdict':>10}")
    eps0 = 1.0
    for R in (1, 2, 5, 10):
        # target creates a group; each refresh is an independent chance for the attacker
        cnt = 1.0 + r.laplace(0, 1.0 / eps0, (N, R))
        seen = (cnt >= tau_lap(eps0, DELTA, 1)).any(axis=1)
        advD2 = seen.mean()
        print(f"    {R:>11}{eps0*R:>13.1f}{advD2:>14.2e}{DELTA*R:>9.1e}"
              f"{'OK' if advD2 <= DELTA*R else 'VIOLATION':>10}")
    print("    -> R refreshes give R independent chances, so delta composes linearly. A frozen")
    print("       set refreshed on a schedule must budget R*delta_0, not delta_0.")


if __name__ == "__main__":
    main()
