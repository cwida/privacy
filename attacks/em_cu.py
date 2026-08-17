"""Dandan's section 1: DP selection of the contribution bound C_u by exponential mechanism.

Her mechanism:
  1. release Ntilde = N + Lap(1/eps_N)                                    (eps_N, 0)-DP
  2. conservative target rank  T = p*Ntilde + (p/eps_N) ln(1/(2 beta_N))
  3. score each candidate r by q(d,r) = -|F_d(r) - T|, F_d(r) = |{u : k_u <= r}|
  4. sample r with Pr ~ exp(eps_C q(d,r) / 2), since Delta_q <= 1          (eps_C, 0)-DP
  5. freeze the selected Chat_u as metadata

Three questions, in increasing order of how much they matter:

  A. Is Delta_q <= 1 correct? Her argument is that one PU shifts K-k+1 cumulative counts at once,
     so RELEASING the vector would have l1 sensitivity up to K -- but the EM never releases it, and
     each individual candidate's score moves by at most 1. Verified here by neighbour construction.
  B. Does the EM actually land on the p-quantile, and at what eps_C?
  C. IS THE p-QUANTILE THE RIGHT TARGET? This is the real question. cu_automatic.py found the
     error-optimal C_v ranging 21-55 across filters while median k_u sat flat at 30 -- so the
     statistic is stable but the optimum is not. If the quantile rule lands far from the optimum,
     selecting it accurately does not help.

    python3 attacks/em_cu.py
"""

import argparse
import itertools

import numpy as np

from fineness_sweep import (DELTA, GROUPINGS, SPLITS, Cells, approx_bounds, google_values, tau,
                            votes)

FILTERS = {"acctbal>=8000": "c_acctbal>=8000", "acctbal>=9500": "c_acctbal>=9500",
           "mktseg=AUTOMOBILE": "c_mktsegment='AUTOMOBILE'", "acctbal<0": "c_acctbal<0"}


def load(con, gexpr, filt):
    rows = con.execute(f"""
        WITH c AS (SELECT o_custkey AS pu, {gexpr} AS g, sum(l_extendedprice) AS t
                   FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
                   JOIN tpch.customer ON c_custkey=o_custkey
                   WHERE {filt} GROUP BY 1,2)
        SELECT dense_rank() OVER (ORDER BY pu)-1 AS pid,
               dense_rank() OVER (ORDER BY g)-1 AS gid, t FROM c""").fetchnumpy()
    c = Cells.__new__(Cells)
    c._init_from(rows["pid"].astype(np.int64), rows["gid"].astype(np.int64),
                 rows["t"].astype(np.float64))
    return c


