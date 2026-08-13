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
    ap.add_argument("--eps", type=float, default=EPS)
    ap.add_argument("--trials", type=int, default=3)
    ap.add_argument("--max-cells", type=int, default=6_000_000)
    a = ap.parse_args()
    eps = a.eps

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    c = Cells(con, GROUPINGS[a.grouping][0], a.filter + a.extra)
    con.close()
    n_cells = len(c.val)
    if n_cells > a.max_cells:
        raise SystemExit(f"refusing: {n_cells:,} cells > --max-cells {a.max_cells:,}")
    label = a.label or (a.grouping + a.extra)
    mk = int(c.k_u.max())
    tot = c.truth
    truth_l1 = float(np.abs(tot).sum())
    print(f"{label}: {n_cells:,} cells, {c.K:,} groups, {c.P:,} PUs, max k_u={mk}, "
          f"median PUs/group={np.median(c.npu_g):,.0f}, n_g/k_u={np.median(c.npu_g)/mk:.1f}")
    print(f"eps={eps}, delta={DELTA:g}, SUM(l_extendedprice), PU=customer, {a.trials} trials")
    print("floor(c) = tau(C=1) = 1 + ln(1/(2 delta/(c+1)))/(eps/(c+1)); release needs n_g/k_u > floor\n")

    r = np.random.default_rng(7)
    lane_of_pu = r.integers(0, M, size=c.P)
    ranks = [c.rank_random(r) for _ in range(a.trials)]
    cs = sorted({1, 2, 5, 10, 30, mk // 2 or 1, mk})
    absv = np.abs(c.val)
    Bs = [float(np.quantile(absv, q)) for q in (0.90, 0.99, 0.999)] + [float(absv.max())]
    Ls = [float(np.quantile(np.abs(tot), q)) for q in (0.5, 0.9, 0.99)] + [float(np.abs(tot).max())]

    lanes = {}
    for cv in cs:
        for bi, B in enumerate(Bs):
            clipped = np.clip(c.val, -B, B)
            for t in range(a.trials):
                lanes[(cv, bi, t)] = lane_matrix(c, ranks[t] < cv, clipped, lane_of_pu)
            del clipped
    vt = {(ce, t): np.bincount(c.gi[ranks[t] < ce], minlength=c.K).astype(float)
          for ce in cs for t in range(a.trials)}

    print(f"{'c':>2} {'arm':<10}{'relL1 SASS':>12}{'relL1 oracle':>14}{'released':>10}"
          f"{'trueL1 kept':>13}{'C_e':>5}{'C_v':>5}{'tau':>12}  (objective)")
    for cc in [int(x) for x in a.aggs.split(",")]:
        eps_eta, delta_eta = eps / (cc + 1.0), DELTA / (cc + 1.0)
        print(f"-- c={cc}: eps_eta={eps_eta:.3f} delta_eta={delta_eta:.3g} "
              f"floor={wilson_tau(eps_eta, delta_eta, 1.0):.1f}  "
              f"eps_cell(C_v)={eps:.1f}/({cc+1}*C_v)")
        masks, thr = {}, {}
        for ce in cs:
            th_l = wilson_tau(eps_eta, delta_eta, ce)
            sig, th_g = gauss_vote(ce, mk, eps_eta, delta_eta)
            thr[("lap", ce)], thr[("gauss", ce)] = th_l, th_g
            for t in range(a.trials):
                v = vt[(ce, t)]
                masks[("lap", ce, t)] = (v + r.laplace(0, ce / eps_eta, size=c.K) >= th_l) & (v >= 1)
                masks[("gauss", ce, t)] = (v + r.normal(0, sig, size=c.K) >= th_g) & (v >= 1)
        vals = {}
        for cv in cs:
            ec, dc = eps / ((cc + 1.0) * cv), DELTA / ((cc + 1.0) * cv)
            for bi in range(len(Bs)):
                for li, lam in enumerate(Ls):
                    for t in range(a.trials):
                        vals[(cv, bi, li, t)] = smooth_median_release(lanes[(cv, bi, t)], lam, ec, dc, r)
        res = {}
        for arm, vote, coupled in (("today", "lap", True), ("decoupled", "lap", False),
                                   ("gaussian", "gauss", False)):
            grid = []
            for ce in cs:
                for cv in ([ce] if coupled else cs):
                    for bi, li in itertools.product(range(len(Bs)), range(len(Ls))):
                        es, rn, kp = [], [], []
                        for t in range(a.trials):
                            m = masks[(vote, ce, t)]
                            out = np.where(m, vals[(cv, bi, li, t)], 0.0)
                            es.append(float(np.abs(out - tot).sum() / truth_l1))
                            rn.append(int(m.sum()))
                            kp.append(float(np.abs(tot[m]).sum() / truth_l1))
                        grid.append((float(np.mean(es)), float(np.mean(kp)), float(np.mean(rn)),
                                     ce, cv, thr[(vote, ce)]))
            best_l1 = min(grid, key=lambda g: g[0])
            best_ks = max(grid, key=lambda g: g[1])
            res[arm] = (best_l1, best_ks)
            for tag, b in (("min relL1", best_l1), ("max key-set", best_ks)):
                print(f"{cc:>2} {arm:<10}{100*b[0]:>11.1f}%{100*(1-b[1]):>13.2f}%{b[2]:>10,.0f}"
                      f"{100*b[1]:>12.1f}%{b[3]:>5}{b[4]:>5}{b[5]:>12,.0f}  ({tag})", flush=True)
        t_, d_, g_ = res["today"], res["decoupled"], res["gaussian"]
        print(f"   -> key-set (oracle values): today {100*(1-t_[1][1]):.2f}% | decoupled-Laplace "
              f"{100*(1-d_[1][1]):.2f}% | Gaussian {100*(1-g_[1][1]):.2f}%  "
              f"=> {(1-d_[1][1])/max(1-g_[1][1],1e-12):.2f}x vs decoupled, "
              f"{(1-t_[1][1])/max(1-g_[1][1],1e-12):.2f}x vs today")
        print(f"   -> end-to-end SASS median: today {100*t_[0][0]:.1f}% | decoupled "
              f"{100*d_[0][0]:.1f}% | Gaussian {100*g_[0][0]:.1f}%\n", flush=True)


if __name__ == "__main__":
    main()
