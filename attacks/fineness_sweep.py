"""Does the l1-clip advantage over Google DP grow with grouping fineness?

Two independent axes, separated by construction on TPC-H:
  - groups shrink (tau binds harder)      month -> month|nation   [k_u unchanged: nation is
                                                                   a function of the customer]
  - k_u grows (truncation bites harder)   month -> day

Both mechanisms are tuned over C_u AND the budget split. Google additionally gets top-C_u
selection (keep each PU's largest cells rather than a random C_u), which is DP-legal and was
the single biggest legitimate Google improvement found by adversarial review.

Scoring: relative L1 over the TRUE (uncapped, untruncated) key set. A tau-suppressed group is
scored as released 0, i.e. it costs its full true value. Anything else lets a mechanism buy
accuracy by returning fewer rows.

    python3 attacks/fineness_sweep.py [--db tpch_sass_sf10.db] [--trials 4]
"""

import argparse
import itertools

import duckdb
import numpy as np
from scipy.stats import norm

EPS, DELTA = 1.0, 1e-6

GROUPINGS = {
    "year":           ("cast(year(l_shipdate) as varchar)", 7),
    "month":          ("strftime(l_shipdate,'%Y-%m')", 84),
    "month|nation":   ("strftime(l_shipdate,'%Y-%m')||'|'||cast(c_nationkey as varchar)", 2095),
    "month|priority": ("strftime(l_shipdate,'%Y-%m')||'|'||o_orderpriority", 420),
    "week|nation":    ("cast(date_trunc('week',l_shipdate) as varchar)||'|'||"
                       "cast(c_nationkey as varchar)", 9050),
    "day":            ("cast(l_shipdate as varchar)", 2526),
    "day|region":     ("cast(l_shipdate as varchar)||'|'||cast(n_regionkey as varchar)", 12630),
}

# eps_b, eps_eta, eps_v. The high-eps_eta rows matter: Google's tau scales with C_u, so it needs
# a LARGE C_u (to kill truncation bias) and a large eps_eta (to afford the tau that C_u buys).
# A grid capped at eps_eta = 1/3 cannot express that and silently under-tunes the baseline.
SPLITS = [(1 / 3, 1 / 3, 1 / 3), (.05, 1 / 3, .6167), (.05, .25, .70),
          (.002, .30, .698), (.002, .20, .798), (.05, .10, .85), (.05, .05, .90),
          (.002, .40, .598), (.002, .50, .498), (.002, .60, .398), (.0001, .50, .4999)]


def tau(eps_eta, delta_eta, cu):
    inner = 2.0 - 2.0 * (1.0 - delta_eta) ** (1.0 / cu)
    return np.inf if inner <= 0 else 1.0 - cu * np.log(inner) / eps_eta


def approx_bounds(vals, eps_b, r):
    """Google's ApproxBounds over log2 bins. Occupied bins only."""
    b = np.clip(np.floor(np.log2(np.maximum(vals, 1.0))).astype(int), 0, 45)
    ub, cb = np.unique(b, return_counts=True)
    noisy = cb + r.laplace(0, 1.0 / eps_b, size=len(ub))
    ok = ub[noisy >= 24.88 / eps_b]
    return 2.0 ** ((ok.max() + 1) if len(ok) else 1)


