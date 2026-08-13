"""Is the Gaussian-votes gain an artifact of the total-L1 metric?

The claim under attack: on floor-binding queries Gaussian votes take ~100% relative L1 down to
16-27%, releasing 94% of the key set instead of 6-30%. Total relative L1 is dominated by large
groups, so a mechanism could score well while the groups it newly releases are pure noise.

This script re-tunes BOTH arms (Laplace votes vs Gaussian votes -- identical l1-clip value
channel, identical budget grid) separately under six metrics:

    L1        sum|out-truth| / sum|truth|                       (the metric used so far)
    medRE     median over ALL groups of |out-truth|/truth       (suppressed group = 1.0)
    MAPEc     mean over ALL groups of min(relerr, 1)            (capped: never worse than silence)
    MAPE      mean over ALL groups of relerr                    (uncapped: punishes junk releases)
    nRMSE     sqrt(mean (out-truth)^2) / sqrt(mean truth^2)
    p95       95th percentile of per-group relative error

and reports, at each arm's own optimum: the per-group error distribution, the release rate by
truth quintile (is the released set biased toward large groups?), what fraction of released
groups are noise (relerr > 1), and the error over the INTERSECTION of the two arms' released
sets (where the only difference left is how much budget each arm had left for values).

    python3 attacks/metric_audit.py --grouping "week|nation" --filter "c_acctbal>=8000 and c_nationkey<5"
"""

import argparse
import itertools

import numpy as np
from scipy.stats import norm

from fineness_sweep import (DELTA, EPS, GROUPINGS, SPLITS, Cells, approx_bounds, tau)

METRICS = ["L1", "medRE", "MAPEc", "MAPE", "nRMSE", "p95"]


class AnyCells(Cells):
    """Cells for an arbitrary (table, PU, group, measure) instead of the TPC-H one."""

    def __init__(self, con, frm, pu, gexpr, where, val):
        rows = con.execute(f"""
            WITH c AS (SELECT {pu} AS pu, {gexpr} AS g, sum({val}) AS t
                       FROM {frm} WHERE {where} GROUP BY 1,2)
            SELECT dense_rank() OVER (ORDER BY pu)-1 AS pid,
                   dense_rank() OVER (ORDER BY g)-1 AS gid, t FROM c""").fetchnumpy()
        self._init_from(rows["pid"].astype(np.int64), rows["gid"].astype(np.int64),
                        rows["t"].astype(np.float64))


def score_all(out, truth, rel):
    """All six metrics for one release. Suppressed groups are scored as released 0."""
    err = np.abs(out - truth)
    re = err / truth
    return {"L1": err.sum() / truth.sum(),
            "medRE": float(np.median(re)),
            "MAPEc": float(np.mean(np.minimum(re, 1.0))),
            "MAPE": float(np.mean(re)),
            "nRMSE": float(np.sqrt(np.mean(err ** 2)) / np.sqrt(np.mean(truth ** 2))),
            "p95": float(np.quantile(re, 0.95))}


def build(c, cus, trials, seed=99):
    r = np.random.default_rng(seed)
    ranks = [c.rank_random(r) for _ in range(trials)]
    cache = {"trials": trials,
             "votes": {(t, ce): np.bincount(c.gi[ranks[t] < ce], minlength=c.K).astype(float)
                       for t in range(trials) for ce in cus},
             "B": {}, "tot": {}}
    for eb in sorted({s[0] for s in SPLITS}):
        for t in range(trials):
            B = approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
            cache["B"][(eb, t)] = B
            cl = np.clip(c.val, -B, B)
            n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
            cache["tot"][(eb, t)] = np.bincount(
                c.gi, weights=cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi],
                minlength=c.K)
    return cache, r


def realise(c, cache, ce, split, kind, mk, r, gate=True):
    """One config -> per-trial (released mask, released output, value noise scale)."""
    eb, ee, ev = split
    if kind == "Gaussian":
        sigma = (np.sqrt(min(ce, mk)) * np.sqrt(2.0 * np.log(1.25 / (DELTA / 2)))
                 / (EPS * ee))
        thr = 1.0 + sigma * norm.ppf(1.0 - (DELTA / 2) / max(ce, 1))
    else:
        thr = tau(EPS * ee, DELTA, ce)
        scale = ce / (EPS * ee)
    outs = []
    for t in range(cache["trials"]):
        vn = (r.normal(0, sigma, size=c.K) if kind == "Gaussian"
              else r.laplace(0, scale, size=c.K))
        v = cache["votes"][(t, ce)]
        rel = v + vn >= thr
        if gate:
            rel &= v > 0
        s = cache["B"][(eb, t)] / (EPS * ev)
        out = np.where(rel, cache["tot"][(eb, t)] + r.laplace(0, s, size=c.K), 0.0)
        outs.append((rel, out, s))
    return outs


