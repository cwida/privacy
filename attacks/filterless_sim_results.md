# Filterless / CROWD universal bounds — simulation results

Simulation of `filterless-crowd-dp-proposal.pdf` (10 June 2026) outside the extension,
via `attacks/filterless_sim.py`, to decide whether to implement it before writing C++.

The proposal is **contribution bounding measured filterlessly**: rather than asking the
analyst for a per-PU bound (`dp_sum_bound`) or estimating one per query with privacy
budget (Wilson's `APPROX_BOUNDS`), derive it once from the *unfiltered* domain and freeze
it. Two levels — a per-group bound `B_g`, and a per-PU total across groups — both picked
with the CROWD rule (bin by powers of `f`, take the top of the highest bin holding at
least `s` distinct PUs, so an isolated outlier never sets a bound).

TPC-H, PU = `customer`, measure `SUM(l_extendedprice)` unless stated, ε = 1, one
aggregate, static data. Metric = median over released groups of mean
`|released − true| / true`, split into deterministic (clipping/truncation) and Laplace
parts. `f = 2`, `s = 350` unless stated.

| mechanism | bound | sensitivity |
|---|---|---|
| `google` | scalar, from the **filtered** partials | `C_u · B_G`, random truncation to `C_u` groups |
| `google_nocu` | same | `max_u k_u · B_G`, no truncation |
| `filterless` | per-group `B_g`, **full domain** | `Δ̄₁ = max_u Σ_g min(a(u,g), B_g)` — the note, eq. (25) |
| `fl_l1crowd` | same `B_g` | `D_s` = CROWD-supported per-PU total, then ℓ1-clip — **not in the note** |

`google` is a stand-in: Wilson's `APPROX_BOUNDS` is modelled by the same CROWD rule on
the filtered partials pooled to a scalar. That isolates the real differences (filtered vs
full-domain, per-group vs global, truncation vs ℓ1-clip) from any difference in how a
bound is picked, and it **flatters the baseline**, which spends no budget here. Truncation
is scored by its exact expectation (`P[keep g] = min(1, C_u/k_u)`).

---

## 1. The sensitivity does not grow with GROUP BY width — good

sf1, `f = 4`, benign:

| group-by | groups | Δ̄₁ | `C_u · B_G` |
|---|---|---|---|
| `o_orderpriority` | 5 | 7,154,829 | 5,242,880 |
| year-quarter | 27 | 7,154,829 | 16,777,216 |
| year-month | 80 | 7,154,829 | 19,922,944 |
| year-month × priority | 395 | 7,154,829 | 22,020,096 |

Identical at every width. Once `B_g` stops binding, Δ̄₁ is the fattest PU's *total*
footprint, and refining the grouping only splits that footprint. Wilson's `C_u · B_G`
grows with `C_u`, and shrinking `C_u` buys truncation bias instead. So Rem. 2.1 holds and
strengthens as the grouping widens. (This refutes the pre-reading objection that an
uncapped norm would lose on wide group-bys.)

## 2. But the norm is not CROWD-protected — fixable

Eq. (25) is a bare `max_u`. `B_g` has crowd support; the total built from it has none. One
PU that is both above `B_g` and present in every group drags the norm up:

| group-by | Δ̄₁ benign | Δ̄₁ attacked | `D_s` (either) |
|---|---|---|---|
| priority (5) | 6,931,895 | 10,485,760 | 8,388,608 |
| quarter (27) | 7,154,829 | 27,787,264 | 8,388,608 |
| month (80) | 7,154,829 | 41,680,896 | 8,388,608 |
| month × priority (395) | 7,154,829 | 207,093,760 | 8,388,608 |

At 395 groups that is 104% median error against Wilson's 17%. Wilson survives *because*
`C_u` truncation caps the norm — the mechanism the note removes. It is also a leak: the
noise scale tracks one identifiable PU's total.

**Repair, using the note's own primitive one level up:** bin the per-PU totals
`n_u = Σ_g min(a(u,g), B_g)` the same way the values are binned, take the top of the
highest bin with ≥ s distinct PUs as `D_s`, and ℓ1-clip each PU's contribution vector to
it. Sensitivity is then exactly `D_s`, deterministic, no truncation error. `D_s` is
identical in every row above — invariant by construction, since moving it needs s PUs to
be simultaneously fat and wide.

## 3. With the repair, filterless wins on broad queries

sf1, `f = 2`, median relative error:

| group-by | groups | | `filterless` | `fl_l1crowd` | `google` |
|---|---|---|---|---|---|
| priority | 5 | benign | 0.1% | 0.1% | **0.0%** |
| | | attacked | 2.8% | 2.8% | 2.8% |
| quarter | 27 | benign | 0.3% | **0.3%** | 0.4% |
| | | attacked | 2.0% | **1.8%** | 1.9% |
| month | 80 | benign | **0.9%** | 1.0% | 1.2% |
| | | attacked | 6.2% | **4.2%** | 4.4% |
| month × priority | 395 | benign | **4.3%** | 5.0% | 6.6% |
| | | attacked | 104.4% | **16.8%** | 17.1% |

`fl_l1crowd` is at or ahead of Wilson everywhere and never blows up. Wilson wins only on
the narrowest grouping, where `C_u = 5` equals the max fan-out so truncation is free.

## 4. Selectivity is the binding constraint — and this one is inherent

The frozen bound cannot shrink with the filter. That is the entire point of Rem. 3.1, and
it is also the mechanism's ceiling. Sweeping a ladder of increasingly selective filters
(sf1, `month`, one frozen metadata set reused for every row; Wilson re-derives per query):

| filter | selectivity | `B_G` | `filterless` | `fl_l1crowd` | `google` |
|---|---|---|---|---|---|
| no filter | 100% | 1,048,576 | **0.4%** | **0.4%** | 1.0% |
| shipmode AIR/REG AIR | 28.6% | 524,288 | **0.8%** | 1.0% | 1.2% |
| + returnflag=R | 7.0% | 262,144 | 1.8% | 2.0% | **0.7%** |
| + quantity<10 | 1.3% | 32,768 | 49.8% | 58.0% | **0.9%** |
| + discount<0.03 | 0.34% | 32,768 | 183.6% | 217.6% | **1.8%** |
| + tax<0.03 | 0.11% | 16,384 | 578.3% | 665.3% | **2.6%** |

The crossover is between 7% and 28% selectivity. Below it the mechanism is unusable: the
noise stays sized to the whole domain while the answers shrink with the filter. sf10 is
the same shape but milder (5.1% / 18.1% / 57.3% at the last three rows vs Wilson's
0.2% / 0.8% / 0.5%), and it holds across measures — `COUNT(*)` gives
0.2% → 105.1% and `SUM(l_quantity)` 0.2% → 576.1% over the same ladder.

This is not a bug and there is no clever fix inside the design: a bound that may not
depend on the filter cannot track the filter. Wilson pays privacy budget to get a
query-specific bound and that purchase is exactly what buys selective-query utility. Two
honest ways forward:

- restrict the claim to broad, high-support workloads (whole-domain dashboards), where the
  numbers above are genuinely better than Wilson's; or
- hybrid: take `min(frozen bound, DP-estimated query-specific bound)`, paying budget for
  the second term as Wilson does, with the frozen bound as a cap on how far an analyst can
  steer. That recovers selective-query utility and keeps a bounded steering range, but it
  gives up "no additional privacy budget", which was much of the appeal.

## 5. Single-cell outliers are fully absorbed — good

The expression-attack shape (`case when pu = target then 1e9 else col end`) simulated as
one PU inflated 10⁶-fold in **one** group: nothing moves. Δ̄₁ stays 7,154,829, `D_s` stays
8,388,608, and every mechanism's error is unchanged (0.9% / 1.0% / 1.2%). Per-group
clipping to `B_g` absorbs it before it can reach either bound. So §9's safe-expression
language is defence in depth here, not the only line of defence.

## 6. Coalitions: safe to s−1, then a hard cliff

`N` colluding PUs, each present in every group at 1000× the group median:

| N | Δ̄₁ (`max_u`) | `D_s` (CROWD) | moved |
|---|---|---|---|
| 0 | 7,154,829 | 8,388,608 | — |
| 1 | 41,680,896 | 8,388,608 | — |
| 10 | 41,680,896 | 8,388,608 | — |
| 100 | 41,680,896 | 8,388,608 | — |
| 349 | 41,680,896 | 8,388,608 | — |
| **350** | 12,616,125,070 | **17,179,869,184** | **×2048** |
| 700 | 12,616,125,070 | 17,179,869,184 | ×2048 |

The crowd rule behaves exactly as advertised up to `s−1`, then flips. Two caveats worth
stating in the note:

- The jump is **unbounded** — 2048× here, and it scales with how fat the coalition makes
  itself. Reaching `s` colluding PUs is a utility DoS, not a privacy break (the colluders
  learn nothing about anyone else), but nothing caps the damage.
- The cliff Peter described for hard-zero crowd clipping (6 June) is not removed, only
  moved from values to totals and made `s` times more expensive to reach. Under static
  data it cannot be probed. Under updates (Rem. 10.1) an adversary who adds PUs one at a
  time and watches the noise would detect the crossing.

## 7. Filter stability confirmed, but the channel is coarse

Narrowing the filter around the highest-`c_acctbal` customer (s = 20, sf1):

| filter | PUs left | `B_G` (filtered) | `B_g` (filterless) |
|---|---|---|---|
| `c_acctbal >= -1000` | 99,996 | 1,048,576 | 1,048,576 |
| `c_acctbal >= 5000` | 45,250 | 1,048,576 | 1,048,576 |
| `c_acctbal >= 9800` | 1,822 | 1,048,576 | 1,048,576 |
| `c_acctbal >= 9990` | 100 | 524,288 | 1,048,576 |
| `c_acctbal >= 9998` | 19 | 0 (collapsed) | 1,048,576 |

Filterless is fixed, as claimed. Wilson's moves by one bin level and then collapses once
the surviving population drops below `s`. The instability is real but one bin wide — this
supports the design rather than proving Wilson unsafe.

## 8. Use `f = 2`, not `f = 4`

The bin factor is the price of the CROWD rule: `D_s` overshoots the true norm by up to
`f`. At `f = 4`, `D_s = 16.8M` against Δ̄₁ = 7.2M (134% overshoot) and robustness costs 2×
the noise. At `f = 2`, `D_s = 8.4M` (17% overshoot) and it costs almost nothing (0.9% vs
1.0% on `month`). Doubling the bin count is free.

---

## Caveats

- Sums and counts only, single aggregate, nonnegative. No signed two-histogram case
  (§9.2), no MIN/MAX, no ε split, no AVG ratio.
- `google` is a stand-in for `APPROX_BOUNDS` and a flattering one (spends no budget).
- Adversarial PUs are synthetic. They are legitimate under the note's threat model, and
  skewed real data gives a weaker version of the same effect, but the magnitudes are not a
  measurement of natural skew.
- Static data throughout. Nothing here tests metadata refresh (Rem. 10.1) or the cost of
  the full-domain pass.
- Δ̄₁, `D_s` and `B_g` are still computed from the data and never noised. Findings 2 and 5
  buy robustness, not a way around Assumption 8.1.

## Bottom line

Two of the three moves hold up. The universal bound is self-limiting under wider
groupings, single-cell outliers cannot touch it, and once the norm is crowd-protected the
mechanism beats Wilson on broad queries while being far more stable. The third move —
freezing the bound against the filter — costs selective-query utility catastrophically,
and that cost is structural rather than a defect in the construction. The decision to make
is whether to scope the claim to broad workloads or to accept a hybrid that pays budget for
a query-specific bound under a frozen cap.

## Reproduce

```bash
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --sweep
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --coalition --skew 1000
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --spike 1000000
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --groupby month_priority \
        --skew 1000 --skew-spread
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 -s 20 --attack
python3 attacks/filterless_sim.py --db tpch_sass_sf10.db --sweep
```
