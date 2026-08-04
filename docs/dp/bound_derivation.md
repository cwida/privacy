# Deriving contribution bounds privately

Two proposals for the same open problem, and what simulating them showed.
Reproduce with `attacks/filterless_sim.py` (TPC-H, PU = `customer`, ε = 1, static data).

## The problem

Smooth-sensitivity SASS needs a **public output domain** `Λ`. Without it the smooth
sensitivity of the per-lane median is unbounded, so `Λ` is not optional. Today it is either
supplied by the analyst (`dp_sass_*_output_bound`) or derived from other supplied constants
(`dp_count_bound × ceil(N_PU/64)`). This is the parameter Peter flagged on 6 June:

> Sample&Aggregate with just the 2h median difference as sensitivity is local DP only and
> will be shot down for the same reason as PAC-DB (it leaks noise, because bounds are query
> dependent). We need domain bounds…

So both proposals below are answers to one question: **how do you derive a contribution
bound from the data without leaking it, and without making it query-dependent?**

- **Peter** (10 June, `filterless-crowd-dp-proposal.pdf`): turn predicates into Boolean
  expressions so the bound calculation never sees the filter, bin the full-domain per-PU
  contributions into exponential bins, and take the top bin holding ≥ `s` distinct PUs.
  Freeze it for the session.
- **Dandan** (3 Aug): publish a coarse upper-tail quantile ladder `U_1 … U_k` once. Per
  query, rewrite to filterless with a per-PU flag `P` = "passes the filter", sort PUs by
  contribution descending, privately release the minimum rank with `P = 1`, and use the
  published bound at that percentile.

Both share one architecture: **a public ladder frozen from the full domain, plus a small
per-query DP statistic choosing where on it to clip.** They differ in the ladder's spacing
and in the per-query statistic.

---

## Peter's proposal

### What holds

**The frozen bound is self-limiting under wider groupings.** Identical at every width — 5
groups and 395 groups both give 7,154,829 — because once the per-group bound stops binding,
the norm is just the fattest PU's total footprint, and refining the grouping only splits that
footprint. Wilson's `C_u · B_G` grows with `C_u` (5.2M → 22.0M over the same range), and
lowering `C_u` buys truncation bias instead. Rem. 2.1 holds and strengthens as grouping
widens.

**Single-cell outliers cannot reach the bound.** A 10⁶× spike in one group (the
`case when pu = target then 1e9` shape) moves nothing. So §9's safe-expression language is
defence in depth, not the only line.

**Coalitions are held to `s−1`.** 1, 10, 100, 349 colluding fat-and-wide PUs change nothing;
350 trips it.

### Three things that need fixing

**1. eq. (25)'s `Δ̄₁ = max_u` is not CROWD-protected.** The per-group bound has crowd support;
the norm built from it has none. One PU that is both above `B_g` and present in every group
drags it up — 7,154,829 → 207,093,760 at 395 groups, giving 104% median error against
Wilson's 17%. Wilson survives that case *because* `C_u` truncation caps the norm, which is
the mechanism the note removes.

It is also a **correctness** bug, not just utility. Assumption 8.1 treats the metadata as
public, so a published bound that moves when one PU is removed is a deterministic membership
test:

| | target in | target out | leaks |
|---|---|---|---|
| Δ̄₁ (eq. 25) | 7,154,829 | 6,481,821 | **yes, deterministically** |
| `D_s` (crowd norm) | 8,388,608 | 8,388,608 | no |

Fix: apply `priv_max` one level up — bin the per-PU totals `n_u = Σ_g min(a(u,g), B_g)`, take
the top bin with ≥ `s` support as `D_s`, and ℓ1-clip each PU's contribution vector to it.
Sensitivity is then exactly `D_s`, deterministically, with no random truncation.

**2. A frozen bound cannot track selectivity, and that is structural.**

| filter | selectivity | frozen | + rung selection | Wilson |
|---|---|---|---|---|
| no filter | 100% | 0.4% | 0.5% | 0.9% |
| shipmode AIR/REG AIR | 28.6% | 0.9% | 0.3% | 1.2% |
| + returnflag=R | 7.0% | 1.8% | 0.3% | 0.7% |
| + quantity<10 | 1.3% | 48.3% | 0.5% | 0.9% |
| + discount<0.03 | 0.34% | 186.3% | 0.9% | 2.0% |
| + tax<0.03 | 0.11% | 562.3% | 2.9% | 2.6% |

Below ~7% selectivity the frozen bound leaves the noise sized to the whole domain while the
answers shrink. Same shape at sf10 and for `COUNT(*)` / `SUM(l_quantity)`. No purely frozen
variant escapes it — Wilson's per-query budget spend on `APPROX_BOUNDS` is exactly what buys
selective-query utility.

Fix: the frozen bounds already sit on a ladder, and nothing requires clipping at the **top**
rung. Per query, pay `ε_select` (10% of ε) for a Laplace-noised histogram of the per-PU
*filtered* norms over the ladder — each PU in one bin, so **sensitivity 1** — then choose the
rung on that noisy histogram as post-processing:

