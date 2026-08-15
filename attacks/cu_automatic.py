"""Does C_u need to be estimated privately -- and is it stable enough to freeze?

Dandan's point: a fully automatic system must choose C_u (max groups contributed) without the
analyst, and since C_u is "relatively stable" it could be frozen like the group set.

Three things to check, because the answer differs per channel:

  VOTE channel.   For Laplace tau, tau(C)/C is monotone increasing in C, so C_e = 1 is EXACTLY
                  optimal -- proven, not measured. No estimation is needed at all. And with a
                  frozen group set there is no vote channel to parameterise in the first place.
  VALUE, Google.  C_v trades truncation bias against C_v*U noise, so it must be chosen, and badly
                  choosing it is expensive. This is where her question bites.
  VALUE, ours.    The l1 norm clip has NO C_v parameter: sensitivity is B regardless of how many
                  groups a PU touches. The parameter does not exist to be estimated.

So the experiment is: (a) is Google's optimal C_v stable across filters, i.e. is freezing it even
sound? (b) what does misspecifying it cost? (c) confirm ours is flat.

    python3 attacks/cu_automatic.py
"""

import argparse
import itertools

import numpy as np

from fineness_sweep import (DELTA, GROUPINGS, SPLITS, Cells, approx_bounds, google_values, tau,
                            votes)

FILTERS = {
    "none": "true",
    "acctbal>=4000": "c_acctbal>=4000",
    "acctbal>=8000": "c_acctbal>=8000",
    "acctbal>=9500": "c_acctbal>=9500",
    "mktseg=AUTOMOBILE": "c_mktsegment='AUTOMOBILE'",
    "nation<5": "c_nationkey<5",
}


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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--grouping", default="month|nation")
    ap.add_argument("--eps", type=float, default=1.0)
    ap.add_argument("--trials", type=int, default=3)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    gexpr = GROUPINGS[a.grouping][0]
    EPS = a.eps
    CVS = [1, 2, 3, 5, 8, 13, 21, 34, 55, 89]
    print(f"{a.grouping}, eps={EPS}. C_v grid {CVS}\n")
    print("(a) Is Google's optimal C_v stable across filters -- i.e. is freezing one value sound?")
    print(f"    {'filter':<20}{'PUs':>9}{'k_u med':>9}{'k_u max':>9}{'best C_v':>10}"
          f"{'error':>9}{'ours':>9}")
    print("    " + "-" * 71)
    rows = {}
    for fname, filt in FILTERS.items():
        c = load(con, gexpr, filt)
        mk = int(c.k_u.max())
        r = np.random.default_rng(99)
        ranks = [c.rank_random(r) for _ in range(a.trials)]
        cus = sorted(set(CVS) | {mk})
        vt = {(t, ce): votes(c, ranks[t], ce) for t in range(a.trials) for ce in cus}
        cache = {}
        for eb in sorted({s[0] for s in SPLITS}):
            for t in range(a.trials):
                U = approx_bounds(c.val, EPS * eb, r)
                B = approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
                cl = np.clip(c.val, -B, B)
                n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
                cache[(eb, t)] = (U, B, np.bincount(
                    c.gi, weights=cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi],
                    minlength=c.K), {cv: google_values(c, cv, U, ranks[t], False) for cv in cus})

        def run(arm, ce, cv, split):
            eb, ee, ev = split
            thr = tau(EPS * ee, DELTA, ce)
            es = []
            for t in range(a.trials):
                U, B, o_tot, g_tot = cache[(eb, t)]
                v = vt[(t, ce)]
                rel = (v + r.laplace(0, ce / (EPS * ee), size=c.K) >= thr) & (v > 0)
                tot, sc = (g_tot[cv], cv * U) if arm == "goog" else (o_tot, B)
                es.append(c.score(np.where(rel, tot + r.laplace(0, sc / (EPS * ev), size=c.K), 0.0)))
            return float(np.mean(es))

        per_cv = {cv: min(run("goog", ce, cv, s) for ce, s in itertools.product(cus, SPLITS))
                  for cv in CVS}
        best_cv = min(per_cv, key=per_cv.get)
        ours = min(run("ours", ce, None, s) for ce, s in itertools.product(cus, SPLITS))
        rows[fname] = (per_cv, best_cv, ours)
        print(f"    {fname:<20}{c.P:>9,}{np.median(c.k_u):>9.0f}{mk:>9}{best_cv:>10}"
              f"{100*per_cv[best_cv]:>8.2f}%{100*ours:>8.2f}%", flush=True)
        del c, cache

    opts = [v[1] for v in rows.values()]
    print(f"\n    optimal C_v ranges {min(opts)}-{max(opts)} across these filters "
          f"({max(opts)/max(min(opts),1):.0f}x spread) -- "
          f"{'STABLE, freezing is sound' if max(opts) <= 2*min(opts) else 'NOT stable'}")

    print("\n(b) What does misspecifying C_v cost? (penalty vs that filter's own best C_v)")
    hdr = f"    {'filter':<20}" + "".join(f"{'C_v='+str(cv):>8}" for cv in CVS)
    print(hdr)
    print("    " + "-" * (len(hdr) - 4))
    for fname, (per_cv, best_cv, ours) in rows.items():
        b = per_cv[best_cv]
        print(f"    {fname:<20}" + "".join(f"{per_cv[cv]/b:>7.1f}x" for cv in CVS))
    print("\n(c) Ours has no C_v at all -- the l1 norm clip gives sensitivity B regardless of how")
    print("    many groups a PU touches, so there is no parameter to estimate, freeze, or get")
    print("    wrong. For the vote channel C_e = 1 is provably optimal, and with a frozen group")
    print("    set there is no vote channel either.")


if __name__ == "__main__":
    main()
