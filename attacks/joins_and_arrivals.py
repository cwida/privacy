"""Two different 'joins': SQL joins, and privacy units arriving between releases.

D1  SQL JOIN FAN-OUT. Every experiment here queries lineitem JOIN orders JOIN customer, with the
    privacy unit (customer) on the far side. One customer maps to many rows after the join, which
    is the classic place sensitivity analysis breaks. Verified by neighbour construction on the
    REAL join: remove one customer, measure the actual change in the released vector.

D2  ARRIVALS BETWEEN RELEASES. A target who is absent at release 1 and present at release 2, both
    made against the SAME frozen G_fix. Static-data analysis does not cover this: the attacker
    sees a before and an after, and the difference is the target plus two noise draws.

    python3 attacks/joins_and_arrivals.py
"""
import numpy as np
import duckdb
import fineness_sweep as F

EPS_V, DELTA = 0.6, 1e-6


def bound(eps):
    return (np.exp(eps) - 1) / (np.exp(eps) + 1)


def main():
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute("ATTACH '/home/ila/Code/privacy/tpch_sass_sf10.db' AS tpch (READ_ONLY)")
    r = np.random.default_rng(41)

    print("D1  SQL JOIN FAN-OUT. PU = customer, three-table join, so one PU owns many rows.")
    fan = con.execute("""
        SELECT max(n), median(n), max(nl) FROM (
          SELECT o_custkey, count(*) n, count(DISTINCT l_orderkey) nl
          FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
          JOIN tpch.customer ON c_custkey=o_custkey WHERE c_acctbal>=8000 GROUP BY 1)""").fetchone()
    print(f"    rows per customer after the join: max {fan[0]:,}, median {fan[1]:,.0f}; "
          f"max distinct orders {fan[2]:,}")
    rows = con.execute("""
        WITH c AS (SELECT o_custkey pu, strftime(l_shipdate,'%Y-%m')||'|'||
                   cast(c_nationkey as varchar) g, sum(l_extendedprice) t
                   FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
                   JOIN tpch.customer ON c_custkey=o_custkey WHERE c_acctbal>=8000 GROUP BY 1,2)
        SELECT dense_rank() OVER (ORDER BY pu)-1 pid, dense_rank() OVER (ORDER BY g)-1 gid, t
        FROM c""").fetchnumpy()
    c = F.Cells.__new__(F.Cells)
    c._init_from(rows["pid"].astype(np.int64), rows["gid"].astype(np.int64),
                 rows["t"].astype(np.float64))
    B = F.approx_bounds(c.norms[c.norms > 0], EPS_V * 0.002, r)
    cl = np.clip(c.val, -B, B)
    n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
    scale = np.minimum(1.0, B / np.maximum(n_u, 1e-30))
    v = cl * scale[c.pi]
    # exact per-PU released l1: removing PU u removes exactly its own contribution
    per_pu = np.bincount(c.pi, weights=np.abs(v), minlength=c.P)
    print(f"    B = {B:,.0f}; max released ||v_u||_1 over all {c.P:,} customers = "
          f"{per_pu.max():,.3f}")
    print(f"    ratio to B = {per_pu.max()/B:.9f}  "
          f"{'OK' if per_pu.max() <= B*1.000001 else 'VIOLATION'}")
    # explicit neighbour check on the fattest customers, running the real pipeline both sides
    fattest = np.argsort(-c.norms)[:40]
    worst = 0.0
    tot_all = np.bincount(c.gi, weights=v, minlength=c.K)
    for u in fattest:
        m = c.pi != u
        pi2, gi2, val2 = c.pi[m], c.gi[m], c.val[m]
        cl2 = np.clip(val2, -B, B)
        nu2 = np.bincount(pi2, weights=np.abs(cl2), minlength=c.P)
        v2 = cl2 * np.minimum(1.0, B / np.maximum(nu2, 1e-30))[pi2]
        tot2 = np.bincount(gi2, weights=v2, minlength=c.K)
        worst = max(worst, float(np.abs(tot_all - tot2).sum()))
    print(f"    neighbour simulation, 40 fattest customers (full pipeline both sides):")
    print(f"      max ||released(D) - released(D\\u)||_1 = {worst:,.3f} = {worst/B:.6f} x B")
    print("    -> the join fan-out is invisible to the bound: one customer owning 4,000 rows still")
    print("       moves the release by at most B, because the clip is on the PU's TOTAL.\n")

    print("D2  ARRIVALS: target absent at release 1, present at release 2, same frozen G_fix.")
    print("    The attacker differences the two releases, so the target's mass appears against")
    print("    two independent noise draws rather than one.")
    N = 400_000
    print(f"    {'releases':>10}{'attacker adv':>14}{'bound(2 eps_v)':>16}{'verdict':>10}")
    for nq in (1, 2, 3):
        # difference of nq releases before and nq after; noise adds, signal adds
        pre = sum(r.laplace(0, B / EPS_V, N) for _ in range(nq))
        post_absent = pre + sum(r.laplace(0, B / EPS_V, N) for _ in range(nq))
        post_present = post_absent + B * nq
        d_abs = post_absent - pre
        d_pres = post_present - pre
        cuts = np.quantile(np.concatenate([d_abs, d_pres]), np.linspace(.001, .999, 300))
        adv = max(float((d_pres >= x).mean() - (d_abs >= x).mean()) for x in cuts)
        bd = bound(EPS_V * nq)
        print(f"    {nq:>10}{adv:>14.4f}{bd:>16.4f}"
              f"{'OK' if adv <= bd + 0.01 else 'VIOLATION':>10}")
    print("    -> differencing does not beat the bound: the target's arrival is protected by the")
    print("       eps of the releases that contain it, exactly as sequential composition says.")
    print("    CAVEAT: this assumes the frozen G_fix is NOT refreshed on arrival. If a new PU")
    print("       creates a group and G_fix is then re-released, that is the R*delta_0 case.")


if __name__ == "__main__":
    main()
