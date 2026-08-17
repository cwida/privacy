"""Attack the broad benchmark: per-group metrics, and Google with every legal improvement.

Two attacks on the median-1.90x headline.

ATTACK 1 -- THE METRIC. Every number in this document is relative total L1. That metric was shown
earlier to report 8.6% error for a release answering 7% of a key set, so it can flatter whichever
arm concentrates its accuracy on the largest groups. Re-scored here under:
  total-L1       sum|out-truth| / sum|truth|              (the incumbent)
  mean-rel       mean over groups of |out-truth|/max(truth,1)   (equal weight per group)
  median-rel     median of the same, robust to a few bad groups
  nRMSE          sqrt(mean (out-truth)^2) / mean(truth)

ATTACK 2 -- A MAXIMALLY STRONG GOOGLE. Give it every DP-legal improvement found in this project:
  top-C_v        keep each PU's largest cells instead of a random C_v
  rescale        multiply kept cells by total_u/kept_u then RE-CLIP to U (fills the C_v*U budget
                 it already pays for; verified to preserve Delta_1 exactly)
  decoupled      separate C_e for votes and C_v for values
None of these is in Wilson et al. or the library, so this is not "Google DP" -- it is the strongest
Google-shaped mechanism I know how to build, and the right thing to report as a robustness check.

    python3 attacks/attack_benchmark.py
"""
import itertools
import numpy as np
import duckdb
import fineness_sweep as F
from broad_benchmark import Q, load, build_gfix

EPS, T = 1.0, 3
PICK = ("tpch SUM price / mo|nation", "tpch SUM price / mo|prio", "tpch COUNT / mo|nation",
        "tpch SUM price / day", "so COUNT posts / month", "so SUM score / month",
        "cb COUNT hits / region", "cb COUNT hits / date|reg")


def metrics(out, truth):
    d = np.abs(out - truth)
    den = np.maximum(np.abs(truth), 1.0)
    return {"total-L1": d.sum() / max(np.abs(truth).sum(), 1e-9),
            "mean-rel": float(np.mean(d / den)),
            "median-rel": float(np.median(d / den)),
            "nRMSE": float(np.sqrt(np.mean(d ** 2)) / max(np.mean(np.abs(truth)), 1e-9))}


def main():
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    for al, p in (("tpch", "tpch_sass_sf10.db"), ("so", "stackoverflow_dba_sqlstorm.db"),
                  ("cb", "clickbench_micro.db")):
        con.execute(f"ATTACH '/home/ila/Code/privacy/{p}' AS {al} (READ_ONLY)")
    r = np.random.default_rng(11)
    MS = ["total-L1", "mean-rel", "median-rel", "nRMSE"]
    print(f"eps={EPS}, {T} trials. 'published' = one C_u, Laplace, random truncation.")
    print("'strong' = published + top-C_v + rescale-with-re-clip + decoupled C_e/C_v.")
    print("Each arm is tuned SEPARATELY under each metric.\n")
    hdr = f"{'query':<26}{'metric':<12}{'published':>11}{'strong':>9}{'ours':>8}{'vs pub':>8}{'vs strong':>11}"
    print(hdr)
    print("-" * len(hdr))
    agg = {m: {"pub": [], "str": []} for m in MS}
    for name in PICK:
        body, where = Q[name]
        c, _ = load(con, body, where)
        if len(c.val) > 6_000_000:
            print(f"{name:<26}  skipped")
            del c
            continue
        mk = int(c.k_u.max())
        cands = sorted(set([1, 2, 3, 5, 8, 13, 21, 34, 55, 89]) | {mk})
        ranks = [c.rank_random(r) for _ in range(T)]
        vt = {(t, ce): F.votes(c, ranks[t], ce) for t in range(T) for ce in cands}
        ca = {}
        for eb in sorted({s[0] for s in F.SPLITS}):
            for t in range(T):
                U = F.approx_bounds(c.val, EPS * eb, r)
                B = F.approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
                cl = np.clip(c.val, -B, B)
                nu = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
                ca[(eb, t)] = (U, B,
                               {cv: F.google_values(c, cv, U, ranks[t], False) for cv in cands},
                               {cv: F.google_values(c, cv, U, c.rank_top, True) for cv in cands},
                               np.bincount(c.gi, weights=cl * np.minimum(
                                   1.0, B / np.maximum(nu, 1e-30))[c.pi], minlength=c.K))

        def run(arm, ce, cv, split):
            eb, ee, ev = split
            thr = F.tau(EPS * ee, F.DELTA, ce)
            acc = {m: [] for m in MS}
            for t in range(T):
                U, B, g_rand, g_res, o = ca[(eb, t)]
                v = vt[(t, ce)]
                rel = (v + r.laplace(0, ce / (EPS * ee), size=c.K) >= thr) & (v > 0)
                if arm == "published":
                    tot, sc = g_rand[cv], cv * U
                elif arm == "strong":
                    tot, sc = g_res[cv], cv * U
                else:
                    tot, sc = o, B
                out = np.where(rel, tot + r.laplace(0, sc / (EPS * ev), size=c.K), 0.0)
                for m, val in metrics(out, c.truth).items():
                    acc[m].append(val)
            return {m: float(np.mean(vs)) for m, vs in acc.items()}

        best = {a: {} for a in ("published", "strong", "ours")}
        for arm in best:
            cvs = cands if arm != "ours" else [None]
            # published couples C_e = C_v; strong decouples; ours has no C_v
            combos = ([(cu, cu, s) for cu, s in itertools.product(cands, F.SPLITS)]
                      if arm == "published" else
                      [(ce, cv, s) for ce, cv, s in itertools.product(cands, cvs, F.SPLITS)])
            for m in MS:
                best[arm][m] = min(run(arm, ce, cv, s)[m] for ce, cv, s in combos)
        for m in MS:
            p_, s_, o_ = best["published"][m], best["strong"][m], best["ours"][m]
            agg[m]["pub"].append(p_ / o_)
            agg[m]["str"].append(s_ / o_)
            lbl = name if m == MS[0] else ""
            print(f"{lbl:<26}{m:<12}{100*p_:>10.2f}%{100*s_:>8.2f}%{100*o_:>7.2f}%"
                  f"{p_/o_:>7.2f}x{s_/o_:>10.2f}x", flush=True)
        print()
        del c, ca
    print("=" * len(hdr))
    for m in MS:
        print(f"{'MEDIAN across queries':<26}{m:<12}{'':>10}{'':>8}{'':>7}"
              f"{np.median(agg[m]['pub']):>7.2f}x{np.median(agg[m]['str']):>10.2f}x")


if __name__ == "__main__":
    main()
