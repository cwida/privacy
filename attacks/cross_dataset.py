"""End-to-end mechanism comparison on StackOverflow and ClickBench, not just TPC-H.

dataset_profile.py answered "which groups get released" analytically. This answers "what is the
error" by running the real pipeline: bound selection, clipping, partition selection, value noise,
scored as relative L1 over the TRUE uncapped key set with tau-suppressed groups charged full error.

Three arms, each tuned over its own C_e (votes) x C_v (values) x budget split:
  published  Wilson et al. / Google DP library: one C_u truncating values AND votes together,
             Laplace on every channel, ApproxBounds over per-cell values.
  ours-lap   l1 per-PU-norm clip + Laplace values + Laplace votes.
  ours-gauss l1 per-PU-norm clip + Laplace values + GAUSSIAN votes (geometry-matched).

The point of running this off TPC-H is that SO and ClickBench have heavy-tailed k_u (median 1,
max 154-2126) where dataset_profile.py predicts Gaussian votes should LOSE. If the end-to-end
numbers disagree with that prediction, one of the two is wrong.

    python3 attacks/cross_dataset.py
"""

import argparse
import itertools

import numpy as np
from scipy.stats import norm

from fineness_sweep import (DELTA, EPS, SPLITS, Cells, approx_bounds, google_values, tau, votes)

# name: (from/join, pu expr, group expr, value expr, where)
QUERIES = {
    "so posts|month":     ("so.Posts", "OwnerUserId", "strftime(CreationDate,'%Y-%m')",
                           "1.0", "OwnerUserId IS NOT NULL"),
    "so posts|month score": ("so.Posts", "OwnerUserId", "strftime(CreationDate,'%Y-%m')",
                             "greatest(Score,0)", "OwnerUserId IS NOT NULL"),
    "so posts|day":       ("so.Posts", "OwnerUserId", "cast(cast(CreationDate as date) as varchar)",
                           "1.0", "OwnerUserId IS NOT NULL"),
    "so comments|month":  ("so.Comments", "UserId", "strftime(CreationDate,'%Y-%m')",
                           "1.0", "UserId IS NOT NULL"),
    "so badges|month":    ("so.Badges", "UserId", "strftime(Date,'%Y-%m')",
                           "1.0", "UserId IS NOT NULL"),
    "cb hits|region":     ("cb.hits", "UserID", "cast(RegionID as varchar)",
                           "1.0", "UserID IS NOT NULL"),
    "cb hits|date|region": ("cb.hits", "UserID",
                            "cast(EventDate as varchar)||'|'||cast(RegionID as varchar)",
                            "1.0", "UserID IS NOT NULL"),
    "cb width|region":    ("cb.hits", "UserID", "cast(RegionID as varchar)",
                           "ResolutionWidth", "UserID IS NOT NULL"),
}


def load(con, frm, pu, gexpr, vexpr, where, max_cells):
    rows = con.execute(f"""
        WITH c AS (SELECT {pu} AS pu, {gexpr} AS g, sum({vexpr}) AS t
                   FROM {frm} WHERE {where} GROUP BY 1,2)
        SELECT dense_rank() OVER (ORDER BY pu)-1 AS pid,
               dense_rank() OVER (ORDER BY g)-1 AS gid, t FROM c""").fetchnumpy()
    n = len(rows["t"])
    if n > max_cells:
        return None, n
    c = Cells.__new__(Cells)
    c._init_from(rows["pid"].astype(np.int64), rows["gid"].astype(np.int64),
                 rows["t"].astype(np.float64))
    return c, n


def gauss_vote(ce, eps_eta, delta):
    sigma = np.sqrt(ce) * np.sqrt(2.0 * np.log(1.25 / (delta / 2))) / eps_eta
    return sigma, 1.0 + sigma * norm.ppf(1.0 - (delta / 2) / max(ce, 1))