```
argmin_r [ Σ_b noisy_b · max(0, f^(b+0.5) − D_s/f^r)  +  |G*| · (D_s/f^r) / ε_value ]
```

Two details, both of which I got wrong first time: **only the norm bound moves with the rung**
(scaling `B_g` too introduces per-group bias a norm histogram cannot predict), and **score
counts, not mass** (a clipped-mass score has sensitivity `D_s`, the same order as the score
differences, so the exponential mechanism picks near-randomly — 56% error at the tail).

**3. Every `count ≥ s` test is a hard threshold.** Both `D_s` and the group universe `G*`. A
bin or group engineered to sit exactly at `s` turns a public quantity into a deterministic
membership test:

| rule | `D_s` MIA | `G*` MIA |
|---|---|---|
| hard `count ≥ s` | **100.0%** | **100.0%** |
| noisy τ, ε_meta=1, m=0 | 64.9% | 67.0% |
| noisy τ, ε_meta=1, m=3 | **50.7%** | **50.6%** |

Use the τ-mechanism already in the extension
(`privacy_mechanisms.cpp:ComputeWilsonPartitionThreshold`), which delivers its stated δ_η
exactly (measured 1.04e-2 against δ=1e-2, 1.10e-3 against 1e-3, across ε_η ∈ {0.1, 1} and
C_u ∈ {1, 10}). **The margin, not the noise, closes it** — at ε_meta = 1 with no margin, 65%
of the signal survives; m = 3 costs τ = 353 instead of 350.

For the **key set** the margin alone is not enough, because one PU can sit in many borderline
groups and removing it flips several decisions at once:

| target in m borderline groups | Laplace(1/ε_η) | Laplace(C_u/ε_η) + Wilson τ |
|---|---|---|
| 1 | 52.8% | 50.0% |
| 10 | 56.7% | 50.0% |
| 50 | **63.3%** | **50.0%** |

So **partition selection still needs a cross-group cap even though the value channel does
not.** The asymmetry to state: **cap the votes, not the values.** A PU may influence at most
`C_u` release decisions while its value contributions stay intact under the ℓ1 bound.
Wilson's `C_u` hurts because it truncates real contributions; used only for the release vote
it costs nothing in the released numbers. Watch the magnitude — τ grows like
`C_u·log(1/δ_η)/ε_η`, which is 853 PUs per group at ε_η=0.1, C_u=10, δ_η=1e-3.

### Ladder granularity is a real trade

| f | `D_s` | no filter | 1.3% sel | 0.11% sel | groups |
|---|---|---|---|---|---|
| 1.25 | 6,077,163 | 1.2% | 1.6% | **19.3%** | 79 |
| 1.5 | 7,371,555 | 1.0% | **0.4%** | **2.2%** | 79 |
| 2 | 8,388,608 | **0.5%** | 0.5% | 2.9% | 80 |
| 4 | 16,777,216 | 0.7% | 0.5% | 5.7% | 80 |

f=1.25 goes backwards: `D_s` = 6,077,163 falls *below* the true max 7,153,466, and a group
loses support. Finer bins hold fewer PUs, so `count ≥ s` selects a lower bin and the noise
saving becomes clipping bias. f=1.5–2 is the optimum; the rung ladder also needs more than 16
rungs below f=1.5.

### Attacks that found nothing

Released answer 51.6% and chosen rung 50.0% (best-threshold MIA, metadata frozen); rung
composition over a crafted 20-query filter family 50.0–50.2%; rung DoS resists injected PUs
to k=300 (the damage at k=1000 is the known `s`-coalition effect); rung MIA 50.0–50.8% down
to a two-PU population. Small-group MIA is 53.5% — **not** a flaw: the contribution is 0.11
noise scales, so the optimal attack is 52.7% and the ceiling for ε_value=0.9 is 71%. Worth
knowing because the median-over-groups metric hides it.

---

## Dandan's proposal

**Exact for filters on the PU entity.** `bound@R_1` equals the tightest valid bound in every
row, necessarily — a passing PU's whole full-domain contribution is in scope, and the sort key
is precisely that quantity:

| filter | R_1 | bound@R_1 | oracle |
|---|---|---|---|
| `c_acctbal >= 2000` | 1 | 7,154,829 | 7,154,829 |
| `c_acctbal >= 8000` | 5 | 6,370,456 | 6,370,456 |
| `c_acctbal >= 9500` | 8 | 6,197,487 | 6,197,487 |

**No adaptivity for filters on fact rows**, which is most of TPC-H:

| filter | selectivity | R_1 | bound@R_1 | oracle |
|---|---|---|---|---|
| no filter | 100% | 1 | 7,154,829 | 7,154,829 |
| shipmode AIR/REG AIR | 28.6% | 1 | 7,154,829 | 2,403,585 |
| + quantity<10 | 1.3% | 1 | 7,154,829 | 70,470 |
| + tax<0.03 | 0.11% | 2 | 6,481,821 | 40,507 |

