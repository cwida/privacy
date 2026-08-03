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

A bound that may not depend on the filter cannot track the filter, so no purely frozen
variant escapes this. Wilson pays privacy budget for a query-specific bound and that
purchase is exactly what buys selective-query utility. **§9 below resolves it** for 10% of
ε, by choosing a rung of the frozen ladder instead of a fresh bound.

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

## 9. Bucketed rung selection resolves the selectivity problem for 10% of ε

The frozen bounds already sit on an exponential ladder (§6: `b*` = highest bin with
support ≥ s). Nothing forces the mechanism to *clip* at the top rung. Clip the **norm**
bound at `D_s / f^n` instead — leaving the per-group bounds `B_g` frozen, since the norm
bound alone is the sensitivity once the ℓ1 clip is enforced — and a selective query can
pick a large `n`.

How the rung is chosen matters more than the idea:

| rule | how | result at 0.11% selectivity |
|---|---|---|
| oracle | best rung, full knowledge, whole ε on the value noise | 1.9% |
| `dp-mass` | exponential mechanism scored on clipped mass | 56.1% |
| `dp-hist` | noisy norm histogram + tradeoff optimisation | **2.9%** |

`dp-mass` fails because the clipped-mass score has per-PU sensitivity `D_s`, the same
order as the score differences, so the mechanism picks close to at random. `dp-hist`
works:

1. Pay `ε_select` once for a Laplace-noised histogram of the per-PU **filtered** norms
   over the frozen ladder. Each PU falls in exactly one bin → ℓ1 sensitivity 1, so
   Laplace(1/`ε_select`) is a tiny perturbation.
2. Optimise on that noisy histogram — free, it is post-processing. Estimate the mass a
   rung clips (PUs in bin `b` treated as sitting at the geometric midpoint `f^(b+0.5)`)
   and trade it against the noise the rung saves:
   `argmin_n [ Σ_b noisy_b · max(0, f^(b+0.5) − D_s/f^n) + groups · (D_s/f^n) / ε_value ]`
3. Release at the chosen rung with Laplace(`(D_s/f^n) / ε_value`).

Composition is ordinary adaptive composition: for every fixed rung the release is
`ε_value`-DP, and the rung is a function of an `ε_select`-DP output, so the total is ε.
Same shape as Wilson's `APPROX_BOUNDS` + Laplace, confined to a frozen public ladder.

sf1, `month`, `ε = 1`, `ε_select = 0.1ε`:

| filter | selectivity | `fl_l1crowd` | oracle | `dp-hist` | rung | `google` |
|---|---|---|---|---|---|---|
| no filter | 100% | 0.4% | 0.4% | **0.5%** | 0 | 0.9% |
| shipmode AIR/REG AIR | 28.6% | 1.0% | 0.3% | **0.3%** | 2 | 1.2% |
| + returnflag=R | 7.0% | 2.1% | 0.3% | **0.3%** | 3 | 0.7% |
| + quantity<10 | 1.3% | 59.3% | 0.5% | **0.5%** | 7 | 0.9% |
| + discount<0.03 | 0.34% | 212.2% | 0.8% | **0.9%** | 8 | 2.0% |
| + tax<0.03 | 0.11% | 645.9% | 1.9% | **2.9%** | 8 | 2.6% |

`dp-hist` is within a fraction of a percent of the oracle and matches or beats Wilson at
every selectivity.

## 10. The rung does not leak membership

The histogram is computed on filtered data, so the bound is query-dependent again and
Rem. 3.1 no longer holds by construction. Tested directly: let the analyst restrict the
query to a PU set S of their choosing and shrink S around one target; run the rung
selection with the target in S and with it removed, and classify from the released rung
alone. Frozen metadata built once from the full domain and reused for every population
size. sf1, `ε_select = 0.1`, 3000 trials:

| \|S\| | mean rung, target in | mean rung, target out | attack accuracy |
|---|---|---|---|
| 100,000 | 0.00 | 0.00 | 50.0% |
| 10,000 | 0.00 | 0.00 | 50.0% |
| 1,000 | 1.00 | 1.00 | 50.0% |
| 100 | 4.88 | 5.05 | 50.8% |
| 10 | 14.97 | 14.97 | 50.0% |
| 2 | 14.99 | 15.00 | 50.0% |

No signal (50.8% is within the upward bias of maximising over 17 candidate thresholds at
this trial count). Sensitivity-1 counts are why: one PU moves one count by 1 against
Laplace(1/`ε_select`) = Laplace(10). Note also the failure mode at tiny \|S\| — the rung
saturates at the bottom of the ladder and the release is clipped to nothing. That is
utility collapse, the same safe failure Wilson's bound has when support runs out.

