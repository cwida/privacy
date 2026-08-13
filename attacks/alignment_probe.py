"""Is Google's rescale trick relying on a property of TPC-H rather than of the mechanism?

Rescale-to-true-total moves most of every PU's mass onto that PU's top-C_v cells. The group-level
bias of doing so is small only if PUs disagree about which of their groups is biggest -- then the
misallocation cancels across many PUs. TPC-H ship dates are uniform, so which of a customer's
months is largest is essentially noise, which is the best possible case for the trick.

This tilts each PU's allocation toward a shared per-group direction while holding fixed
EVERYTHING either mechanism's accounting reads:
  - each PU's total (hence every per-PU norm, hence our B)
  - the (PU, group) incidence, so k_u, the vote histograms and tau are untouched
  - sum of truth
Only *how* a PU spreads its own total changes, and how much PUs agree with each other.

    python3 attacks/alignment_probe.py [--alphas 0,0.5,1,2]
"""

import argparse
import copy
import itertools

import numpy as np

from fineness_sweep import ARMS, EPS, GROUPINGS, Cache, Cells, tune


def alignment(c):
    """HHI of PUs' argmax group, x K. 1.0 = PUs disagree completely; K = all agree."""
    o = np.lexsort((-c.val, c.pi))
    top_g = c.gi[o][c.starts[:-1]]                    # each PU's largest-value group
    frac = np.bincount(top_g, minlength=c.K) / c.P
    return float(np.sum(frac ** 2) * c.K)


def tilt(c, alpha, r):
    """Tilt every PU toward a shared random per-group direction, preserving each PU's total."""
    d = copy.copy(c)
    z = r.standard_normal(c.K)                        # shared direction, independent of group size
    w = c.val * np.exp(alpha * z[c.gi])
    tot_new = np.bincount(c.pi, weights=w, minlength=c.P)
    d.val = w * (c.norms / np.maximum(tot_new, 1e-30))[c.pi]
    d.truth = np.bincount(c.gi, weights=d.val, minlength=c.K)
    o = np.lexsort((-np.abs(d.val), d.pi))
    d.rank_top = np.empty(len(d.val), np.int64)
    d.rank_top[o] = np.arange(len(d.val)) - d.starts[d.pi[o]]
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--filter", default="c_acctbal>=8000")
    ap.add_argument("--grouping", default="month|nation")
    ap.add_argument("--alphas", default="0,0.5,1,2")
    ap.add_argument("--trials", type=int, default=3)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    c0 = Cells(con, GROUPINGS[a.grouping][0], a.filter)
    mk = int(c0.k_u.max())
    cus = sorted({1, 2, 5, 10, 19, 30, mk // 2 or 1, mk})
    print(f"{a.grouping}, sf10, WHERE {a.filter}: {c0.K:,} groups, {c0.P:,} PUs, max k_u={mk}")
    print("invariant across every row: per-PU totals, per-PU norms, (PU,group) incidence,")
    print(f"sum of truth = {c0.truth.sum():,.0f}, and therefore our B and both vote histograms\n")
    hdr = (f"{'alpha':>6}{'alignment':>11}{'max cell':>13}{'Google':>8}{'+top':>8}"
           f"{'+rescale':>10}{'ours':>8}{'gap':>7}  Google C_e/C_v")
    print(hdr)
    print("-" * (len(hdr) + 6))
    for alpha in [float(x) for x in a.alphas.split(",")]:
        r = np.random.default_rng(4)
        c = c0 if alpha == 0.0 else tilt(c0, alpha, r)
        assert np.allclose(np.bincount(c.pi, weights=c.val, minlength=c.P), c0.norms)
        cache = Cache(c, cus, a.trials)
        res = {arm: tune(cache, cus, arm) for arm in ARMS}
        g = min(res[k][0] for k in ARMS if k != "ours")
        o = res["ours"]
        best_g = min((k for k in ARMS if k != "ours"), key=lambda k: res[k][0])
        print(f"{alpha:>6.1f}{alignment(c):>11.2f}{c.val.max():>13,.0f}"
              f"{100*res['google'][0]:>7.2f}%{100*res['google+top'][0]:>7.2f}%"
              f"{100*res['google+top+rescale'][0]:>9.2f}%{100*o[0]:>7.2f}%{g/o[0]:>6.2f}x"
              f"  {res[best_g][1][0]}/{res[best_g][1][1]}", flush=True)
        del cache
    print("\nalpha=0 is real TPC-H. If the gap grows with alignment, the rescale trick -- and so")
    print("the 1.3x -- is a property of this data generator, not of the two mechanisms.")


if __name__ == "__main__":
    main()
