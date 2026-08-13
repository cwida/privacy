"""Gaussian noise on the VOTE histogram, where the l2/l1 gap is real.

Gaussian lost badly on the value channel because the l1 clip makes Delta_2 = Delta_1 = B exactly
(a PU can put all its mass in one group). The vote channel is the opposite geometry: a PU that
votes 1 in each of its k_u groups has

    l1 sensitivity = k_u        but      l2 sensitivity = sqrt(k_u)

so Laplace is forced to truncate to C_e groups -- which divides a group's expected vote count by
k_u/C_e -- while Gaussian can let every PU vote in every group and pay only sqrt(k_u).

Why this matters: with Laplace the release condition for a group works out to

    n_g * C_e / k_u  >=  tau ~ C_e * ln(1/2 delta_eta) / eps_eta

and C_e CANCELS, leaving n_g / k_u >~ 30-45. That is the floor that makes week|nation and
day|region return nothing at eps=1 for every mechanism. It is a property of the l1 vote geometry,
not of the value clipping, so no amount of budget re-allocation moves it.

    python3 attacks/vote_gaussian.py
"""

import argparse
import itertools

import numpy as np
from scipy.stats import norm

from fineness_sweep import (DELTA, EPS, GROUPINGS, SPLITS, Cells, approx_bounds, tau, votes)


def gauss_sigma(l2_sens, eps, delta):
    """Analytic-ish Gaussian calibration (classical bound; valid for eps <= 1)."""
    return l2_sens * np.sqrt(2.0 * np.log(1.25 / delta)) / eps


def gauss_tau(sigma, delta_eta, cv):
    """A group present in only one of two neighbouring datasets has true vote count 1.
    Union bound over the <= cv groups one PU can create."""
    return 1.0 + sigma * norm.ppf(1.0 - delta_eta / max(cv, 1))


def full_votes(c, cv, rank):
    """Every PU votes 1 in each of its groups, truncated to cv. l2 sensitivity sqrt(min(cv,k_u))."""
    keep = rank < cv
    return np.bincount(c.gi[keep], minlength=c.K).astype(float)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--filter", default="c_acctbal>=8000")
    ap.add_argument("--groupings", default="month|nation,week|nation,day|region")
    ap.add_argument("--trials", type=int, default=3)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    print(f"SUM(l_extendedprice), PU=customer, WHERE {a.filter}, eps={EPS}, delta={DELTA:g}")
    print("value channel is the l1 clip in both arms; only the VOTE histogram changes.")
    print("Laplace: truncate to C_e, noise Lap(C_e/eps_eta), Wilson tau.")
    print("Gaussian: truncate to C_v, l2 sens sqrt(C_v), noise N(0,sigma^2), Gaussian tau.\n")
    for name in a.groupings.split(","):
        c = Cells(con, GROUPINGS[name][0], a.filter)
        mk = int(c.k_u.max())
        cus = sorted({1, 2, 5, 10, 19, 30, mk // 2 or 1, mk})
        r = np.random.default_rng(99)
        ranks = [c.rank_random(r) for _ in range(a.trials)]
        vt = {(t, cv): full_votes(c, cv, ranks[t]) for t in range(a.trials) for cv in cus}
        ebs = sorted({s[0] for s in SPLITS})
        Bs, tot = {}, {}
        for eb in ebs:
            for t in range(a.trials):
                B = approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
                Bs[(eb, t)] = B
                cl = np.clip(c.val, -B, B)
                n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
                tot[(eb, t)] = np.bincount(
                    c.gi, weights=cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi],
                    minlength=c.K)
        print(f"  {name}: {c.K:,} groups, max k_u={mk}, median PUs/group "
              f"{np.median(c.npu_g):,.0f}, n_g/k_u = {np.median(c.npu_g)/mk:.1f}")
        print(f"    {'vote noise':<12}{'error':>9}{'released':>11}{'C_e/C_v':>9}{'tau':>10}"
              f"  best split")
        out = {}
        for kind in ("Laplace", "Gaussian"):
            best = None
            for cv, (eb, ee, ev) in itertools.product(cus, SPLITS):
                # both arms get the SAME total delta for partition selection: DELTA.
                # Laplace spends all of it on the threshold; Gaussian must split it between
                # the mechanism and the threshold, which is a real cost to the Gaussian arm.
                if kind == "Laplace":
                    thr = tau(EPS * ee, DELTA, cv)
                    scale = cv / (EPS * ee)
                else:
                    sigma = gauss_sigma(np.sqrt(min(cv, mk)), EPS * ee, DELTA / 2)
                    thr = gauss_tau(sigma, DELTA / 2, cv)
                es, rl = [], []
                for t in range(a.trials):
                    noise = (r.laplace(0, scale, size=c.K) if kind == "Laplace"
                             else r.normal(0, sigma, size=c.K))
                    rel = vt[(t, cv)] + noise >= thr
                    y = np.where(rel, tot[(eb, t)]
                                 + r.laplace(0, Bs[(eb, t)] / (EPS * ev), size=c.K), 0.0)
                    es.append(c.score(y))
                    rl.append(rel.sum())
                e = float(np.mean(es))
                if best is None or e < best[0]:
                    best = (e, cv, (eb, ee, ev), float(np.mean(rl)), thr)
            out[kind] = best
            print(f"    {kind:<12}{100*best[0]:>8.2f}%{best[3]:>11,.0f}{best[1]:>9}"
                  f"{best[4]:>10,.0f}  ({best[2][0]:.3f},{best[2][1]:.3f},{best[2][2]:.3f})",
                  flush=True)
        lp, gs = out["Laplace"][0], out["Gaussian"][0]
        print(f"    -> Gaussian votes: {lp/gs:.2f}x  ({out['Laplace'][3]:,.0f} -> "
              f"{out['Gaussian'][3]:,.0f} of {c.K:,} groups released)\n")
        del c


if __name__ == "__main__":
    main()