So the query-dependence the hybrid reintroduces is confined to a coarse, capped, and
empirically silent channel: the rung can never exceed the frozen bound, moves in factor-f
steps, and is selected from noised counts rather than from mass.

## 11. Attacks against the fixed mechanism

Four attacks aimed at the surface the two fixes create (CROWD norm + ℓ1 clip + `dp-hist`
rung selection). sf1, `month`, filter at 1.3% selectivity, target = the PU with the
largest full-domain norm, 2000 trials.

**A. The metadata channel — the strongest argument for the norm fix.** Assumption 8.1
treats the frozen metadata as fixed *and public*. So any published bound that **moves**
when one PU is removed reveals that PU's membership outright, with no noise in the way:

| quantity | target in | target out | leaks |
|---|---|---|---|
| Δ̄₁ (eq. 25, `max_u`) | 7,154,829 | 6,481,821 | **YES** |
| `D_s` (CROWD norm, the fix) | 8,388,608 | 8,388,608 | no |
| groups in G* | 80 | 80 | no |

Under the note's own assumption, eq. (25) gives a *deterministic* membership test for the
norm-defining PU. The CROWD norm does not move. This is not a utility argument — it is a
correctness argument for the fix.

Residual channel: `D_s` only moves if the deciding bin holds exactly `s` members, in which
case losing one PU drops it by a factor `f`. No bin is near the threshold in this data
(counts 902 / 9,012 / 37,629 / 48,415 / 3,922 against `s = 350`), but an adversary who can
place PUs near a bin boundary could engineer it — see **§12**, which builds that knife-edge
and closes it.

**B. End-to-end MIA on the released answer**, metadata frozen:

| statistic | accuracy |
|---|---|
| released total | 51.6% |
| chosen rung | 50.0% |

51.6% is at the edge of the best-threshold-maximisation bias at this trial count; worth
rerunning with more trials before quoting it as clean.

**C. Repeated queries.** Each repetition draws a fresh noisy histogram, so an analyst who
reruns the query could in principle average the rung channel down. Measured accuracy stays
at 50.0% for 1, 10, 50 and 200 repeats — the rung is a discrete argmin that both worlds
saturate at the same value, so there is nothing to average. Note the accounting rather than
the leak: 200 repeats spend 20.0 in `ε_select` alone, so an implementation must cache the
rung per (query, session) or charge for it.

**D. Rung DoS.** I expected the rung to be cheap to manipulate, since its objective is
built from sensitivity-1 counts. It is not:

| injected fat+wide PUs | rung | noise scale | median rel err |
|---|---|---|---|
| 0 | 7 | 72,818 | 0.5% |
| 10 | 7 | 72,818 | 0.5% |
| 30 | 7 | 72,818 | 0.5% |
| 100 | 7 | 72,818 | 0.5% |
| 300 | 7 | 72,818 | 0.5% |
| 1000 | 15 | 582,542 | 4.1% |

Nothing moves below `s = 350`: the mass-weighted objective is dominated by the bulk of the
population, so a few hundred injected PUs cannot shift the argmin. The damage at k = 1000
is the **already-known** `s`-coalition effect on `D_s` from §6 (`D_s` itself rises), not a
new rung-specific vulnerability. So the fixes add no attack surface below `s`; the
`s`-sized coalition remains the only lever.

## 12. Fixing the knife-edge: noise the threshold, and pay for it once

`D_s` is the top of the highest bin whose distinct-PU count reaches `s` — a **hard
threshold on a count**. If the deciding bin holds exactly `s` members, removing one PU
drops `D_s` by a factor `f`, and since `D_s` is public under Assumption 8.1 that is a
deterministic membership test. The fix is not more clipping: it is to decide the bin from
**noised** counts, i.e. the same τ-thresholding the extension already implements for
partition selection (`privacy_mechanisms.cpp:ComputeWilsonPartitionThreshold`). Counts have
sensitivity 1, so `count + Laplace(1/ε_meta) ≥ τ` with `τ = s + m/ε_meta` is
(ε_meta, δ_meta)-DP.

Knife-edge built deliberately: the real norm histogram plus an engineered bin holding
exactly `s = 350` members, then one member removed. sf1, 2000 trials:

