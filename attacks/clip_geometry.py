"""Which l1-clipping geometry has the least bias, at identical privacy?

Every geometry below maps a PU's cell vector t_u to some v_u with ||v_u||_1 <= B, so all of
them are exactly as private (value noise Laplace(B/eps_v)). They differ only in WHERE the
clipped mass is taken from, which is pure utility. Only PUs with ||t_u||_1 > B are touched.

  proportional   v = t * min(1, B/||t||_1)              mass removed in proportion to each cell
  softthresh     v = sign(t) * max(|t| - lam, 0)        l1-ball projection: kills SMALL cells
  waterfill      v = sign(t) * min(|t|, c)              per-PU adaptive cap: shaves LARGE cells
  keeptop        keep largest cells whole until B is spent, drop the rest
  uniformcap     v = sign(t) * min(|t|, B/k_u)          flat per-PU cap, Google-shaped

lam / c are solved per PU so that ||v_u||_1 = B exactly.

    python3 attacks/clip_geometry.py [--groupings month,month|nation,day]
"""

import argparse
import itertools

import numpy as np

from fineness_sweep import (DELTA, EPS, GROUPINGS, SPLITS, Cells, approx_bounds, tau, votes)


def _sorted_desc(c):
    """Order cells by descending |t| within each PU; return (order, |t| sorted, within-PU idx)."""
    o = np.lexsort((-np.abs(c.val), c.pi))
    s = np.abs(c.val)[o]
    j = np.arange(len(s)) - c.starts[c.pi[o]] + 1          # 1-based rank within PU
    prefix = np.cumsum(s)
    prefix -= np.repeat(prefix[c.starts[:-1]] - s[c.starts[:-1]], c.k_u)
    return o, s, j, prefix


def _pick_first_valid(c, o, valid, cand):
    """Each clipped PU has exactly one valid breakpoint; scatter its value into a per-PU array."""
    out = np.full(c.P, np.nan)
    out[c.pi[o][valid]] = cand[valid]
    return out


def clip(c, B, how):
    """Return v with ||v_u||_1 <= B for every PU, under the named geometry."""
    norms = c.norms
    over = norms > B
    if how == "proportional":
        cl = np.clip(c.val, -B, B)
        n_u = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
        return cl * np.minimum(1.0, B / np.maximum(n_u, 1e-30))[c.pi]
    if how == "uniformcap":
        cap = np.where(over, B / np.maximum(c.k_u, 1), np.inf)
        return np.sign(c.val) * np.minimum(np.abs(c.val), cap[c.pi])

    o, s, j, prefix = _sorted_desc(c)
    nxt = np.empty_like(s)
    nxt[:-1] = s[1:]
    nxt[-1] = 0.0
    last = j == c.k_u[c.pi[o]]
    nxt = np.where(last, 0.0, nxt)

    if how == "softthresh":                       # keep top j cells, subtract lam from each
        lam = (prefix - B) / j
        valid = over[c.pi[o]] & (s > lam) & (lam >= nxt)
        lamu = _pick_first_valid(c, o, valid, lam)
        lamu = np.where(np.isnan(lamu), 0.0, lamu)
        return np.sign(c.val) * np.maximum(np.abs(c.val) - lamu[c.pi], 0.0)
    if how == "waterfill":                        # cap top j cells at c, leave the rest
        cc = (B - norms[c.pi[o]] + prefix) / j
        valid = over[c.pi[o]] & (s > cc) & (cc >= nxt)
        ccu = _pick_first_valid(c, o, valid, cc)
        ccu = np.where(np.isnan(ccu), np.inf, ccu)
        return np.sign(c.val) * np.minimum(np.abs(c.val), ccu[c.pi])
    if how == "keeptop":                          # take whole cells greedily, truncate the last
        room = B - (prefix - s)                   # budget left before this cell
        v = np.clip(room, 0.0, None)
        v = np.minimum(s, v)
        out = np.empty_like(c.val)
        out[o] = np.sign(c.val[o]) * np.where(over[c.pi[o]], v, s)
        return out
    raise ValueError(how)


GEOMS = ["proportional", "softthresh", "waterfill", "keeptop", "uniformcap"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="/home/ila/Code/privacy/tpch_sass_sf10.db")
    ap.add_argument("--filter", default="c_acctbal>=8000")
    ap.add_argument("--groupings", default="month,month|nation,day")
    ap.add_argument("--trials", type=int, default=3)
    a = ap.parse_args()

    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute(f"ATTACH '{a.db}' AS tpch (READ_ONLY)")

    print(f"SUM(l_extendedprice), PU=customer, WHERE {a.filter}, eps={EPS}, {a.trials} trials")
    print("all geometries have identical sensitivity B -> identical privacy; "
          "differences are pure bias\n")
    for name in a.groupings.split(","):
        c = Cells(con, GROUPINGS[name][0], a.filter)
        mk = int(c.k_u.max())
        cus = sorted({1, 2, 5, 10, 19, 30, mk})
        r = np.random.default_rng(99)
        ranks = [c.rank_random(r) for _ in range(a.trials)]
        vt = {(t, cu): votes(c, ranks[t], cu) for t in range(a.trials) for cu in cus}
        ebs = sorted({s[0] for s in SPLITS})
        Bs = {(eb, t): approx_bounds(c.norms[c.norms > 0], EPS * eb, r)
              for eb in ebs for t in range(a.trials)}
        print(f"  {name}: {c.K:,} groups, max k_u={mk}, "
              f"{int(np.sum(c.norms > Bs[(ebs[0], 0)])):,} of {c.P:,} PUs over B")
        print(f"    {'geometry':<14}{'error':>9}{'clip bias':>11}{'C_u':>5}  best split")
        rows = []
        for how in GEOMS:
            tot = {}
            for (eb, t), B in Bs.items():
                v = clip(c, B, how)
                assert np.max(np.bincount(c.pi, weights=np.abs(v), minlength=c.P)) <= B * 1.000001
                tot[(eb, t)] = np.bincount(c.gi, weights=v, minlength=c.K)
            best = None
            for cu, (eb, ee, ev) in itertools.product(cus, SPLITS):
                thr = tau(EPS * ee, DELTA * ee, cu)
                es = []
                for t in range(a.trials):
                    rel = vt[(t, cu)] + r.laplace(0, cu / (EPS * ee), size=c.K) >= thr
                    out = np.where(rel, tot[(eb, t)]
                                   + r.laplace(0, Bs[(eb, t)] / (EPS * ev), size=c.K), 0.0)
                    es.append(c.score(out))
                e = float(np.mean(es))
                if best is None or e < best[0]:
                    bias = c.score(tot[(eb, 0)])
                    best = (e, cu, (eb, ee, ev), bias)
            rows.append((how, best))
            print(f"    {how:<14}{100*best[0]:>8.3f}%{100*best[3]:>10.3f}%{best[1]:>5}"
                  f"  ({best[2][0]:.3f},{best[2][1]:.3f},{best[2][2]:.3f})", flush=True)
        b = min(rows, key=lambda x: x[1][0])
        p = dict(rows)["proportional"]
        print(f"    -> best is {b[0]} at {100*b[1][0]:.3f}%, "
              f"{p[0]/b[1][0]:.3f}x better than proportional\n")
        del c


if __name__ == "__main__":
    main()