def tune(c, cache, cus, kind, mk, r, gate=True):
    """Best config per metric, plus the full metric table at every config."""
    res = {}
    for ce, split in itertools.product(cus, SPLITS):
        outs = realise(c, cache, ce, split, kind, mk, r, gate)
        m = {k: float(np.mean([score_all(o, c.truth, rl)[k] for rl, o, _ in outs]))
             for k in METRICS}
        m["_rel"] = float(np.mean([rl.sum() for rl, _, _ in outs]))
        res[(ce, split)] = m
    best = {k: min(res, key=lambda cfg: res[cfg][k]) for k in METRICS}
    return best, res


def describe(c, outs, label):
    """Error distribution and release-set bias at one config."""
    rel = np.stack([o[0] for o in outs])
    out = np.stack([o[1] for o in outs])
    re = np.abs(out - c.truth) / c.truth
    rr = rel.mean(0)                                    # per-group release rate
    q = np.quantile(c.truth, [0.2, 0.4, 0.6, 0.8])
    dec = np.digitize(c.truth, q)
    releasedre = re[rel]
    print(f"    {label}")
    print(f"      released {rel.sum(1).mean():,.0f}/{c.K:,} groups "
          f"({100*rel.sum(1).mean()/c.K:.1f}%), covering "
          f"{100*np.mean([(c.truth*r_).sum() for r_ in rel])/c.truth.sum():.1f}% of true mass; "
          f"value noise scale b={np.mean([o[2] for o in outs]):,.0f}")
    print(f"      rel.err ALL groups   p5 {np.quantile(re,.05):>7.3f}  p50 "
          f"{np.quantile(re,.5):>7.3f}  p95 {np.quantile(re,.95):>8.3f}  max "
          f"{re.max():>10.1f}")
    if releasedre.size:
        print(f"      rel.err RELEASED     p5 {np.quantile(releasedre,.05):>7.3f}  p50 "
              f"{np.quantile(releasedre,.5):>7.3f}  p95 {np.quantile(releasedre,.95):>8.3f}"
              f"  max {releasedre.max():>10.1f}")
        print(f"      released groups with rel.err > 0.5: "
              f"{100*np.mean(releasedre>0.5):.1f}%,  > 1.0: {100*np.mean(releasedre>1):.1f}%")
    print("      release rate by true-size quintile (small->large): " +
          " ".join(f"{100*rr[dec==i].mean():5.1f}%" for i in range(5)))
    print("      median rel.err by quintile (released only):       " +
          " ".join((f"{np.median(re[:,dec==i][rel[:,dec==i]]):5.2f} "
                    if rel[:, dec == i].any() else "    -  ") for i in range(5)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--grouping", default="week|nation")
    ap.add_argument("--filter", default="c_acctbal>=8000 and c_nationkey<5")
    ap.add_argument("--trials", type=int, default=4)
    ap.add_argument("--eps", type=float, default=None)
    ap.add_argument("--query", default=None,
                    help="a name from band_map.QUERIES (StackOverflow / ClickBench / TPC-H) "
                         "instead of --grouping/--filter")
    ap.add_argument("--skew", type=float, default=0.0,
                    help="multiply every cell of group g by lognormal(0,skew). TPC-H group "
                         "totals are near-uniform, which is exactly the case where total-L1 "
                         "and per-group metrics cannot disagree. This injects the group-size "
                         "skew that real data has, WITHOUT touching the vote structure.")
    a = ap.parse_args()
    if a.eps:
        import fineness_sweep
        fineness_sweep.EPS = a.eps
        globals()["EPS"] = a.eps

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    if a.query:
        from band_map import QUERIES
        frm, pu, gexpr, where, val = QUERIES[a.query]
        for al, p in (("so", "stackoverflow_dba_sqlstorm.db"),
                      ("cb", "clickbench_micro.db")):
            if a.query.startswith(al):
                con.execute(f"ATTACH '/home/ila/Code/privacy/{p}' AS {al} (READ_ONLY)")
        c = AnyCells(con, frm, pu, gexpr, where, val)
        a.grouping, a.filter = a.query, where
    else:
        c = Cells(con, GROUPINGS[a.grouping][0], a.filter)
    if a.skew > 0:
        w = np.random.default_rng(7).lognormal(0.0, a.skew, size=c.K)
        c.val = c.val * w[c.gi]
        c.truth = np.bincount(c.gi, weights=c.val, minlength=c.K)
        c.norms = np.bincount(c.pi, weights=np.abs(c.val), minlength=c.P)
    q = np.quantile(c.truth, [.05, .5, .95])
    print(f"group-total skew: p5 {q[0]:,.3g}  p50 {q[1]:,.3g}  p95 {q[2]:,.3g}   "
          f"p95/p5 = {q[2]/q[0]:.1f}x")
    mk = int(c.k_u.max())
    cus = sorted({1, 2, 5, 10, 19, 30, mk // 2 or 1, mk})
    cache, r = build(c, cus, a.trials)
    print(f"{a.grouping}  WHERE {a.filter}  eps={EPS} delta={DELTA:g}  "
          f"{c.K:,} groups, {c.P:,} PUs, max k_u={mk}, "
          f"n_g/k_u={np.median(c.npu_g)/mk:.1f}\n")

    tuned = {}
    for kind in ("Laplace", "Gaussian"):
        tuned[kind] = tune(c, cache, cus, kind, mk, r)

    print(f"{'metric':<8}{'Laplace':>10}{'cfg':>18}{'Gaussian':>11}{'cfg':>18}{'gain':>8}")
    print("-" * 73)
    for k in METRICS:
        row = []
        for kind in ("Laplace", "Gaussian"):
            best, res = tuned[kind]
            cfg = best[k]
            row.append((res[cfg][k], f"C_e={cfg[0]},ev={cfg[1][2]:.2f}"))
        print(f"{k:<8}{row[0][0]:>10.4f}{row[0][1]:>18}{row[1][0]:>11.4f}{row[1][1]:>18}"
              f"{row[0][0]/row[1][0]:>7.2f}x")

    print("\nper-group detail at each arm's L1-optimal config:")
    cfgs = {}
    for kind in ("Laplace", "Gaussian"):
        cfg = tuned[kind][0]["L1"]
        cfgs[kind] = cfg
        outs = realise(c, cache, cfg[0], cfg[1], kind, mk, r)
        describe(c, outs, f"{kind} votes, C_e={cfg[0]}, split={tuple(round(x,3) for x in cfg[1])}")

    print("\nper-group detail at each arm's medRE-optimal config:")
    for kind in ("Laplace", "Gaussian"):
        cfg = tuned[kind][0]["medRE"]
        outs = realise(c, cache, cfg[0], cfg[1], kind, mk, r)
        describe(c, outs, f"{kind} votes, C_e={cfg[0]}, split={tuple(round(x,3) for x in cfg[1])}")

    # intersection of the two arms' released sets, at their own L1 optima
    ol = realise(c, cache, cfgs["Laplace"][0], cfgs["Laplace"][1], "Laplace", mk, r)
    og = realise(c, cache, cfgs["Gaussian"][0], cfgs["Gaussian"][1], "Gaussian", mk, r)
    el, eg, ns = [], [], []
    for (rl, l, _), (rg, g, _) in zip(ol, og):
        m = rl & rg
        if not m.any():
            continue
        ns.append(m.sum())
        el.append(np.abs(l[m] - c.truth[m]).sum() / c.truth[m].sum())
        eg.append(np.abs(g[m] - c.truth[m]).sum() / c.truth[m].sum())
    print(f"\nintersection of released sets: {np.mean(ns):,.0f} groups; "
          f"L1 there  Laplace {np.mean(el):.4f}  Gaussian {np.mean(eg):.4f}  "
          f"gain {np.mean(el)/np.mean(eg):.2f}x")
    print("  (same value mechanism in both arms, so this isolates the leftover value budget)")


if __name__ == "__main__":
    main()
