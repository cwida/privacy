"""Buy l2 slack on the VALUE channel by forbidding concentration.

Earlier reasoning declared the value channel Laplace-only: under a pure l1 norm clip a PU may put
all its mass in one group, so Delta_2 = Delta_1 = B and Gaussian has nothing to buy. That is true
of the clip as written -- but concentration is a CHOICE, and we can forbid it.

Clip each cell to +/-b FIRST, then scale the vector so its l1 norm is <= B. Now

    ||v_u||_1 <= B   and   ||v_u||_inf <= b     =>   ||v_u||_2 <= sqrt(B*b)

since sum v^2 <= max|v| * sum|v|. (Scaling only shrinks, so the inf bound survives it.) Gaussian
therefore beats Laplace on the value channel iff

    B / sqrt(B*b) > sqrt(ln(1.25/delta))   i.e.   B/b > ln(1.25/delta) ~ 14

so a per-cell cap an order of magnitude below B already flips the channel. The cost is bias: small
b truncates the heavy cells. b is a free parameter -- Google has no equivalent, because its
per-cell cap U must also carry the whole bound (its Delta_2 = sqrt(C_v)*U = sqrt(Delta_1 * U), the
same form, but with U pinned by ApproxBounds rather than tunable).

Arms, each tuned over its own C_e x C_v/b x budget split:
  published   Wilson et al.: one C_u, Laplace everywhere
  ours-l1     l1 norm clip, Laplace values
  ours-dual   l1 norm clip + per-cell cap b, GAUSSIAN values (this file's proposal)

    python3 attacks/dual_clip.py [--grouping month|nation] [--eps 1.0]
"""

import argparse
import itertools

import numpy as np
from scipy.stats import norm

from fineness_sweep import (DELTA, GROUPINGS, SPLITS, Cells, approx_bounds, google_values, tau,
                            votes)


def gauss_sigma(l2, eps, delta):
    return l2 * np.sqrt(2.0 * np.log(1.25 / delta)) / eps


def dual_clip(c, B, b):
    """Cap each cell at b, then scale each PU so its l1 norm is <= B. Returns per-group totals
    and the realised (l1, linf, l2) sensitivity, so the bound can be checked rather than assumed."""
    cl = np.clip(c.val, -b, b)
    n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
    v = cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi]
    l1 = np.bincount(c.pi, weights=np.abs(v), minlength=c.P).max()
    l2 = np.sqrt(np.bincount(c.pi, weights=v * v, minlength=c.P).max())
    assert l1 <= B * 1.000001, f"l1 {l1} > {B}"
    assert l2 <= np.sqrt(B * b) * 1.000001, f"l2 {l2} > {np.sqrt(B*b)}"
    return np.bincount(c.gi, weights=v, minlength=c.K), l1, l2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--filter", default="c_acctbal>=8000")
    ap.add_argument("--grouping", default="month|nation")
    ap.add_argument("--eps", type=float, default=1.0)
    ap.add_argument("--trials", type=int, default=3)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    c = Cells(con, GROUPINGS[a.grouping][0], a.filter)
    EPS = a.eps
    mk = int(c.k_u.max())
    cus = sorted({1, 2, 3, 5, 8, 13, 21, 34, 55, mk})
    r = np.random.default_rng(99)
    ranks = [c.rank_random(r) for _ in range(a.trials)]
    vt = {(t, ce): votes(c, ranks[t], ce) for t in range(a.trials) for ce in cus}
    ebs = sorted({s[0] for s in SPLITS})

    print(f"{a.grouping}: {c.K:,} groups, {c.P:,} PUs, max k_u={mk}; eps={EPS}, delta={DELTA:g}")
    print(f"Gaussian beats Laplace on values iff B/b > ln(1.25/delta) = "
          f"{np.log(1.25/DELTA):.1f}\n")

    U, B, g_tot, o_tot, d_tot = {}, {}, {}, {}, {}
    for eb in ebs:
        for t in range(a.trials):
            U[(eb, t)] = approx_bounds(c.val, EPS * eb, r)
            for cv in cus:
                g_tot[(eb, t, cv)] = google_values(c, cv, U[(eb, t)], ranks[t], False)
            B[(eb, t)] = approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
            o_tot[(eb, t)], _, _ = dual_clip(c, B[(eb, t)], np.inf)
    # per-cell caps as fractions of B: this is the new free parameter
    FRACS = [1.0, 1 / 4, 1 / 16, 1 / 64, 1 / 256, 1 / 1024, 1 / 4096]
    for eb in ebs:
        for t in range(a.trials):
            for f in FRACS:
                d_tot[(eb, t, f)] = dual_clip(c, B[(eb, t)], B[(eb, t)] * f)[0]

    def run(arm, ce, cv, f, split):
        eb, ee, ev = split
        thr = tau(EPS * ee, DELTA, ce)
        es = []
        for t in range(a.trials):
            rel = (vt[(t, ce)] + r.laplace(0, ce / (EPS * ee), size=c.K) >= thr) & (vt[(t, ce)] > 0)
            if arm == "published":
                tot = g_tot[(eb, t, cv)]
                nz = r.laplace(0, cv * U[(eb, t)] / (EPS * ev), size=c.K)
            elif arm == "ours-l1":
                tot = o_tot[(eb, t)]
                nz = r.laplace(0, B[(eb, t)] / (EPS * ev), size=c.K)
            else:
                tot = d_tot[(eb, t, f)]
                sig = gauss_sigma(np.sqrt(B[(eb, t)] * B[(eb, t)] * f), EPS * ev, DELTA / 2)
                nz = r.normal(0, sig, size=c.K)
            es.append(c.score(np.where(rel, tot + nz, 0.0)))
        return float(np.mean(es))

    best = {}
    for arm in ("published", "ours-l1", "ours-dual"):
        cvs = cus if arm == "published" else [None]
        fs = FRACS if arm == "ours-dual" else [None]
        cands = [(run(arm, ce, cv, f, s), ce, cv, f, s)
                 for ce, cv, f, s in itertools.product(cus, cvs, fs, SPLITS)]
        best[arm] = min(cands)
        e, ce, cv, f, s = best[arm]
        extra = f"C_v={cv}" if cv else (f"b=B/{1/f:.0f}" if f else "")
        print(f"  {arm:<12}{100*e:>8.3f}%   C_e={ce:<4}{extra:<12} "
              f"split=({s[0]:.3f},{s[1]:.3f},{s[2]:.3f})")

    print(f"\n  ours-dual vs published : {best['published'][0]/best['ours-dual'][0]:.2f}x")
    print(f"  ours-dual vs ours-l1   : {best['ours-l1'][0]/best['ours-dual'][0]:.2f}x")

    print(f"\n  per-cell cap sweep at the best split ({'b=B/x'}):")
    _, ce, _, _, s = best["ours-dual"]
    eb = s[0]
    print(f"    {'b':>10}{'B/b':>9}{'l2/l1':>8}{'error':>10}")
    for f in FRACS:
        e = min(run("ours-dual", ce2, None, f, s2)
                for ce2, s2 in itertools.product(cus, SPLITS))
        print(f"    B/{1/f:<8.0f}{1/f:>9.0f}{np.sqrt(f):>8.3f}{100*e:>9.3f}%")


if __name__ == "__main__":
    main()
