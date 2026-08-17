"""Is the broad benchmark's 1.88x median an artifact of eps = 1?

An earlier finding: on month|nation the gain is 3.31x at eps=0.25 but 1.26x at eps=1 -- a 2.6x
swing on one query. Every headline in this document is measured at eps=1, so this re-runs a
representative subset across eps to see whether the ranking or the magnitude depends on it.

    python3 attacks/eps_sweep_benchmark.py
"""
import itertools
import numpy as np
import duckdb
import fineness_sweep as F
from broad_benchmark import Q, load

T, EPSES = 2, (0.25, 0.5, 1.0, 2.0, 4.0)
PICK = ("tpch SUM price / mo|nation", "tpch SUM price / mo|prio", "tpch COUNT / mo|nation",
        "so COUNT posts / month", "cb COUNT hits / region")


def main():
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    for al, p in (("tpch", "tpch_sass_sf10.db"), ("so", "stackoverflow_dba_sqlstorm.db"),
                  ("cb", "clickbench_micro.db")):
        con.execute(f"ATTACH '/home/ila/Code/privacy/{p}' AS {al} (READ_ONLY)")
    r = np.random.default_rng(5)
    print("google-best (its own optimal C_u) vs ours, swept over eps.\n")
    print(f"{'query':<28}" + "".join(f"{'eps='+str(e):>12}" for e in EPSES))
    print("-" * (28 + 12 * len(EPSES)))
    allr = {e: [] for e in EPSES}
    for name in PICK:
        body, where = Q[name]
        c, _ = load(con, body, where)
        if len(c.val) > 6_000_000:
            print(f"{name:<28} skipped")
            del c
            continue
        mk = int(c.k_u.max())
        cands = sorted(set([1, 2, 3, 5, 8, 13, 21, 34, 55, 89]) | {mk})
        ranks = [c.rank_random(r) for _ in range(T)]
        vt = {(t, ce): F.votes(c, ranks[t], ce) for t in range(T) for ce in cands}
        cells = ""
        for EPS in EPSES:
            ca = {}
            for eb in sorted({s[0] for s in F.SPLITS}):
                for t in range(T):
                    U = F.approx_bounds(c.val, EPS * eb, r)
                    B = F.approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
                    cl = np.clip(c.val, -B, B)
                    nu = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
                    ca[(eb, t)] = (U, B,
                                   {cv: F.google_values(c, cv, U, ranks[t], False)
                                    for cv in cands},
                                   np.bincount(c.gi, weights=cl * np.minimum(
                                       1.0, B / np.maximum(nu, 1e-30))[c.pi], minlength=c.K))

            def ev(kind):
                best = None
                for ce, (eb, ee, evv) in itertools.product(cands, F.SPLITS):
                    thr = F.tau(EPS * ee, F.DELTA, ce)
                    for cv in (cands if kind == "g" else [None]):
                        es = []
                        for t in range(T):
                            U, B, g, o = ca[(eb, t)]
                            v = vt[(t, ce)]
                            rel = (v + r.laplace(0, ce / (EPS * ee), size=c.K) >= thr) & (v > 0)
                            tot, sc = (g[cv], cv * U) if kind == "g" else (o, B)
                            es.append(c.score(np.where(rel, tot + r.laplace(
                                0, sc / (EPS * evv), size=c.K), 0.0)))
                        e = float(np.mean(es))
                        best = e if best is None or e < best else best
                return best
            g, o = ev("g"), ev("o")
            allr[EPS].append(g / o)
            cells += f"{g/o:>11.2f}x"
            del ca
        print(f"{name:<28}{cells}", flush=True)
        del c
    print("-" * (28 + 12 * len(EPSES)))
    print(f"{'median':<28}" + "".join(f"{np.median(allr[e]):>11.2f}x" for e in EPSES))
    print(f"{'min':<28}" + "".join(f"{min(allr[e]):>11.2f}x" for e in EPSES))


if __name__ == "__main__":
    main()