class Cells:
    """(PU, group) cells for one grouping, with per-PU rank arrays precomputed."""

    def __init__(self, con, gexpr, filt):
        rows = con.execute(f"""
            WITH c AS (
              SELECT o_custkey AS pu, {gexpr} AS g, sum(l_extendedprice) AS t
              FROM tpch.lineitem JOIN tpch.orders ON o_orderkey=l_orderkey
              JOIN tpch.customer ON c_custkey=o_custkey
              JOIN tpch.nation ON n_nationkey=c_nationkey
              WHERE {filt} GROUP BY 1,2)
            SELECT dense_rank() OVER (ORDER BY pu)-1 AS pid,
                   dense_rank() OVER (ORDER BY g)-1 AS gid, t
            FROM c""").fetchnumpy()
        self._init_from(rows["pid"].astype(np.int64), rows["gid"].astype(np.int64),
                        rows["t"].astype(np.float64))

    def _init_from(self, pi, gi, val):
        self.pi, self.gi, self.val = pi, gi, val
        self.K = int(self.gi.max()) + 1
        self.P = int(self.pi.max()) + 1
        self.truth = np.bincount(self.gi, weights=self.val, minlength=self.K)
        self.npu_g = np.bincount(self.gi, minlength=self.K).astype(float)
        # per-PU cell counts, and rank-by-descending-value within each PU (for top-C_u)
        self.k_u = np.bincount(self.pi, minlength=self.P)
        o = np.lexsort((-self.val, self.pi))
        self.rank_top = np.empty(len(self.val), np.int64)
        starts = np.concatenate([[0], np.cumsum(self.k_u)])
        self.rank_top[o] = np.arange(len(self.val)) - starts[self.pi[o]]
        self.starts = starts
        self.norms = np.bincount(self.pi, weights=np.abs(self.val), minlength=self.P)

    def rank_random(self, r):
        """Rank within each PU under a fresh random permutation (Google's random truncation)."""
        o = np.lexsort((r.random(len(self.val)), self.pi))
        rk = np.empty(len(self.val), np.int64)
        rk[o] = np.arange(len(self.val)) - self.starts[self.pi[o]]
        return rk

    def score(self, out):
        return float(np.sum(np.abs(out - self.truth)) / np.sum(np.abs(self.truth)))


def votes(c, rank, cu):
    """Per-group distinct-PU counts with each PU voting in at most C_u groups."""
    keep = rank < cu
    return np.bincount(c.gi[keep], minlength=c.K).astype(float)


def google_values(c, cv, U, rank, rescale):
    """Google's per-PU value contribution, truncated to C_v cells each capped at U.

    With rescale, the kept cells are multiplied by total_u/kept_u to restore the PU's mass and
    then RE-CLIPPED to U. Still <= C_v cells each <= U, so ||v_u||_1 <= C_v*U is untouched --
    it just stops leaving unused sensitivity budget on the table. Rescaling WITHOUT the re-clip
    is not legal.
    """
    keep = rank < cv
    # two-sided: np.minimum(.,U) alone caps only from above and loses the bound on signed data,
    # the same bug class already found and fixed in the l1 clip. Identical on non-negative data.
    v = np.clip(c.val, -U, U)
    if rescale:
        kept = np.bincount(c.pi, weights=np.where(keep, c.val, 0.0), minlength=c.P)
        f = np.where(np.abs(kept) > 1e-30, c.norms / np.maximum(np.abs(kept), 1e-30), 1.0)
        v = np.clip(c.val * f[c.pi], -U, U)
    tot = np.bincount(c.gi[keep], weights=v[keep], minlength=c.K)
    used = np.bincount(c.pi[keep], weights=np.abs(v[keep]), minlength=c.P).max()
    assert used <= cv * U * 1.000001, f"sensitivity violated: {used} > {cv*U}"
    return tot


class Cache:
    """Everything that does not depend on (eps_eta, eps_v) is computed once.

    Both arms get a separate C_e for the vote histogram and C_v for the values -- they are two
    releases with two sensitivities and nothing forces them equal. Ours simply has no C_v.
    """

    def __init__(self, c, cus, trials):
        r = np.random.default_rng(99)
        self.c, self.trials = c, trials
        self.rank = [c.rank_random(r) for _ in range(trials)]
        self.votes = {(t, ce): votes(c, self.rank[t], ce)
                      for t in range(trials) for ce in cus}
        ebs = sorted({s[0] for s in SPLITS})
        self.U, self.B, self.g_rand, self.g_top, self.g_res, self.o_tot = {}, {}, {}, {}, {}, {}
        for eb in ebs:
            for t in range(trials):
                U = approx_bounds(c.val, EPS * eb, r)
                self.U[(eb, t)] = U
                for cv in cus:
                    self.g_rand[(eb, t, cv)] = google_values(c, cv, U, self.rank[t], False)
                    self.g_top[(eb, t, cv)] = google_values(c, cv, U, c.rank_top, False)
                    self.g_res[(eb, t, cv)] = google_values(c, cv, U, c.rank_top, True)
                B = approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
                self.B[(eb, t)] = B
                cl = np.clip(c.val, -B, B)
                n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
                self.o_tot[(eb, t)] = np.bincount(
                    c.gi, weights=cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi],
                    minlength=c.K)
        self.r = r


ARMS = {"google": "g_rand", "google+top": "g_top", "google+top+rescale": "g_res", "ours": None}