def em_select(k_u, K, p, eps_n, eps_c, beta_n, r):
    """Her Algorithm 1, verbatim."""
    N = len(k_u)
    n_tilde = N + r.laplace(0, 1.0 / eps_n)
    T = p * n_tilde + (p / eps_n) * np.log(1.0 / (2.0 * beta_n))
    cands = np.arange(1, K + 1)
    F = np.cumsum(np.bincount(np.clip(k_u, 1, K), minlength=K + 1)[1:])
    q = -np.abs(F - T)                       # Delta_q <= 1
    w = np.exp(eps_c * (q - q.max()) / 2.0)
    return int(cands[r.choice(len(cands), p=w / w.sum())]), F, T


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--grouping", default="month|nation")
    ap.add_argument("--eps", type=float, default=1.0)
    ap.add_argument("--eps-n", type=float, default=0.01)
    ap.add_argument("--eps-c", type=float, default=0.04)
    ap.add_argument("--beta-n", type=float, default=0.01)
    ap.add_argument("--trials", type=int, default=2)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    gexpr = GROUPINGS[a.grouping][0]
    EPS = a.eps
    r = np.random.default_rng(99)

    # ---- A. is Delta_q <= 1 correct? neighbour construction, worst case k=1 ----
    print("A. SENSITIVITY OF THE SCORE. Her claim: releasing (F(1)..F(K)) would have l1 sensitivity")
    print("   up to K, but max_r |q(d,r) - q(d',r)| <= 1 because the EM scores each candidate")
    print("   separately. Removing one PU with k_u = k shifts K-k+1 cumulative counts at once:\n")
    K_pub = 80
    demo = np.array([1, 3, 3, 7, 12, 30, 30, 55, 70])
    for k in (1, 30, 70):
        d2 = np.delete(demo, np.where(demo == k)[0][0])
        F1 = np.cumsum(np.bincount(np.clip(demo, 1, K_pub), minlength=K_pub + 1)[1:])
        F2 = np.cumsum(np.bincount(np.clip(d2, 1, K_pub), minlength=K_pub + 1)[1:])
        T = 0.7 * len(demo)
        q1, q2 = -np.abs(F1 - T), -np.abs(F2 - T)
        print(f"   remove a PU with k_u={k:>2}: cumulative counts changed = "
              f"{int((F1 != F2).sum()):>2} of {K_pub}, l1 change = {int(np.abs(F1-F2).sum()):>2}, "
              f"but max_r |dq| = {np.abs(q1 - q2).max():.1f}")
    print("   -> her Delta_q <= 1 holds; the l1 of the histogram is a red herring, as she says.\n")

    # ---- B and C ----
    print(f"B/C. Does the p-quantile rule find a GOOD C_u? {a.grouping}, eps={EPS}, "
          f"EM budget eps_N={a.eps_n} + eps_C={a.eps_c}")
    print(f"     {'filter':<20}{'k_u p50':>8}{'p70':>6}{'p90':>6}{'p99':>6}"
          f"{'EM pick':>9}{'best C_v':>10}{'err@EM':>9}{'err@best':>10}{'penalty':>9}")
    print("     " + "-" * 95)
    CVS = [1, 2, 3, 5, 8, 13, 21, 34, 55, 89]
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
                cache[(eb, t)] = (U, {cv: google_values(c, cv, U, ranks[t], False)
                                      for cv in cands})

        def err(cv, eq):
            best = None
            for ce, (eb, ee, ev) in itertools.product(cands, SPLITS):
                thr = tau(eq * ee, DELTA, ce)
                es = []
                for t in range(a.trials):
                    U, g = cache[(eb, t)]
                    v = vt[(t, ce)]
                    rel = (v + r.laplace(0, ce / (eq * ee), size=c.K) >= thr) & (v > 0)
                    es.append(c.score(np.where(rel, g[cv] + r.laplace(
                        0, cv * U / (eq * ev), size=c.K), 0.0)))
                e = float(np.mean(es))
                best = e if best is None or e < best else best
            return best

        per_cv = {cv: err(cv, EPS) for cv in CVS}
        best_cv = min(per_cv, key=per_cv.get)
        picks = [em_select(c.k_u, mk, 0.7, a.eps_n, a.eps_c, a.beta_n, r)[0] for _ in range(15)]
        pick = int(np.median(picks))
        # EM budget comes out of the query's own eps
        e_em = err(min(CVS, key=lambda x: abs(x - pick)), EPS - a.eps_n - a.eps_c)
        qs = [int(np.quantile(c.k_u, x)) for x in (0.5, 0.7, 0.9, 0.99)]
        print(f"     {fname:<20}{qs[0]:>8}{qs[1]:>6}{qs[2]:>6}{qs[3]:>6}"
              f"{pick:>9}{best_cv:>10}{100*e_em:>8.2f}%{100*per_cv[best_cv]:>9.2f}%"
              f"{e_em/per_cv[best_cv]:>8.2f}x", flush=True)
        del c, cache
    print("\n     'penalty' = error using the EM-selected C_u (and paying for the EM) over the")
    print("     error at that filter's own best C_v. 1.00x would mean the quantile rule is optimal.")


if __name__ == "__main__":
    main()
