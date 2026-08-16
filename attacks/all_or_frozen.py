"""Does Dandan's All-or-Frozen rule ever fire?

Her mechanism (draft 15 Aug, section 2):
  - automatic bounding already builds, per group, a B=64-bin log histogram of the per-PU values
    for one randomly chosen aggregation attribute, noised with Laplace(C_u/eps_B) per bin
  - certify group g by M_g = max_i noisy_bin_i(g) >= tau_AF
  - AllPass = AND over every group in the query's own group set
  - AllPass -> release the query-specific (small) group set; otherwise fall back to G_fix

The privacy analysis is sound: conventional per-group thresholding leaks if ANY of a PU's <= C_u
newly created groups passes (an OR, needing rho_tau <= delta/C_u), whereas All-or-Frozen needs ALL
of them to pass (an AND, needing only rho_tau <= delta). That is worth
tau_PG - tau_AF ~ (C_u/eps_B)*log(C_u).

The open question is empirical and it is about the AND, not the threshold: with thousands of
groups, does "every group passes" ever happen? One small group forces the fallback.

    python3 attacks/all_or_frozen.py
"""

import argparse

import numpy as np

from fineness_sweep import GROUPINGS, Cells

B_BINS = 64


def tau_af(b, delta, B=B_BINS):
    """inf{tau >= 1 : (1 - .5 e^{-(tau-1)/b})(1 - .5 e^{-tau/b})^{B-1} >= 1 - delta}"""
    lo, hi = 1.0, 1.0 + 200.0 * b
    for _ in range(200):
        mid = (lo + hi) / 2
        p = (1 - 0.5 * np.exp(-(mid - 1) / b)) * (1 - 0.5 * np.exp(-mid / b)) ** (B - 1)
        if p >= 1 - delta:
            hi = mid
        else:
            lo = mid
    return hi


def tau_pg(b, delta, cu, B=B_BINS):
    """Conventional per-group: the same but with delta split across the C_u groups a PU creates."""
    return tau_af(b, 1.0 - (1.0 - delta) ** (1.0 / cu), B)


def group_histograms(c):
    """Per group, the 64-bin log histogram of its per-(PU,group) values. One PU contributes one
    value to one bin of one group's histogram, per her assumption."""
    v = np.abs(c.val)
    bins = np.clip(np.floor(np.log2(np.maximum(v, 1.0))).astype(int), 0, B_BINS - 1)
    H = np.zeros((c.K, B_BINS))
    np.add.at(H, (c.gi, bins), 1.0)
    return H


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--filter", default="c_acctbal>=8000")
    ap.add_argument("--groupings", default="month,month|priority,month|nation")
    ap.add_argument("--eps-b", type=float, default=0.05)
    ap.add_argument("--delta", type=float, default=1e-6)
    ap.add_argument("--trials", type=int, default=200)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    print(f"filter '{a.filter}', eps_B={a.eps_b}, delta={a.delta:g}, B={B_BINS} bins, "
          f"{a.trials} trials\n")
    r = np.random.default_rng(7)
    for name in a.groupings.split(","):
        c = Cells(con, GROUPINGS[name][0], a.filter)
        H = group_histograms(c)
        maxbin = H.max(axis=1)
        cu = int(c.k_u.max())
        b = cu / a.eps_b
        t_af, t_pg = tau_af(b, a.delta), tau_pg(b, a.delta, cu)
        print(f"  {name}: {c.K:,} groups, max k_u={cu}, Laplace scale b=C_u/eps_B={b:,.0f}")
        print(f"    tau_AF={t_af:,.0f}   tau_PG={t_pg:,.0f}   "
              f"gap {t_pg-t_af:,.0f} (her estimate {b*np.log(cu):,.0f})")
        print(f"    true max bin per group: min {maxbin.min():,.0f}, "
              f"p1 {np.percentile(maxbin,1):,.0f}, median {np.median(maxbin):,.0f}, "
              f"max {maxbin.max():,.0f}")
        allpass, perpass = 0, []
        for _ in range(a.trials):
            noisy = H + r.laplace(0, b, size=H.shape)
            M = noisy.max(axis=1)
            p = M >= t_af
            perpass.append(p.mean())
            allpass += int(p.all())
        print(f"    per-group pass rate at tau_AF: {100*np.mean(perpass):.2f}%")
        print(f"    P[ALL {c.K:,} groups pass]: {allpass}/{a.trials} = "
              f"{100*allpass/a.trials:.1f}%")
        # how many groups would need to exist for AllPass to be likely
        q = float(np.mean(perpass))
        if 0 < q < 1:
            kmax = np.log(0.5) / np.log(q) if q < 1 else np.inf
            print(f"    -> at that per-group rate, AllPass is >50% likely only up to "
                  f"~{kmax:,.0f} groups")
        print(flush=True)
        del c, H


if __name__ == "__main__":
    main()