`R_1` is 1 or 2 at every selectivity, so the bound never leaves the full-domain max while the
oracle falls 176×. A large customer passes any predicate on `lineitem` — it owns enough rows
that some satisfy anything — and the bound then applied is its *full-domain* contribution, not
the part surviving the filter. **The rank identifies which PU is the top passer, not how much
of it passes.** `R_s` barely helps (5.19M vs 40,507). Coarsening to a published `U_i` rounds
*up*, so it can only loosen this further.

**`R_1` also has the wrong sensitivity.** `LS(R_1) ≈ R_1 − 1`: one added or modified PU that
both contributes at the top and passes forces `R_1` to 1. Smooth sensitivity cannot rescue a
statistic whose local sensitivity scales with the magnitude it reports, and the coupling runs
backwards — sensitivity is worst exactly when `R_1` is large, i.e. when the statistic is
informative. `R_s` fixes it: one PU shifts it by one position in the passing order.

**What her ladder does better than ours.** Equal-probability rungs can never lose support —
with k rungs of n/k PUs each, `count ≥ s` holds by construction for any k ≤ n/s. That is
exactly the failure mode our f=1.25 row shows. Proposed synthesis: **coalesce exponential bins
upward until each clears `s`**, rather than discarding unsupported bins. Keeps magnitude
resolution where the data supports it, coarsens automatically where it does not, and retains
tail information the current rule throws away. Bound selection needs resolution in *magnitude*
near the optimum, and a pure quantile ladder is coarse exactly there.

---

## The two ideas together

They are complementary, separated by **whether the filter cuts between PUs or inside one PU's
rows**:

| | ladder indexed by | per-query statistic | right when |
|---|---|---|---|
| Peter + rung selection | filtered contribution (histogram) | the noisy histogram | the filter cuts inside a PU's rows |
| Dandan | full-domain contribution (quantiles) | rank of the top passer | the filter selects whole PUs |

That distinction is in neither write-up and should be.

**Why the histogram route is cheap, and the generalisable point:** reduce the parameter to a
**count query over a public ladder**. Counts have sensitivity 1 regardless of the magnitude of
the quantity being bounded, so the cost lands in the count domain rather than the value domain.
That is why rung selection costs 10% of ε rather than a multiplicative factor. Three rules
follow, and each predicted a defect above before it was found:

1. Never take a bare `max`/`min` over units — use a crowd or quantile statistic, i.e. reduce
   it to a count. (eq. 25; `R_1`.)
2. Never hard-threshold a data-dependent count — noise it *and* add a margin. (`D_s`, `G*`.)
3. Whichever channel is uncapped across cells needs a cross-cell cap, applied to the cheapest
   channel — votes, not values. (Partition amplification.)

**Accounting.** `ε_meta` (and δ_meta) is paid **once per session** for the noised thresholds,
because the metadata is frozen and shared by every query; `ε_select + ε_value` per query. For
each fixed rung the release is `ε_value`-DP and the rung is a function of an `ε_select`-DP
output, so ordinary adaptive composition applies. This removes the need for Assumption 8.1
altogether — the metadata becomes DP rather than assumed public — and the cost has the right
shape: Wilson pays `N · ε_bounds` over N queries, this pays `ε_meta + N · ε_select`. **That is
the defensible pitch — a one-time bound cost instead of a per-query one — not "no budget for
bounds", which the selectivity table shows cannot work.**

## Untested

- Everything here is a **single nonnegative additive aggregate**, sums and counts, static data.
- **Neither idea has been tested inside the smooth-sensitivity SASS pipeline** — both were
  evaluated as bound suppliers in a Laplace setting. Whether a DP-derived `Λ` actually improves
  the smooth-median release is the obvious next measurement, and it needs the extension built.
- MIN/MAX under grouping: Prop. 8.3 is stated for one group; the ℓ1 argument across `G*` is
  missing. AVG as a ratio, and the ε split across `c` aggregates, are also open.
- Metadata refresh: re-deriving after writes means re-paying `ε_meta`, and neither note says
  how stale metadata may be.
- Coalition damage above `s` is unbounded (×2048 measured, scaling with how fat they make
  themselves). Utility DoS, not an inference channel, but uncapped.
- `Δ̄₁`/`D_s` were invariant to grouping width on this data, suggesting one scalar per measure
  rather than per (measure, grouping) — worth confirming on more measures before relying on it.

## Reproduce

```bash
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --sweep --bucketed   # selectivity + rungs
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --knife              # threshold knife-edge
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --partition          # key-set channel
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --suite              # MIA on the fixed mechanism
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --rank               # Dandan, fact filters
python3 attacks/filterless_sim.py --db tpch_sf1.db --sf 1 --rank --entity-filters
```
