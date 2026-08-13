"""Geometry-matched votes inside the SMOOTH-SENSITIVITY (dp_sass) release, not the Laplace path.

Everything previously measured about Gaussian partition-selection votes was measured with a
summed value channel + Laplace. This harness re-runs it against a genuine sample-and-aggregate
median release, mirroring src/compiler/privacy_mechanisms.cpp and
src/aggregates/dp_laplace_noise.cpp line for line:

  per-(PU, group) partial      clipped to dp_sum_bound                (BuildSampleMedianLowerExpression)
  per-PU group truncation      row_number() <= C_u over (PU)          (ApplyMaxGroupsContributed)
  lane assignment              lane(PU) = hash(PU) mod 64, s = 1      (dp_sample_lanes = 1)
  lane answer                  64 * sum of that lane's PU partials    (DpSampleRescale(1) = 64)
  clip                         to [-LAMBDA, LAMBDA]                   (dp_sass_sum_output_bound)
  release                      lower median of the 64 sorted answers  (median_idx = (m+1)/2 - 1)
  smooth sensitivity           exact NRS envelope over ALL i<=p<=j    (SmoothMedianSensitivityExact64)
                               S* = max (x_j - x_i) exp(-beta (j-i-1)), x_0/x_65 = domain sentinels
                               beta = eps_cell / (2 ln(2/delta_cell)), p = 32
  noise                        Laplace(2 S* / eps_cell)               (scale = 2*smooth/epsilon)
  accounting                   eps_eta = eps/(c+1), delta_eta = delta/(c+1),
                               eps_cell = eps/((c+1) C_v), delta_cell = delta/((c+1) C_v)

Only the VOTE channel changes between arms:

  today       one C_u truncates votes AND values AND divides the cell budget; Laplace(C_u/eps_eta)
              votes against the Wilson tau.  (this is what src/ does)
  decoupled   separate C_e (votes) and C_v (values+budget), still Laplace votes.  steelman.
  gaussian    separate C_e, votes noised N(0, sigma^2) with sigma = sqrt(min(C_e,k_max)) *
              sqrt(2 ln(1.25/delta_g)) / eps_eta, threshold 1 + sigma z_{1-delta_thr/C_e}.

delta handling: the Laplace arms spend the whole delta_eta on the threshold; the Gaussian arm
must split delta_eta between the mechanism and the threshold. A vote-support gate
(released &= votes >= 1) is applied in every arm so tau's union bound covers only the C_e groups
a PU actually votes in.

Scoring: relative L1 over the TRUE, uncapped, untruncated key set; a suppressed group is charged
its full true value. Also reported: released groups and the share of true L1 mass released, which
isolates the vote channel from the (very noisy at eps=1) smooth-median value channel.

    python3 attacks/sass_vote_geometry.py --grouping 'month|nation'
"""

import argparse
import itertools

import numpy as np
from scipy.stats import norm

from fineness_sweep import DELTA, EPS, GROUPINGS, Cells, tau

M = 64            # dp_sass_m
P_RANK = 32       # SMOOTH_MEDIAN_64_P (1-indexed lower-median rank in the padded array)
LANE_RESCALE = 64.0  # DpSampleRescale(sample_lanes=1) = 1/(1-(63/64)^1)


# ---------------------------------------------------------------- vote channel

def wilson_tau(eps_eta, delta_eta, cu):
    return tau(eps_eta, delta_eta, cu)


def gauss_vote(ce, k_max, eps_eta, delta_eta):
    """sigma and threshold for Gaussian votes. delta_eta is split half mechanism, half threshold."""
    d_mech = delta_eta / 2.0
    d_thr = delta_eta / 2.0
    sigma = np.sqrt(min(ce, k_max)) * np.sqrt(2.0 * np.log(1.25 / d_mech)) / eps_eta
    thr = 1.0 + sigma * norm.ppf(1.0 - d_thr / max(ce, 1))
    return sigma, thr


# ------------------------------------------------- smooth-sensitivity median release