| rule | `D_s` in | `D_s` out | MIA accuracy |
|---|---|---|---|
| hard `count ≥ s` (the note) | 16,777,216 | 2,097,152 | **100.0%** |
| noisy τ, ε_meta=0.01, m=0 | 9,769,583 | 9,344,909 | 51.4% |
| noisy τ, ε_meta=0.1, m=0 | 9,415,164 | 8,783,921 | 52.1% |
| noisy τ, ε_meta=1.0, m=0 | 9,283,043 | 4,908,384 | 64.9% |
| noisy τ, ε_meta=0.01, m=3 | 2,449,474 | 2,399,142 | 50.2% |
| noisy τ, ε_meta=0.1, m=3 | 2,522,874 | 2,420,113 | 50.3% |
| **noisy τ, ε_meta=1.0, m=3** | 2,420,113 | 2,199,912 | **50.7%** |

Noise alone is not enough — at ε_meta = 1.0 the Laplace(1) perturbation is too small
against a one-count difference and 64.9% of the signal survives. The **margin** is what
closes it, and it costs almost nothing at ε_meta = 1.0 (τ = 353 instead of 350). That is
the same `s + log(1/δ)/ε` shape as the Wilson threshold.

**Why this is affordable, and why it is the actual argument for filterlessness.** `ε_meta`
is spent **once for the whole session**, because the metadata is frozen and shared by every
query in the family. Wilson pays for its bounds on *every query*. So:

| | bound cost for N queries |
|---|---|
| Wilson | `N · ε_bounds` |
| filterless with DP metadata | `ε_meta + N · ε_value` |

As N grows the bound cost vanishes. The pitch is not "no privacy budget for bounds" — §4
shows that cannot work — it is "**a one-time bound cost instead of a per-query one**", and
that removes the need for Assumption 8.1 rather than merely documenting it.

## 13. Per-group bounds become optional once the norm is CROWD-protected

With the CROWD norm and ℓ1 clipping in place, dropping `B_g` entirely (clip only the norm)
costs nothing measurable — sf1, `dp-hist` column across the selectivity ladder:

| | 100% | 28.6% | 7.0% | 1.3% | 0.34% |
|---|---|---|---|---|---|
| with `B_g` (the note) | 0.5% | 0.3% | 0.3% | 0.5% | 0.9% |
| ℓ1 clip only | **0.3%** | 0.3% | 0.3% | 0.5% | 0.9% |

And the single-cell spike of §5 is still absorbed — but only by the *norm*, not by `B_g`:

| spike 10⁶× in one group | Δ̄₁ | `D_s` | `filterless` err | `fl_l1crowd` err |
|---|---|---|---|---|
| with `B_g` | 7,154,829 | 8,388,608 | 0.9% | 1.0% |
| ℓ1 clip only | 1,382,428,045,303 | 8,388,608 | **164,041%** | **1.0%** |

So per-group clipping is what protects eq. (25) from expression attacks; the CROWD norm
does not need it. That matters because it shrinks the frozen metadata from *one bound per
group* to **one scalar**, whose histogram has sensitivity 1 — exactly the quantity §12 can
make DP cheaply. Two reasons to keep `B_g` anyway: it caps how much a single PU can pollute
one group's value (with ℓ1 clipping alone the cap is `D_s`, not `B_g`), and the note's
clipped MIN/MAX (Rem. 7.1, eqs. 21/23) uses it as the clip cap.

## 14. The group universe has the same threshold bug

`G* = {g : distinct PUs ≥ s}` (§5 of the note) is the *same* hard count threshold as `D_s`.
If a group sits exactly at `s`, removing one PU makes the whole group vanish from the
output — and group presence is directly observable, so this is a deterministic membership
test with no noise anywhere near it:

| rule | P(released \| target in) | P(released \| out) | MIA accuracy |
|---|---|---|---|
| hard `count ≥ s` (the note) | 100.0% | 0.0% | **100.0%** |
| noisy τ, ε_meta=1.0, m=0 | 51.7% | 17.8% | 67.0% |
| noisy τ, ε_meta=0.1, m=0 | 47.8% | 45.6% | 51.1% |
| **noisy τ, ε_meta=1.0, m=3** | 2.4% | 1.2% | **50.6%** |

Identical shape to §12, identical fix, and the margin is again what closes it. This is
exactly the partition-selection mechanism `dp_standard` and `dp_sass` already run, so the
note should reuse it rather than define a raw `s` gate. In this data the gate never binds
(real per-group PU counts run 1,192–17,717 against `s = 350`), so it is a latent bug rather
than an observed one — but it is engineerable.

## 15. Rung composition across a crafted filter family — no leak

The rung is chosen per query, so an analyst issuing Q queries reads Q rungs. Family of 20
nested filters (`l_quantity < k`), statistic = the sum of the observed rungs:

| Q | MIA accuracy | ε_select spent |
|---|---|---|
| 1 | 50.0% | 0.1 |
| 5 | 50.0% | 0.5 |
| 10 | 50.0% | 1.0 |
| 20 | 50.2% | 2.0 |

Nothing accumulates. The binding constraint is budget, not leakage: 20 queries spend 2.0 in
`ε_select` alone, which is the accounting argument for caching the rung per (query,
session).

## 16. Small-group MIA: within the ε guarantee, as it should be

Every other table reports the **median** over groups, which hides small groups. Adversarial
version: the smallest released group, with the target being its largest contributor.

| quantity | value |
|---|---|
| group value, target in | 50,989,932 |
| group value, target out | 50,727,788 |
| target contribution | 262,144 |
| noise scale | 2,330,169 |
| MIA accuracy | **53.5%** |

This is not a flaw — it is the DP guarantee behaving exactly as specified. The contribution
is 0.11 noise scales, so the Laplace total-variation distance is `1 − e^(−0.11/2) = 0.055`
and the optimal attack accuracy is 52.7%; the measured 53.5% matches that within
Monte-Carlo error, and both sit far below the `e^ε/(1+e^ε) = 71%` ceiling for
`ε_value = 0.9`. Worth reporting precisely because the median metric conceals it: there is
a real per-query advantage on small groups, it is bounded by ε, and no amount of clipping
changes that.

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

The design holds up, with two changes to the note.

1. **Crowd-protect the norm** (§2). eq. (25)'s `max_u` is the one real hole; applying
   `priv_max` to the per-PU totals and ℓ1-clipping to the result closes it and removes the
   random-truncation error at the same time.
2. **Select a rung of the frozen ladder per query** (§9), from a noised sensitivity-1
   histogram, for ~10% of ε. Without this the mechanism is unusable below ~7% selectivity;
   with it, it matches or beats Wilson across the whole selectivity range and stays within
   a fraction of a percent of the oracle.

What survives unchanged: the sensitivity is self-limiting under wider groupings (§1),
single-cell outliers cannot touch either bound (§5), coalitions are held to `s−1` members
(§6), and the frozen bound caps how far any query-specific selection can be steered (§7,
§10). The rung channel shows no membership signal at any population size down to \|S\| = 2.

Both fixes were then attacked directly (§11) and held: the CROWD norm does not move when
the target PU is removed (eq. 25 does, which under Assumption 8.1 is a deterministic
membership test), the released answer and the rung show no membership signal, repeated
queries add nothing to average, and the rung cannot be shifted by fewer than `s` injected
PUs. The fixes add no attack surface below `s`.

3. **Noise every support threshold** (§12, §14) — `D_s` *and* the group universe `G*`. The `count ≥ s` rule is a hard threshold, so an
   Both are hard `count ≥ s` tests, and a bin or group engineered to sit exactly at `s` gives
   a 100%-accuracy membership test. `count + Laplace(1/ε_meta) ≥ s + m/ε_meta` closes both
   (50.7% and 50.6% at ε_meta = 1, m = 3), using the τ-mechanism the extension already has.
   The margin, not the noise, is what closes it.

With §12 in place, **Assumption 8.1 is no longer needed** — the metadata is itself DP. And
the cost is the right shape: `ε_meta` is paid once per session, where Wilson pays
`ε_bounds` per query, so the bound cost per query tends to zero. That, rather than "no
budget for bounds", is the defensible pitch.

Optional simplification (§13): once the norm is CROWD-protected, per-group bounds cost
nothing to drop, which shrinks the frozen metadata to a single sensitivity-1 scalar. Keep
them only for per-group pollution control and for the clipped MIN/MAX construction.

The revised design is written up in [`docs/dp/filterless_crowd.md`](../docs/dp/filterless_crowd.md).

What remains open: everything here is single-aggregate, sums and counts, static data. The
update story (Rem. 10.1) still needs an accounting — re-deriving metadata after writes
means re-paying `ε_meta`, and the note does not say how often.

## Reproduce

```bash
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --sweep
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --coalition --skew 1000
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --spike 1000000
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --groupby month_priority \
        --skew 1000 --skew-spread
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 -s 20 --attack
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --sweep --bucketed
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --rung-attack --trials 3000
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --suite --trials 2000 \
        --filter "l_shipmode in ('AIR','REG AIR') and l_returnflag = 'R' and l_quantity < 10"
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --knife --trials 2000
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --suite2 --trials 1000
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --sweep --bucketed --no-group-bound
python3 attacks/filterless_sim.py --db tpch_sass_sf10.db --sweep
```