def evaluate(c, cache, arm, ce, cv, split, r, trials):
    eb, ee, ev = split
    if arm == "ours-gauss":
        sigma, thr = gauss_vote(ce, EPS * ee, DELTA)
    else:
        thr = tau(EPS * ee, DELTA, ce)
    es = []
    for t in range(trials):
        vn = (r.normal(0, sigma, size=c.K) if arm == "ours-gauss"
              else r.laplace(0, ce / (EPS * ee), size=c.K))
        vc = cache["votes"][(t, ce)]
        rel = (vc + vn >= thr) & (vc > 0)          # vote-support gate
        if arm == "published":
            tot, sc = cache["g_tot"][(eb, t, cv)], cv * cache["U"][(eb, t)]
        else:
            tot, sc = cache["o_tot"][(eb, t)], cache["B"][(eb, t)]
        es.append(c.score(np.where(rel, tot + r.laplace(0, sc / (EPS * ev), size=c.K), 0.0)))
    return float(np.mean(es))


def build(c, cus, trials):
    r = np.random.default_rng(99)
    ranks = [c.rank_random(r) for _ in range(trials)]
    cache = {"votes": {(t, ce): votes(c, ranks[t], ce) for t in range(trials) for ce in cus},
             "U": {}, "B": {}, "g_tot": {}, "o_tot": {}}
    for eb in sorted({s[0] for s in SPLITS}):
        for t in range(trials):
            U = approx_bounds(c.val, EPS * eb, r)
            cache["U"][(eb, t)] = U
            for cv in cus:
                cache["g_tot"][(eb, t, cv)] = google_values(c, cv, U, ranks[t], False)
            B = approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
            cache["B"][(eb, t)] = B
            cl = np.clip(c.val, -B, B)
            n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
            cache["o_tot"][(eb, t)] = np.bincount(
                c.gi, weights=cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi],
                minlength=c.K)
    return cache, r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--queries", default=",".join(QUERIES))
    ap.add_argument("--trials", type=int, default=3)
    ap.add_argument("--max-cells", type=int, default=5_000_000)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    for al, p in (("so", "stackoverflow_dba_sqlstorm.db"), ("cb", "clickbench_micro.db")):
        con.execute(f"ATTACH '/home/ila/Code/privacy/{p}' AS {al} (READ_ONLY)")
    print(f"eps={EPS}, delta={DELTA:g}, {a.trials} trials; every arm tuned over its own")
    print("C_e x C_v x budget split. Relative L1 over the TRUE key set, suppressed = full error.\n")
    hdr = (f"{'query':<22}{'groups':>8}{'k_u m/max':>11}{'published':>11}{'ours-lap':>10}"
           f"{'ours-gau':>10}{'best gain':>11}")
    print(hdr)
    print("-" * len(hdr))
    for name in a.queries.split(","):
        frm, pu, gexpr, vexpr, where = QUERIES[name]
        c, n = load(con, frm, pu, gexpr, vexpr, where, a.max_cells)
        if c is None:
            print(f"{name:<22}  SKIPPED: {n:,} cells > --max-cells {a.max_cells:,}", flush=True)
            continue
        mk = int(c.k_u.max())
        cus = sorted({1, 2, 3, 5, 8, 13, 21, 34, 55, mk})
        cache, r = build(c, cus, a.trials)
        best = {}
        for arm in ("published", "ours-lap", "ours-gauss"):
            cvs = cus if arm == "published" else [None]
            best[arm] = min(evaluate(c, cache, arm, ce, cv, s, r, a.trials)
                            for ce, cv, s in itertools.product(cus, cvs, SPLITS))
        gain = best["published"] / min(best["ours-lap"], best["ours-gauss"])
        print(f"{name:<22}{c.K:>8,}{f'{np.median(c.k_u):.0f}/{mk}':>11}"
              f"{100*best['published']:>10.2f}%{100*best['ours-lap']:>9.2f}%"
              f"{100*best['ours-gauss']:>9.2f}%{gain:>10.2f}x", flush=True)
        del c, cache
    print("\n'best gain' = published / best of our two vote variants. Where k_u is heavy-tailed")
    print("(median 1) ours-gauss should LOSE to ours-lap, per attacks/dataset_profile.py.")


if __name__ == "__main__":
    main()