def gauss_vote_params(ce, mk, eps_eta, delta):
    """Gaussian on the vote histogram: every PU votes 1 in each of its (truncated to C_e) groups,
    so l2 sensitivity is sqrt(min(C_e, max k_u)) where l1 would have been C_e. Both arms may use
    this -- it is a partition-selection change, independent of how values are clipped."""
    sigma = np.sqrt(min(ce, mk)) * np.sqrt(2.0 * np.log(1.25 / (delta / 2))) / eps_eta
    thr = 1.0 + sigma * norm.ppf(1.0 - (delta / 2) / max(ce, 1))
    return sigma, thr


def tune(cache, cus, arm, vote_noise="laplace"):
    """Tune over C_e (votes) x C_v (values) x budget split. Ours has no C_v."""
    c, r = cache.c, cache.r
    tab = ARMS[arm]
    cvs = cus if tab else [None]
    mk = int(c.k_u.max())
    best = None
    for ce, cv, (eb, ee, ev) in itertools.product(cus, cvs, SPLITS):
        if vote_noise == "gauss":
            sigma, thr = gauss_vote_params(ce, mk, EPS * ee, DELTA)
        else:
            thr = tau(EPS * ee, DELTA, ce)
        es, rels = [], []
        for t in range(cache.trials):
            vn = (r.normal(0, sigma, size=c.K) if vote_noise == "gauss"
                  else r.laplace(0, ce / (EPS * ee), size=c.K))
            rel = cache.votes[(t, ce)] + vn >= thr
            if tab:
                tot, sc = getattr(cache, tab)[(eb, t, cv)], cv * cache.U[(eb, t)]
            else:
                tot, sc = cache.o_tot[(eb, t)], cache.B[(eb, t)]
            out = np.where(rel, tot + r.laplace(0, sc / (EPS * ev), size=c.K), 0.0)
            es.append(c.score(out))
            rels.append(rel.sum())
        e = float(np.mean(es))
        if best is None or e < best[0]:
            best = (e, (ce, cv), (eb, ee, ev), float(np.mean(rels)))
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--filter", default="c_acctbal>=8000")
    ap.add_argument("--trials", type=int, default=4)
    ap.add_argument("--groupings", default=",".join(GROUPINGS))
    ap.add_argument("--votes", default="laplace", choices=["laplace", "gauss"])
    a = ap.parse_args()

    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")
    print(f"SUM(l_extendedprice), PU=customer, WHERE {a.filter}, eps={EPS}, {a.trials} trials")
    print("every arm tuned over C_e (votes) x C_v (values) x 7 budget splits;")
    print("suppressed groups scored as 100% error against the uncapped truth\n")
    hdr = (f"{'grouping':<15}{'groups':>7}{'max k_u':>8}{'med PU/g':>9}"
           f"{'Google':>8}{'+top':>8}{'+rescale':>9}{'ours':>8}{'gap':>7}  best cfg")
    print(hdr)
    print("-" * (len(hdr) + 14))
    for name in a.groupings.split(","):
        gexpr = GROUPINGS[name][0]
        c = Cells(con, gexpr, a.filter)
        mk = int(c.k_u.max())
        cus = sorted({1, 2, 5, 10, 19, 30, mk // 2 or 1, mk})
        cache = Cache(c, cus, a.trials)
        res = {arm: tune(cache, cus, arm, a.votes) for arm in ARMS}
        g = min(res[k][0] for k in ARMS if k != "ours")
        o = res["ours"]
        print(f"{name:<15}{c.K:>7,}{mk:>8}{np.median(c.npu_g):>9,.0f}"
              f"{100*res['google'][0]:>7.2f}%{100*res['google+top'][0]:>7.2f}%"
              f"{100*res['google+top+rescale'][0]:>8.2f}%{100*o[0]:>7.2f}%"
              f"{g/o[0]:>6.2f}x  ours C_e={o[1][0]}, "
              f"Google C_e={res['google+top+rescale'][1][0]}/C_v="
              f"{res['google+top+rescale'][1][1]}", flush=True)
        del c, cache
    print("\n'+top'     = Google keeps each PU's C_v largest cells instead of a random C_v.")
    print("'+rescale' = ... then rescales them to the PU's true total and re-clips to U,")
    print("             which fills the C_v*U sensitivity it is already paying for.")
    print("'gap'      = best of the three Google arms over ours.")


if __name__ == "__main__":
    main()