def smooth_median_release(sorted_lanes, lam, eps_cell, delta_cell, rng, noise=True,
                          return_stats=False, clamp=False):
    """Exact NRS median smooth sensitivity + Laplace, vectorised over groups.

    sorted_lanes: (K, 64) lane answers already sorted ascending (clipping is monotone, so the
    caller may sort once and clip per LAMBDA).
    """
    k = sorted_lanes.shape[0]
    x = np.empty((k, M + 2))
    x[:, 0] = -lam
    np.clip(sorted_lanes, -lam, lam, out=x[:, 1:M + 1])
    x[:, M + 1] = lam
    beta = eps_cell / (2.0 * np.log(2.0 / delta_cell))
    med = x[:, P_RANK]                      # 1-indexed rank 32 == 0-indexed 31 of the raw lanes
    smooth = np.zeros(k)
    js = np.arange(P_RANK, M + 2)
    right = x[:, P_RANK:M + 2]
    for i in range(0, P_RANK + 1):
        w = np.exp(-beta * (js - i - 1).astype(float))
        cand = (right - x[:, i][:, None]) * w[None, :]
        np.maximum(smooth, cand.max(axis=1), out=smooth)
    np.maximum(smooth, 0.0, out=smooth)
    scale = 2.0 * smooth / eps_cell
    out = med + (rng.laplace(0.0, 1.0, size=k) * scale if noise else 0.0)
    # src/ does NOT clamp the released SUM/COUNT median back into the public domain (only the AVG
    # ratio is clamped), so the default here matches src/. clamp=True is free DP post-processing.
    if clamp:
        out = np.clip(out, -lam, lam)
    return (out, smooth, scale) if return_stats else out


def lane_matrix(c, keep, clipped, lane_of_pu):
    """(K, 64) lane answers: 64 * sum of the kept per-PU partials routed to that lane."""
    idx = c.gi[keep] * M + lane_of_pu[c.pi[keep]]
    ls = np.bincount(idx, weights=clipped[keep], minlength=c.K * M).reshape(c.K, M)
    ls *= LANE_RESCALE
    ls.sort(axis=1)
    return ls


