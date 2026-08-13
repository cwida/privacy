"""Dandan's follow-ups, 12 Aug 2026 — three checkable claims.

(1) AVG under half-dataset splitting needs no rescaling, so it should escape the exact
    cancellation that kills SUM.
(2) Restricting ApproxBounds to c2-c1+2 candidate bins (derived from a stored inter-attribute
    relationship 2^c1*a1 <= a2 <= 2^c2*a1) should give a better bound for the same budget.
(3) Requiring the max noisy bin count to clear a threshold in each of t per-group histograms
    should let us combine evidence and use a smaller tau.

    python3 attacks/dandan_followups.py
"""

import numpy as np

from fineness_sweep import GROUPINGS, Cells

P_SUCCESS = 1.0 - 1e-9
EPS = 1.0


def ab_threshold(n_bins, eps_b=1.0):
    return -np.log(2.0 * (1.0 - P_SUCCESS ** (1.0 / (2.0 * n_bins)))) / eps_b


def q1_avg_splitting(c, r, trials=4000):
    """Half the PUs, double the per-attribute budget. Does AVG escape the cancellation?"""
    B, Cc = 4.0e6, 20.0            # per-PU sum bound and count bound
    t = 4                          # aggregation attributes
    tot_v = float(c.norms.sum())
    tot_n = float(c.P)
    truth = tot_v / tot_n
    print("(1) AVG under half-dataset splitting")
    print(f"    t={t} attributes, eps={EPS}; full-data per-attribute budget eps/t={EPS/t:.3f},")
    print(f"    split per-attribute budget 2eps/t={2*EPS/t:.3f} (parallel composition over"
          " disjoint PU halves)")
    rows = []
    for label, share, e_att in (("full data",  1.0, EPS / t),
                                ("half, doubled budget", 0.5, 2 * EPS / t)):
        v, n = tot_v * share, tot_n * share
        # sampling error from evaluating on a random half of the PUs
        if share < 1.0:
            samp = np.array([c.norms[r.random(c.P) < share].sum() for _ in range(200)])
            samp_rel = float(np.std(samp / (tot_v * share)))
        else:
            samp_rel = 0.0
        ns = r.laplace(0, B / e_att, trials)
        nc = r.laplace(0, Cc / e_att, trials)
        est = (v + ns) / np.maximum(n + nc, 1.0)
        rel = float(np.mean(np.abs(est - truth)) / truth)
        rows.append((label, B / e_att, v, rel, samp_rel))
        print(f"    {label:<22} noise scale on SUM {B/e_att:>12,.0f}   true SUM {v:>16,.0f}"
              f"   rel err {100*rel:>7.4f}%")
    a, b = rows[0], rows[1]
    print(f"    -> noise scale HALVES ({a[1]:,.0f} -> {b[1]:,.0f}) but so does the true SUM,")
    print(f"       so relative error is unchanged: {100*a[3]:.4f}% vs {100*b[3]:.4f}%"
          f" = {b[3]/a[3]:.3f}x")
    print(f"    -> the halved COUNT in the denominator IS the rescaling. No rescale is written")
    print(f"       down, but the cancellation still happens. Plus sampling error on the half:"
          f" {100*rows[1][4]:.3f}%\n")


def q2_restricted_bins():
    print("(2) Restricting ApproxBounds to fewer candidate bins")
    print("    threshold = -ln(2(1 - P^(1/2n)))/eps_b,  P = 1-1e-9  -> grows LOGARITHMICALLY in n")
    print(f"    {'bins n':>8}{'threshold x eps_b':>20}{'vs 64 bins':>13}")
    base = ab_threshold(64)
    for n in (2, 4, 8, 16, 32, 64, 128, 256, 512):
        t = ab_threshold(n)
        print(f"    {n:>8}{t:>20.2f}{t/base:>12.3f}x")
    print(f"    -> collapsing 64 candidate bins to 4 (c2-c1=2) lowers the threshold by only"
          f" {100*(1-ab_threshold(4)/base):.0f}%.")
    print("    -> and the budget it would save is not there to save: across every sweep in")
    print("       docs/dp/bound_derivation.md the tuned optimum is eps_b = 0.002, i.e. 0.2% of")
    print("       eps. Making automatic bounding entirely FREE is worth 0.2%.\n")


def q3_combined_tau(r, trials=200000):
    """t per-group tests, budget eps_eta/t each, all must pass. Does the threshold drop?"""
    print("(3) Combining evidence across t per-group histograms to lower tau")
    eps_eta, delta_eta, cu = 0.30, 3.0e-7, 1
    print(f"    eps_eta={eps_eta}, delta_eta={delta_eta:g}, C_u={cu}; a singleton group has"
          " true count 1")
    print("    tau really does drop with t. But tau is not the decision-relevant number --")
    print("    the smallest group that actually gets RELEASED is, and that is what to compare.")
    print(f"    {'t tests':>8}{'per-test eps':>14}{'noise/test':>12}{'tau':>8}"
          f"{'FP rate':>11}{'min group released':>20}")
    for t in (1, 2, 4, 8):
        e = eps_eta / t                       # budget split across the t histograms
        scale = cu / e
        # each test must fire with prob delta^(1/t) on a singleton so the AND fires with delta
        p = delta_eta ** (1.0 / t)
        thr = 1.0 - scale * np.log(2.0 * p)   # Laplace tail: P(X >= thr-1) = p
        fp = np.mean(np.all(1.0 + r.laplace(0, scale, (trials, t)) >= thr, axis=1))
        # n such that P(all t tests pass) = 0.5
        n50 = thr + scale * np.log(0.5 / (1.0 - 2.0 ** (-1.0 / t)))
        print(f"    {t:>8}{e:>14.4f}{scale:>12.2f}{thr:>8.1f}{fp:>11.2e}{n50:>20.1f}")
    print("    -> tau falls 48.8 -> 32.6, but the release floor RISES 48.8 -> 80.5. Splitting")
    print("       eps_eta across t tests multiplies each test's noise by t, and requiring all t")
    print("       to pass costs more than the lower threshold buys.")
    print("    -> the underlying reason is that all t histograms estimate the SAME quantity (how")
    print("       many PUs are in the group), so measuring it t times on eps_eta/t each is")
    print("       strictly worse than measuring it once on eps_eta. An OR instead of an AND")
    print("       loses too: false positives then need p = delta/t, which raises tau.")
    print("    -> reusing the bound-selection histograms for free does not rescue it either:")
    print("       they are paid for by eps_b = 0.002, so their noise scale is C_u/0.002 = 500")
    print("       against a threshold near 48 -- far too noisy to carry any evidence.\n")


