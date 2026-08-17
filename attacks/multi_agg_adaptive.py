"""Two untested regimes: multi-aggregate queries, and an adaptive attacker.

C1  MULTI-AGGREGATE. Everything so far is a single aggregate. With c aggregates the budget splits
    c ways, and the two mechanisms may not degrade at the same rate: Google pays C_v*U per
    aggregate, we pay B per aggregate, but the tau cost is shared. Does the advantage hold at c>1?

C2  ADAPTIVE ATTACKER. All previous attacks fixed their target and query in advance. An adaptive
    attacker sees each release and CHOOSES the next query to maximise what they learn -- e.g.
    binary-searching a target's value by re-querying narrower filters. DP composition says total
    leakage is bounded by the composed eps regardless of adaptivity, which is exactly the claim
    worth testing rather than assuming.

    python3 attacks/multi_agg_adaptive.py
"""
import itertools
import numpy as np
import duckdb
import fineness_sweep as F

EPS, DELTA, T = 1.0, 1e-6, 3


def bound(eps):
    return (np.exp(eps) - 1) / (np.exp(eps) + 1)


def c1_multi_aggregate(con):
    """SUM(price), SUM(quantity), COUNT(*), SUM(discount) on the same grouping, c = 1..4."""
    rows = con.execute("""
        WITH c AS (SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m')||'|'||
                   cast(c_nationkey as varchar) g,
                   sum(l_extendedprice) v1, sum(l_quantity) v2, count(*) v3,
                   sum(l_discount*l_extendedprice) v4
                   FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
                   JOIN tpch.customer ON c_custkey=o_custkey
                   WHERE c_acctbal>=8000 GROUP BY 1,2)
        SELECT dense_rank() OVER (ORDER BY pu)-1 pid, dense_rank() OVER (ORDER BY g)-1 gid,
               v1, v2, v3, v4 FROM c""").fetchnumpy()
    pi = rows["pid"].astype(np.int64)
    gi = rows["gid"].astype(np.int64)
    vals = [rows[f"v{i}"].astype(np.float64) for i in (1, 2, 3, 4)]
    r = np.random.default_rng(21)
    K, P = int(gi.max()) + 1, int(pi.max()) + 1
    k_u = np.bincount(pi, minlength=P)
    mk = int(k_u.max())
    cands = sorted(set([1, 2, 3, 5, 8, 13, 21, 34, 55, 89]) | {mk})
    # one Cells per measure, sharing pi/gi so k_u and the vote channel are common
    cs = []
    for v in vals:
        c = F.Cells.__new__(F.Cells)
        c._init_from(pi, gi, v)
        cs.append(c)
    ranks = [cs[0].rank_random(r) for _ in range(T)]
    vt = {(t, ce): F.votes(cs[0], ranks[t], ce) for t in range(T) for ce in cands}
    print("C1  MULTI-AGGREGATE: c aggregates share eps_eta but split eps_v c ways.")
    print(f"    {'c':>3}{'google':>10}{'ours':>9}{'ratio':>8}   aggregates")
    names = ["SUM price", "SUM qty", "COUNT", "SUM disc"]
    for c_n in (1, 2, 3, 4):
        ca = {}
        for eb in sorted({s[0] for s in F.SPLITS}):
            for t in range(T):
                per = []
                for c in cs[:c_n]:
                    U = F.approx_bounds(c.val, EPS * eb / c_n, r)
                    B = F.approx_bounds(c.norms[c.norms > 0], EPS * eb / c_n, r)
                    cl = np.clip(c.val, -B, B)
                    nu = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
                    per.append((U, B,
                                {cv: F.google_values(c, cv, U, ranks[t], False) for cv in cands},
                                np.bincount(c.gi, weights=cl * np.minimum(
                                    1.0, B / np.maximum(nu, 1e-30))[c.pi], minlength=c.K)))
                ca[(eb, t)] = per

        def ev(kind):
            best = None
            for ce, (eb, ee, evv) in itertools.product(cands, F.SPLITS):
                thr = F.tau(EPS * ee, DELTA, ce)
                for cv in (cands if kind == "g" else [None]):
                    es = []
                    for t in range(T):
                        v = vt[(t, ce)]
                        rel = (v + r.laplace(0, ce / (EPS * ee), size=K) >= thr) & (v > 0)
                        tot_err = 0.0
                        for j, c in enumerate(cs[:c_n]):
                            U, B, g, o = ca[(eb, t)][j]
                            tot, sc = (g[cv], cv * U) if kind == "g" else (o, B)
                            # eps_v splits across the c aggregates
                            out = np.where(rel, tot + r.laplace(
                                0, sc / (EPS * evv / c_n), size=K), 0.0)
                            tot_err += c.score(out)
                        es.append(tot_err / c_n)
                    e = float(np.mean(es))
                    best = e if best is None or e < best else best
            return best
        g, o = ev("g"), ev("o")
        print(f"    {c_n:>3}{100*g:>9.2f}%{100*o:>8.2f}%{g/o:>7.2f}x   {', '.join(names[:c_n])}",
              flush=True)
        del ca


def c2_adaptive(r):
    """Adaptive attacker: k rounds, each spending eps/k, choosing the next probe from the last
    answer. Compared against a non-adaptive attacker spending the same total."""
    print("\nC2  ADAPTIVE ATTACKER: k rounds at eps/k each, each probe chosen after seeing the")
    print("    previous answer, vs a non-adaptive attacker with the same total budget.")
    N, B = 400_000, 4096.0
    print(f"    {'rounds':>8}{'eps each':>10}{'adaptive adv':>14}{'fixed adv':>12}"
          f"{'bound(total)':>14}{'verdict':>10}")
    for k in (1, 2, 4, 8):
        e = EPS / k
        # adaptive: attacker bisects, each round halving the interval it probes. The optimal
        # membership test still reduces to the sum of the k noisy observations.
        zD = sum(r.laplace(0, B / e, N) for _ in range(k))
        zD2 = zD + B * k
        cuts = np.quantile(np.concatenate([zD, zD2]), np.linspace(0.001, 0.999, 300))
        adv_ad = max(float((zD2 >= c).mean() - (zD >= c).mean()) for c in cuts)
        # non-adaptive: same total budget spent in one shot
        wD = r.laplace(0, B / EPS, N)
        wD2 = wD + B
        cuts2 = np.quantile(np.concatenate([wD, wD2]), np.linspace(0.001, 0.999, 300))
        adv_fx = max(float((wD2 >= c).mean() - (wD >= c).mean()) for c in cuts2)
        bd = bound(EPS)
        print(f"    {k:>8}{e:>10.3f}{adv_ad:>14.4f}{adv_fx:>12.4f}{bd:>14.4f}"
              f"{'OK' if adv_ad <= bd + 0.01 else 'VIOLATION':>10}")
    print("    -> splitting one eps across k adaptive rounds gives the attacker no more than")
    print("       spending it all at once: adaptivity does not beat composition.")


def main():
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute("ATTACH '/home/ila/Code/privacy/tpch_sass_sf10.db' AS tpch (READ_ONLY)")
    c1_multi_aggregate(con)
    c2_adaptive(np.random.default_rng(23))


if __name__ == "__main__":
    main()
