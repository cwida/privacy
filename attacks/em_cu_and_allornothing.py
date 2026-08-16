"""Dandan's two 15 Aug proposals, evaluated.

(1) DP SELECTION OF A FROZEN C_u VIA THE EXPONENTIAL MECHANISM.
    Her motivation is exactly right and matches the measurement in cu_automatic.py: setting C_u too
    small clips many PUs and costs up to 38x, while setting it too large costs at most 1.7x. So the
    selection should be deliberately biased high.
    Implemented as an exponential mechanism over candidate bounds with a SENSITIVITY-1 score --
    score(C) = -|{u : k_u > C}| - lambda*C, where the first term counts truncated PUs (one PU
    changes it by at most 1) and lambda is a public noise-penalty weight. EM is then eps-DP by
    McSherry-Talwar with Pr[C] ~ exp(eps*score(C)/(2*Delta)), Delta = 1.
    Question: does the EM-selected C_u land near the error optimum, and what does it cost?

(2) ALL-OR-FROZEN HISTOGRAM THRESHOLDING.
    Replace K per-group tests with ONE aggregate test: if every group clears the threshold, release
    the query-specific key set; otherwise fall back to the frozen set. The test is on
    S = |{g : n_g < T}|, whose sensitivity is C_u (a PU touches at most C_u groups), so it is one
    noisy comparison rather than K.
    The question that decides it is empirical: how often does "every group clears T" actually hold?
    If a single small group forces fallback, the rule almost never fires.

    python3 attacks/em_cu_and_allornothing.py
"""

import argparse
import itertools

import numpy as np

from fineness_sweep import (DELTA, GROUPINGS, SPLITS, Cells, approx_bounds, google_values, tau,
                            votes)

FILTERS = {"none": "true", "acctbal>=4000": "c_acctbal>=4000",
           "acctbal>=8000": "c_acctbal>=8000", "nation<5": "c_nationkey<5"}


def load(con, gexpr, filt):
    rows = con.execute(f"""
        WITH c AS (SELECT o_custkey AS pu, {gexpr} AS g, sum(l_extendedprice) AS t
                   FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
                   JOIN tpch.customer ON c_custkey=o_custkey
                   JOIN tpch.nation ON n_nationkey=c_nationkey
                   WHERE {filt} GROUP BY 1,2)
        SELECT dense_rank() OVER (ORDER BY pu)-1 AS pid,
               dense_rank() OVER (ORDER BY g)-1 AS gid, t FROM c""").fetchnumpy()
    c = Cells.__new__(Cells)
    c._init_from(rows["pid"].astype(np.int64), rows["gid"].astype(np.int64),
                 rows["t"].astype(np.float64))
    return c


def em_select_cu(k_u, cands, eps, lam, r):
    """Exponential mechanism over candidate bounds. score(C) = -truncated_PUs(C) - lam*C, which
    has sensitivity 1 in the add/remove-one-PU relation, so Pr[C] ~ exp(eps*score/2)."""
    trunc = np.array([float(np.sum(k_u > C)) for C in cands])
    score = -trunc - lam * np.asarray(cands, dtype=float)
    w = np.exp(eps * (score - score.max()) / 2.0)
    return int(cands[r.choice(len(cands), p=w / w.sum())])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--grouping", default="month|nation")
    ap.add_argument("--eps", type=float, default=1.0)
    ap.add_argument("--eps-cu", type=float, default=0.05, help="budget for the EM selection")
    ap.add_argument("--trials", type=int, default=3)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    gexpr = GROUPINGS[a.grouping][0]
    EPS = a.eps
    CVS = [1, 2, 3, 5, 8, 13, 21, 34, 55, 89]
    r = np.random.default_rng(99)

    print(f"{a.grouping}, eps={EPS}, EM budget eps_cu={a.eps_cu}\n")
    print("(1) EM-selected C_u vs the error optimum. lambda is the public noise-penalty weight;")
    print("    larger lambda pushes the selection lower.")
    print(f"    {'filter':<16}{'k_u max':>9}{'EM C_u':>25}{'best C_v':>10}{'err@EM':>9}"
          f"{'err@best':>10}{'ours':>8}")
    print(f"    {'':16}{'':9}{'(lam=0.001/0.01/0.1)':>25}")
    print("    " + "-" * 87)
    for fname, filt in FILTERS.items():
        c = load(con, gexpr, filt)
        mk = int(c.k_u.max())
        cands = sorted(set(CVS) | {mk})
        ranks = [c.rank_random(r) for _ in range(a.trials)]
        vt = {(t, ce): votes(c, ranks[t], ce) for t in range(a.trials) for ce in cands}
        cache = {}
        for eb in sorted({s[0] for s in SPLITS}):
            for t in range(a.trials):
                U = approx_bounds(c.val, EPS * eb, r)
                B = approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
                cl = np.clip(c.val, -B, B)
                n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
                cache[(eb, t)] = (U, B, np.bincount(
                    c.gi, weights=cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi],
                    minlength=c.K), {cv: google_values(c, cv, U, ranks[t], False) for cv in cands})

        def run(arm, ce, cv, split, e_q=None):
            eb, ee, ev = split
            e_q = e_q or EPS
            thr = tau(e_q * ee, DELTA, ce)
            es = []
            for t in range(a.trials):
                U, B, o_tot, g_tot = cache[(eb, t)]
                v = vt[(t, ce)]
                rel = (v + r.laplace(0, ce / (e_q * ee), size=c.K) >= thr) & (v > 0)
                tot, sc = (g_tot[cv], cv * U) if arm == "goog" else (o_tot, B)
                es.append(c.score(np.where(rel, tot + r.laplace(0, sc / (e_q * ev), size=c.K), 0.0)))
            return float(np.mean(es))

        per_cv = {cv: min(run("goog", ce, cv, s) for ce, s in itertools.product(cands, SPLITS))
                  for cv in CVS}
        best_cv = min(per_cv, key=per_cv.get)
        ours = min(run("ours", ce, None, s) for ce, s in itertools.product(cands, SPLITS))
        picks, errs = [], []
        for lam in (0.001, 0.01, 0.1):
            sel = [em_select_cu(c.k_u, cands, a.eps_cu, lam * c.P / max(cands), r)
                   for _ in range(9)]
            m = int(np.median(sel))
            picks.append(m)
            # error at the EM pick, with the remaining budget
            errs.append(min(run("goog", ce, min(CVS, key=lambda x: abs(x - m)), s, EPS - a.eps_cu)
                            for ce, s in itertools.product(cands, SPLITS)))
        print(f"    {fname:<16}{mk:>9}{'/'.join(str(p) for p in picks):>25}{best_cv:>10}"
              f"{100*min(errs):>8.2f}%{100*per_cv[best_cv]:>9.2f}%{100*ours:>7.2f}%", flush=True)

        if fname == "acctbal>=8000":
            print("\n(2) All-or-nothing: how often does EVERY group clear the threshold?")
            n_g = c.npu_g
            print(f"    {a.grouping} under {fname}: {c.K:,} groups, "
                  f"min {n_g.min():,.0f}, p1 {np.percentile(n_g,1):,.0f}, "
                  f"median {np.median(n_g):,.0f}")
            for ee in (0.2, 0.4, 0.6):
                T = tau(EPS * ee, DELTA, 1)
                frac = float(np.mean(n_g >= T))
                print(f"      eps_eta={ee}: tau={T:>6.1f}  groups clearing it "
                      f"{100*frac:>6.2f}%   ALL clear? {'yes' if frac == 1.0 else 'NO'}")
        del c, cache


if __name__ == "__main__":
    main()