def em_median(x, eps, lo, hi, r):
    """Exponential-mechanism median over a PUBLIC range [lo,hi]. Pure eps-DP, no delta, and no
    data-dependent sensitivity: changing one point moves the rank utility by at most 1."""
    xs = np.clip(np.sort(x), lo, hi)
    edges = np.concatenate(([lo], xs, [hi]))
    width = np.diff(edges)                            # gap i spans ranks i
    n = len(xs)
    util = -np.abs(np.arange(len(width)) - n / 2.0)    # |rank - n/2|
    logw = np.log(np.maximum(width, 1e-300)) + eps * util / 2.0
    logw -= logw.max()
    w = np.exp(logw)
    w /= w.sum()
    i = r.choice(len(w), p=w)
    return float(edges[i] + r.random() * width[i])


def q4_median_splitting(c, r, trials=120):
    """Does half-dataset splitting win for a GLOBAL-sensitivity (range-based) median?"""
    print("(4) Half-dataset splitting with a global-sensitivity median (not smooth sensitivity)")
    x = c.norms[c.norms > 0]
    lo, hi = 0.0, 8.0e6                               # public output range
    truth = float(np.median(x))
    print(f"    data = per-PU totals, n={len(x):,}, true median {truth:,.0f},"
          f" public range [0, {hi:,.0f}]")
    print("    exponential mechanism on rank utility: sensitivity is 1 rank REGARDLESS of n,")
    print("    so unlike smooth sensitivity it does not grow when the data halves.")
    print("    swept over eps and n, because the answer depends on which error dominates:")
    print(f"    {'n':>9}{'eps':>8}{'full err':>10}{'half err':>10}{'half/full':>10}"
          f"{'subsampling':>13}{'  dominated by':<16}")
    for n_sub in (2000, 181532):
        xs_pop = x[r.random(len(x)) < n_sub / len(x)] if n_sub < len(x) else x
        # subsampling error of the half-sample median alone, with NO DP noise at all
        sub = float(np.mean([abs(float(np.median(xs_pop[r.random(len(xs_pop)) < 0.5])) - truth)
                             / truth for _ in range(200)]))
        for eps in (0.002, 0.02, 0.2, 2.0):
            res = []
            for share, e in ((1.0, eps), (0.5, 2 * eps)):
                errs = []
                for _ in range(trials):
                    xx = xs_pop[r.random(len(xs_pop)) < share] if share < 1.0 else xs_pop
                    errs.append(abs(em_median(xx, e, lo, hi, r) - truth) / truth)
                res.append(float(np.mean(errs)))
            reg = "DP noise" if res[1] > 2 * sub else "subsampling"
            print(f"    {len(xs_pop):>9,}{eps:>8.3f}{100*res[0]:>9.4f}%{100*res[1]:>9.4f}%"
                  f"{res[1]/res[0]:>9.2f}x{100*sub:>12.4f}%  {reg:<16}")
    print("    -> two clean regimes. While DP noise dominates, the ratio sits at 0.80-1.09x:")
    print("       the rank error halves with the doubled budget, but the rank is now out of n/2,")
    print("       so the quantile error is essentially unchanged -- the same cancellation, with")
    print("       at most a ~1.25x win at the edge. Once the mechanism is accurate the")
    print("       subsampling error of the median itself dominates and the doubled budget cannot")
    print("       buy it back: 1.3x, 20x, 290x.")
    print("    -> so a global-sensitivity median does NOT rescue splitting. It reproduces the")
    print("       same narrow window the smooth-sensitivity version had (~0.85x), and the window")
    print("       sits where the median is too noisy to be worth releasing anyway.\n")


def main():
    import duckdb
    con = duckdb.connect(config={"threads": 2})
    con.execute("SET enable_progress_bar=false")
    con.execute("ATTACH '/home/ila/Code/privacy/tpch_sass_sf10.db' AS tpch (READ_ONLY)")
    r = np.random.default_rng(7)
    c = Cells(con, GROUPINGS["month"][0], "c_acctbal>=8000")
    print(f"TPC-H sf10, PU=customer, acctbal>=8000: {c.P:,} PUs\n")
    q1_avg_splitting(c, r)
    q2_restricted_bins()
    q3_combined_tau(r)
    q4_median_splitting(c, r)


if __name__ == "__main__":
    main()