# ---------------------------------------------------------------------- driver

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--filter", default="c_acctbal>=8000")
    ap.add_argument("--grouping", default="month|nation")
    ap.add_argument("--extra", default="", help="extra SQL predicate, e.g. ' AND c_nationkey<5'")
    ap.add_argument("--label", default=None)
    ap.add_argument("--aggs", default="1,2,4", help="c = number of user aggregates")
    ap.add_argument("--trials", type=int, default=3)
    ap.add_argument("--max-cells", type=int, default=6_000_000)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")

    gexpr = GROUPINGS[a.grouping][0]
    c = Cells(con, gexpr, a.filter + a.extra)
    con.close()
    n_cells = len(c.val)
    if n_cells > a.max_cells:
        raise SystemExit(f"refusing: {n_cells:,} cells > --max-cells {a.max_cells:,}")
    label = a.label or (a.grouping + a.extra)
    mk = int(c.k_u.max())
    ratio = float(np.median(c.npu_g)) / mk
    print(f"{label}: {n_cells:,} cells, {c.K:,} groups, {c.P:,} PUs, max k_u={mk}, "
          f"median PUs/group={np.median(c.npu_g):,.0f}, n_g/k_u={ratio:.1f}")
    print(f"eps={EPS}, delta={DELTA:g}, SUM(l_extendedprice), PU=customer, {a.trials} trials\n")

    r = np.random.default_rng(7)
    lane_of_pu = r.integers(0, M, size=c.P)
    ranks = [c.rank_random(r) for _ in range(a.trials)]

    cs = sorted({1, 2, 5, 10, 30, mk // 2 or 1, mk})
    absv = np.abs(c.val)
    Bs = [float(np.quantile(absv, q)) for q in (0.90, 0.99, 0.999)] + [float(absv.max())]
    tot = c.truth
    Ls = [float(np.quantile(np.abs(tot), q)) for q in (0.5, 0.9, 0.99)] + [float(np.abs(tot).max())]

    # ---- value channel: cache sorted lane matrices per (C_v, B, trial); they do not depend on
    # the vote arm, LAMBDA, or c.
    lanes = {}
    for cv in cs:
        for bi, B in enumerate(Bs):
            clipped = np.clip(c.val, -B, B)
            for t in range(a.trials):
                lanes[(cv, bi, t)] = lane_matrix(c, ranks[t] < cv, clipped, lane_of_pu)
            del clipped

    # ---- vote channel: cache raw (untruncated-by-arm) vote counts per (C_e, trial)
    vt = {(ce, t): np.bincount(c.gi[ranks[t] < ce], minlength=c.K).astype(float)
          for ce in cs for t in range(a.trials)}

    truth_l1 = float(np.abs(tot).sum())
    print(f"{'c':>2} {'arm':<12}{'rel L1':>9}{'released':>10}{'true L1 kept':>14}"
          f"{'C_e':>5}{'C_v':>5}{'B':>11}{'LAMBDA':>12}{'tau':>10}")
    results = {}
    for cc in [int(x) for x in a.aggs.split(",")]:
        eps_eta = EPS / (cc + 1.0)
        delta_eta = DELTA / (cc + 1.0)
        # released masks per (arm, C_e, trial)
        masks = {}
        for ce in cs:
            th_lap = wilson_tau(eps_eta, delta_eta, ce)
            sig, th_g = gauss_vote(ce, mk, eps_eta, delta_eta)
            for t in range(a.trials):
                v = vt[(ce, t)]
                masks[("lap", ce, t)] = ((v + r.laplace(0, ce / eps_eta, size=c.K) >= th_lap)
                                         & (v >= 1))
                masks[("gauss", ce, t)] = ((v + r.normal(0, sig, size=c.K) >= th_g) & (v >= 1))
            masks[("lap", ce, "thr")] = th_lap
            masks[("gauss", ce, "thr")] = th_g
        # noised value vectors per (C_v, B, LAMBDA, trial)
        vals = {}
        for cv in cs:
            eps_cell = EPS / ((cc + 1.0) * cv)
            delta_cell = DELTA / ((cc + 1.0) * cv)
            for bi in range(len(Bs)):
                for li, lam in enumerate(Ls):
                    for t in range(a.trials):
                        vals[(cv, bi, li, t)] = smooth_median_release(
                            lanes[(cv, bi, t)], lam, eps_cell, delta_cell, r)

        for arm, vote, coupled in (("today", "lap", True), ("decoupled", "lap", False),
                                   ("gaussian", "gauss", False)):
            best = None
            for ce in cs:
                cvs = [ce] if coupled else cs
                for cv, bi, li in itertools.product(cvs, range(len(Bs)), range(len(Ls))):
                    es, rel_n, kept = [], [], []
                    for t in range(a.trials):
                        m = masks[(vote, ce, t)]
                        out = np.where(m, vals[(cv, bi, li, t)], 0.0)
                        es.append(float(np.abs(out - tot).sum() / truth_l1))
                        rel_n.append(int(m.sum()))
                        kept.append(float(np.abs(tot[m]).sum() / truth_l1))
                    e = float(np.mean(es))
                    if best is None or e < best[0]:
                        best = (e, np.mean(rel_n), np.mean(kept), ce, cv, Bs[bi], Ls[li],
                                masks[(vote, ce, "thr")])
            results[(cc, arm)] = best
            print(f"{cc:>2} {arm:<12}{100*best[0]:>8.2f}%{best[1]:>10,.0f}{100*best[2]:>13.1f}%"
                  f"{best[3]:>5}{best[4]:>5}{best[5]:>11,.0f}{best[6]:>12,.0f}{best[7]:>10,.0f}",
                  flush=True)
        t_ = results[(cc, "today")]
        d_ = results[(cc, "decoupled")]
        g_ = results[(cc, "gaussian")]
        print(f"   -> c={cc}: gaussian vs today {t_[0]/g_[0]:.2f}x, vs decoupled-Laplace "
              f"{d_[0]/g_[0]:.2f}x | groups {t_[1]:,.0f}/{d_[1]:,.0f} -> {g_[1]:,.0f} of {c.K:,}"
              f" | true L1 kept {100*t_[2]:.1f}%/{100*d_[2]:.1f}% -> {100*g_[2]:.1f}%\n",
                  flush=True)


if __name__ == "__main__":
    main()
