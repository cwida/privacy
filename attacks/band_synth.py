"""Controlled test of the band hypothesis: hold k_u fixed, vary how much group sizes disperse.

band_map.py shows the Gaussian-vs-Laplace gain lives in a multiplicative window of n_g whose
width is sqrt(k_h)/rho ~ 3.5x, and that TPC-H is the only dataset whose whole key set fits inside
one such window (n_g p95/p5 = 2.1). That is a correlational argument across datasets, which also
differ in k_u, in value distribution and in size.

This isolates it. Synthetic bipartite key sets with k_u = 49 for EVERY privacy unit (far above
the k_u ~ 4-5 crossover, so Gaussian always wins the threshold comparison) and group sizes drawn
lognormal(0, sigma). Only sigma changes. Each cell has value 1, so a group's true total IS its
n_g -- the realistic correlated case, where small groups are both hard to release AND easy to
drown in value noise.

    python3 attacks/band_synth.py
"""

import argparse

import numpy as np

import metric_audit as ma
from fineness_sweep import Cells


class SynthCells(Cells):
    def __init__(self, P, K, k, sigma, seed=1):
        r = np.random.default_rng(seed)
        w = r.lognormal(0.0, sigma, size=K)
        p = w / w.sum()
        pi, gi = [], []
        for lo in range(0, P, 5000):                       # chunked to keep memory flat
            n = min(5000, P - lo)
            g = r.choice(K, size=(n, k), p=p)
            for j in range(n):
                u = np.unique(g[j])
                gi.append(u)
                pi.append(np.full(len(u), lo + j))
        pi = np.concatenate(pi)
        gi = np.concatenate(gi)
        keep = np.isin(np.arange(K), gi)                   # drop groups nobody touched
        remap = np.cumsum(keep) - 1
        self._init_from(pi.astype(np.int64), remap[gi].astype(np.int64),
                        np.ones(len(gi)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pus", type=int, default=20000)
    ap.add_argument("--groups", type=int, default=2000)
    ap.add_argument("--ku", type=int, default=49)
    ap.add_argument("--sigmas", default="0,0.4,0.8,1.2,1.6,2.0")
    ap.add_argument("--trials", type=int, default=3)
    a = ap.parse_args()

    print(f"{a.pus:,} PUs, every one in exactly k_u={a.ku} groups (crossover is ~5), "
          f"{a.groups:,} groups, value 1 per cell")
    print(f"eps={ma.EPS} delta={ma.DELTA:g}; both arms tuned over C_e x 11 budget splits, "
          f"{a.trials} trials\n")
    hdr = (f"{'sigma':>6}{'n_g p5':>9}{'p50':>8}{'p95':>9}{'p95/p5':>8}"
           f"{'L1 lap':>9}{'L1 gau':>9}{'gain':>7}{'MAPEc gain':>12}"
           f"{'rel lap':>9}{'rel gau':>9}")
    print(hdr)
    print("-" * len(hdr))
    for sigma in [float(s) for s in a.sigmas.split(",")]:
        c = SynthCells(a.pus, a.groups, a.ku, sigma)
        mk = int(c.k_u.max())
        cus = sorted({1, 2, 5, 10, 19, 30, mk // 2 or 1, mk})
        cache, r = ma.build(c, cus, a.trials)
        out = {}
        for kind in ("Laplace", "Gaussian"):
            best, res = ma.tune(c, cache, cus, kind, mk, r)
            out[kind] = (res[best["L1"]]["L1"], res[best["MAPEc"]]["MAPEc"],
                         res[best["L1"]]["_rel"])
        q = np.quantile(c.npu_g, [.05, .5, .95])
        print(f"{sigma:>6.1f}{q[0]:>9,.0f}{q[1]:>8,.0f}{q[2]:>9,.0f}{q[2]/max(q[0],1):>8.1f}"
              f"{out['Laplace'][0]:>9.4f}{out['Gaussian'][0]:>9.4f}"
              f"{out['Laplace'][0]/out['Gaussian'][0]:>6.2f}x"
              f"{out['Laplace'][1]/out['Gaussian'][1]:>11.2f}x"
              f"{out['Laplace'][2]:>9,.0f}{out['Gaussian'][2]:>9,.0f}", flush=True)
        del c, cache
    print("\nk_u is constant across every row, so any collapse in the gain is caused by group-size")
    print("dispersion alone -- the gain is the share of the key set inside one ~3.5x band.")


if __name__ == "__main__":
    main()
