"""Which k_u distribution decides whether the l1 clip or Google-style truncation wins?

The whale experiment showed a bimodal k_u lets Google win, but that was one shape. This sweeps a
family of k_u distributions at matched PU count, group count and total mass, and reports several
candidate summary statistics alongside the outcome, so the predictor can be identified rather
than assumed.

Intuition to test: our B is the max per-PU NORM, and every group pays that noise; Google pays
C_v * U where U is the max per-CELL value and C_v trades truncation bias. For roughly equal cell
values, norm ~ k_u * cell, so the ratio should track how far the largest k_u sits above the k_u
that Google can afford to truncate to.

    python3 attacks/ku_families.py
"""
import itertools
import numpy as np
import fineness_sweep as F

EPS, T, K, N = 1.0, 3, 200, 30000


def make(ku, rr):
    """Build cells for a given per-PU k_u vector; cell values are iid so only k_u shape varies."""
    pi, gi, val = [], [], []
    for u, k in enumerate(ku):
        k = int(min(max(k, 1), K))
        gs = rr.choice(K, size=k, replace=False)
        for g in gs:
            pi.append(u); gi.append(g); val.append(abs(rr.normal(100, 20)))
    c = F.Cells.__new__(F.Cells)
    c._init_from(np.array(pi), np.array(gi), np.array(val, dtype=float))
    return c


def evaluate(c, rr):
    mk = int(c.k_u.max())
    cands = sorted(set([1, 2, 3, 5, 8, 13, 21, 34, 55, 89]) | {mk})
    ranks = [c.rank_random(rr) for _ in range(T)]
    vt = {(t, ce): F.votes(c, ranks[t], ce) for t in range(T) for ce in cands}
    ca = {}
    for eb in sorted({s[0] for s in F.SPLITS}):
        for t in range(T):
            U = F.approx_bounds(c.val, EPS * eb, rr)
            B = F.approx_bounds(c.norms[c.norms > 0], EPS * eb, rr)
            cl = np.clip(c.val, -B, B)
            nu = np.bincount(c.pi, weights=np.abs(cl), minlength=c.P)
            ca[(eb, t)] = (U, B, {cv: F.google_values(c, cv, U, ranks[t], False) for cv in cands},
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
                    rel = (v + rr.laplace(0, ce / (EPS * ee), size=c.K) >= thr) & (v > 0)
                    tot, sc = (g[cv], cv * U) if kind == "g" else (o, B)
                    es.append(c.score(np.where(rel, tot + rr.laplace(
                        0, sc / (EPS * evv), size=c.K), 0.0)))
                e = float(np.mean(es))
                best = e if best is None or e < best else best
        return best
    return ev("g"), ev("o")


def main():
    fams = []
    for c_ in (1, 5, 20, 60):
        fams.append((f"constant({c_})", lambda r, c_=c_: np.full(N, c_)))
    for m in (10, 60, 200):
        fams.append((f"uniform(1,{m})", lambda r, m=m: r.integers(1, m + 1, N)))
    for a in (1.3, 2.0, 3.0):
        fams.append((f"zipf(a={a})", lambda r, a=a: np.minimum(r.zipf(a, N), K)))
    for s_ in (0.6, 1.4):
        fams.append((f"lognormal(s={s_})",
                     lambda r, s_=s_: np.maximum(1, r.lognormal(2.5, s_, N).astype(int))))
    for f_, W in ((0.005, 100), (0.02, 100), (0.05, 60)):
        fams.append((f"bimodal({f_},{W})",
                     lambda r, f_=f_, W=W: np.where(r.random(N) < f_, W, 1)))
    print(f"K={K} groups, N={N:,} PUs, iid cell values -- only the k_u SHAPE varies.\n")
    hdr = (f"{'k_u family':<20}{'med':>5}{'mean':>6}{'max':>5}{'max/med':>9}{'harm':>6}"
           f"{'cells':>9}{'google':>9}{'ours':>8}{'ratio':>8}  who")
    print(hdr)
    print("-" * len(hdr))
    for name, gen in fams:
        rr = np.random.default_rng(4)
        ku = gen(rr)
        if ku.sum() > 4_000_000:
            print(f"{name:<20}  skipped, {ku.sum():,} cells")
            continue
        c = make(ku, rr)
        g, o = evaluate(c, rr)
        who = "GOOGLE" if g < o * 0.98 else ("ours" if o < g * 0.98 else "tie")
        harm = len(c.k_u) / np.sum(1.0 / c.k_u)
        print(f"{name:<20}{np.median(c.k_u):>5.0f}{c.k_u.mean():>6.1f}{c.k_u.max():>5}"
              f"{c.k_u.max()/max(np.median(c.k_u),1):>9.1f}{harm:>6.1f}{len(c.val):>9,}"
              f"{100*g:>8.2f}%{100*o:>7.2f}%{g/o:>7.2f}x  {who}", flush=True)
        del c


if __name__ == "__main__":
    main()
