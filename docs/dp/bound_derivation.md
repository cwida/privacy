# Deriving contribution bounds privately

Two proposals for the same open problem, and what simulating them showed.
Reproduce with `attacks/filterless_sim.py` (TPC-H, PU = `customer`, ε = 1, static data).

## PRIOR ART — four of the five already exist. Read before writing anything up.

Checked 14 Aug against actual source (729 blobs enumerated from `google/differential-privacy`,
files fetched raw) and primary papers, with an independent refutation pass that re-fetched every
load-bearing quote. **The utility measurements below stand; the novelty claims mostly do not.**

**#1 Frozen group set — SHIPS, in two systems.** Tumult Analytics is the exact technique:
`QueryBuilder.get_groups()` under an `ApproxDPBudget(ε₀,δ₀)`, then `KeySet.from_dataframe()` for
later queries — and their tutorial spends δ once and runs the follow-up under `PureDPBudget`.
Privacy on Beam documents the derive-then-reuse case explicitly in `pbeam/count.go`: *"You use a
differentially private operation to come up with the list of partitions… the output of a
SelectPartitions operation"*, and **enforces** it (`checkPartitionSelectionEpsilon` requires ε=0
with public partitions). Their codelab states the amortisation rationale verbatim: *"privacy budget
on partition selection only once for the entire pipeline."* Wilson et al. §6 lists caching as future
work. Narrow residue: everyone frames this **within one pipeline**; a *persistent* key set reused
across separately-submitted later sessions is not costed anywhere. That is a systems framing, not a
mechanism.

**#2 Adaptive base — a config flag.** `ApproxBounds<T>::Builder::SetBase()`, `SetScale()`,
`SetNumBins()` are all public (`cc/algorithms/approx-bounds.h:731`, default `base_ = 2.0`). The
proposal is literally `.SetScale(lo).SetBase(pow(hi/lo,1.0/64)).SetNumBins(64)`. Residue: Wilson
et al. §5.1.1 *hard-codes* "64-bin logarithmic histogram of base 2" while the library parameterises
it, Java hard-codes `base = 2.0` with no setter, and nothing auto-derives the base from a public
bound. So the contribution is **"use the knob the paper didn't"** plus the 1.68× measurement —
never the capability.

**#3 Gaussian votes — pre-empted comprehensively, and one of my claims is false.**
- Implemented: Google C++ `GaussianPartitionSelection` (`cc/algorithms/partition-selection.h:443`,
  `CalculateStddev(..., sqrt(max_partitions_contributed))` — the √`C_u` argument exactly); Go
  dispatches on `if s.l0Sensitivity > 3 { // Gaussian thresholding outperforms }`; PipelineDP ships
  four strategies including `GAUSSIAN_THRESHOLDING` and `WEIGHTED_GAUSSIAN_THRESHOLDING`; OpenDP
  `make_gaussian_threshold`; Qrlew's `gaussian_tau(...)` is a line-for-line match.
- Published: Gopi et al. (ICML 2020) call it *"a straight-forward extension… ℓ₁-sensitivity by Δ₀
  (and ℓ₂-sensitivity by √Δ₀)"* and use it as a **benchmark**. Desfontaines et al. (PoPETs 2022)
  Fig. 4 benchmarks it and recommends *"weighted Gaussian thresholding for κ ≥ 4… the crossing
  point happens for κ = 3, this stays true for varying ε and δ."* **My measured crossover of
  harmonic-mean `k_u` ≈ 4.3, ε/δ-invariant, is a rediscovery of their κ = 3.**
- **And "no per-user truncation to `C_u` is needed" is definitively false.** DPSU's Weighted
  Gaussian still pre-truncates each user to Δ₀. Gaussian buys `√k_u` instead of `k_u` — it does not
  remove the cap. That sentence must be struck wherever it appears below.

**#4 Count-conditioned shrinkage — the only survivor, and narrowly.** The refuter searched
empirical Bayes, James–Stein, constrained inference, Fay–Herriot and noise-aware Bayesian
post-processing and found nothing using the partition-selection count as a *covariate* for the
co-released sums. Two near misses to pre-empt: **Private-PGM** (McKenna et al.) fuses all noisy
measurements into one coherent estimate — a reviewer will say a count and a sum are just two
measurements; and **"Debiasing Functions of Private Statistics in Postprocessing"** (FORC 2025)
covers private sample sizes and means. Also exists: James–Stein for DP (arXiv 2211.15019).

**#5 ℓ1 per-PU-norm clip — prior art exists, though the report's citations were partly
misattributed.** Harrison & Manurangsi (arXiv 2603.09167) build selection mechanisms under *"Lr
norm constraints on vector contributions… When r = 1 it gives us a drop-in replacement for the
Laplace mechanism"*. PipelineDP's `max_contributions` is the ℓ₁-vs-ℓ₀×ℓ∞ parameterisation choice
and genuinely ships — **but it caps the *number* of contributions, giving `max_contributions ×
max_value`, not a rescaled norm bound `B`**, so it is not our clip. Norm clipping with rescaling is
standard in DP-SGD/FL (Abadi et al.; Andrew et al.) but for gradients, not grouped SQL aggregates.
**This is the least-settled verdict and the one worth a careful manual check before claiming
anything.**

**Two citation errors to fix before anyone checks them:** Google's `partition_selection.md`
describes only truncated geometric — it does *not* document Gaussian (the class exists, the doc
doesn't mention it); and PipelineDP's `NormKind.L1`/`vector_max_norm` governs `VECTOR_SUM`
coordinates, not group keys.

**Net:** this was worth doing and the timing is lucky. What survives is the *measurement* work —
the τ floor proof, the `k_u`-shape scope condition, the resonance finding, the metric critique, the
two privacy bugs — plus one small post-processing idea. The mechanism contributions do not survive.

## SUMMARY — five results, and what each is worth

Read this first; the sections below are in the order they were discovered, and most of the early
headline numbers were later corrected. All figures are relative ℓ1 over the true uncapped key set,
τ-suppressed groups charged full error, **every arm tuned over its own parameters**.

| # | result | gain | whose | where measured |
|---|---|---|---|---|
| 1 | **Frozen group set `G_fix`** — derive the key set once by DP, reuse it, skip τ | **8.2×** (break-even at N=2 queries) | Dandan | τ-binding query, TPC-H sf10 |
| 2 | **Gaussian votes** — partition selection has ℓ1=`k_u`, ℓ2=`√k_u` | **4.5×**, and empty answer → 94% key set | this project | τ-binding queries |
| 3 | **Adaptive-base ApproxBounds** — fixed 64 bins over a narrowed range | **1.68×** worst case, 1.00× alignment spread | Dandan | month\|nation |
| 4 | **Count-conditioned shrinkage** — free post-processing of the τ counts | 1.02×–2.15× | this project | all datasets |
| 5 | **ℓ1 per-PU-norm clip** — the idea this file started from | **3.2×–6.0×** | this project | TPC-H only |

**The baseline is Google DP as published** (Wilson et al. PoPETs 2020 and the library): one `C_u`
truncating values and votes together, Laplace on every channel, ApproxBounds over per-cell values
in base-2 bins. A hardened Google — adding rescale-to-true-total and separate `C_u` for votes vs
values, **neither of which is published anywhere** — would close #5 to 1.2×–1.5× on uniform data,
though not on aligned data (2.3×–4.6×). That belongs in a discussion section, not the headline.

**Scope conditions, all measured, none optional:**

- **#5 needs privacy units that spread across many groups.** On StackOverflow and ClickBench
  (median `k_u` = 1) it gives 0.90×–1.45× and is *worse* than Google on 2 of 8 queries. The
  condition is that Google be *forced* into a large `C_u`; when it can set `C_u`=1 for free, its
  per-cell bound `U` ≈ our norm bound `B` and there is no advantage available.
- **#2 needs the same thing** — crossover at harmonic-mean `k_u` ≈ 4.3 — and it *loses* in the
  smooth-sensitivity (`dp_sass`) path, where the value channel is independently broken so
  releasing more groups is worse (252% → 288%).
- **#1 needs one frozen set per grouping key** and expires when the data changes.
- **#1–#4 do not depend on the ℓ1 clip**, so a reviewer can correctly observe they would improve
  Google DP just as much. They are contributions to the Wilson-et-al. line, not a moat.
- **Nothing is implemented in `src/`.** All simulation.

**Two privacy bugs were found and fixed in our own mechanism** before shipping: the clip was
written as `Σ_g min(t, B)`, a signed sum rather than a norm, which loses ε-DP entirely on signed
measures (39.3% of PUs over bound on an ordinary net-revenue query); and δ was under-charged ~53×
because a PU creates a group by having a *value* there, not a vote. Both fixes are free.

**Methodological finding, and the most transferable result here:** eight separate gains in this
document evaporated once the baseline was tuned as hard as the proposal — 880× → 12× → 4.8× →
4.65× → 1.36×. A DP mechanism comparison at a fixed budget split, a fixed `C_u`, or against a
library's default configuration is not evidence.

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

| filter | selectivity | min passing PUs/group | frozen | + rung selection | Wilson |
|---|---|---|---|---|---|
| no filter | 100% | — | 0.4% | 0.5% | 0.9% |
| shipmode AIR/REG AIR | 28.6% | 818 | 0.9% | 0.3% | 1.2% |
| + returnflag=R | 7.0% | ~100s | 1.8% | 0.3% | 0.7% |
| + quantity<10 | 1.3% | 12 (2/42 groups fail τ) | 48.3% | 0.5% | 0.9% |
| ~~+ discount<0.03~~ | 0.34% | — | — | — | — |
| ~~+ tax<0.03~~ | 0.11% | 1, median 172 — **all 42 fail τ** | — | — | — |

**The last two rows are outside the mechanism's support and must not be read as results.** At
0.11% selectivity every group has fewer than τ ≈ 380 distinct passing PUs (median 172, min 1),
so per-query τ-thresholding suppresses all of them — the correct output is empty, and the
2.9% / 562% figures previously recorded there describe an output no correct mechanism would
release. Google DP suppresses the same groups for the same reason, so the comparison is moot
on both sides.

Within the supported range the finding stands and is if anything cleaner: at 1.3% selectivity,
where 40 of 42 groups are releasable, the frozen bound gives **48.3%** and rung selection
**0.5%**. Below ~7% the frozen bound leaves the noise sized to the whole domain while the
answers shrink; no purely frozen variant escapes it. Same shape at sf10 and for `COUNT(*)` /
`SUM(l_quantity)`.

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

## Which one to use

**One mechanism, not both.** They share an architecture, so they do not compose usefully, and
the histogram statistic dominates the rank statistic:

- It strictly contains the rank's information — the coarse filtered contribution distribution
  versus one point of it.
- It costs the **same ε as a single count**: each PU falls in exactly one bin, so the bins are
  disjoint and Laplace(1/`ε_select`) per bin is `ε_select`-DP for the whole histogram by
  parallel composition. There is no budget saving in using a scalar.
- Sensitivity 1 against `LS(R_1) ≈ R_1`.
- Works for filters that cut inside a PU's rows as well as filters that select whole PUs.

Running both and taking the tighter bound would split ε, and the histogram at half budget beats
the rank at full budget. **What to take from Dandan's proposal is the ladder construction** —
equal support per rung — as the bin-coalescing rule below.

### Filters that pass too few PUs

Neither bound rule works when a group has very few passing PUs — the histogram of ~10 filtered
norms is pure noise, the rung is chosen at random, and the release clips to nothing. But the
fix is not a better bound rule: it is **per-query τ-thresholding on the filtered distinct-PU
count**, which suppresses the group outright. That converts silent garbage into an honest empty
result, and it is the mechanism the extension already runs for `dp_standard` / `dp_sass`.

Measured, distinct **passing** PUs per group across the ladder (τ ≈ 380 at s = 350, ε_η = 0.1):

| filter | selectivity | min | median | groups failing τ |
|---|---|---|---|---|
| shipmode AIR/REG AIR | 28.6% | 818 | 12,025 | 0 / 80 |
| + quantity<10 | 1.3% | 12 | 1,826 | 2 / 42 |
| + tax<0.03 | 0.11% | 1 | 172 | **42 / 42** |

So the usable range is roughly ≥1% selectivity on this schema, and the mechanism refuses
cleanly below it rather than degrading.

Three consequences:

- **This supersedes the "do not omit empty groups" rule in the per-query procedure.** That rule
  is only needed when the *frozen* `G*` is the sole gate: omitting a group because its filtered
  value is zero would leak that no PU passed. With a per-query τ gate, the omission is done *by*
  a DP mechanism under budget, so it is sanctioned.
- It costs a third per-query channel, `(ε_η, δ_η)`, and needs the `C_u` **vote** cap, since one
  PU can be borderline in many groups (52.8% → 63.3% MIA as that count goes 1 → 50).
- **Charge `ε_select` and `ε_η` unconditionally**, including when the result comes out empty. If
  the system skips the charge on an empty result, the remaining-budget ledger becomes a side
  channel telling the analyst whether any group cleared τ.

## Procedure

### Once per dataset, per (measure, grouping) template

"Universal" means universal across **filters**, not across measures or groupings, so this runs
per template. Parameters: `s` (crowd size), `f` (1.5–2), `C_u`, `ε_meta`, `δ_meta`.

1. **Materialise the full-domain per-PU contribution relation** `a(u,g)`: pre-aggregate the
   join by (PU, grouping key) with **no analyst filters** — PK–FK join predicates do not count
   as filters. This is the IVM-maintained view. Optionally apply §4 sampled filter retention
   (keep all active PUs, hash-sample inactive ones on `r` bits, requiring support `s/2^r`);
   that costs utility only, never privacy.
2. **Group universe `G*`**: per-group distinct-PU counts, released through
   `count + Laplace(C_u/ε_G) ≥ τ`, with `τ = max(s, wilson_τ(ε_G, δ_G, C_u))`. Enforce the
   `C_u` **vote** cap here (the existing rank cap, `ApplyMaxGroupsContributed`) — one PU
   touches up to `k_u` groups, so this table's sensitivity is `k_u` without a cap.
3. **Per-group bound `B_g`**: bin `a(u,g)` by powers of `f` within each group; count distinct
   PUs per (group, bin); take the top bin whose noised count clears τ, **coalescing bins upward
   until each clears τ** rather than discarding unsupported ones. Same `C_u` cap and the same
   sensitivity argument as step 2. Optional — see §13: with the crowd norm in place this can be
   dropped, which removes the only `C_u`-costing value metadata and leaves one scalar.
4. **Crowd norm `D_s`**: roll up per-PU totals `n_u = Σ_{g∈G*} min(a(u,g), B_g)`, bin those the
   same way, take the top bin clearing τ. **Sensitivity 1** — each PU lands in exactly one bin —
   so this is the cheap one.
5. Publish `(G*, {B_g}, D_s)` and the ladder definition (`f`, `s`, bin edges). Charge
   `ε_meta = ε_G + ε_B + ε_D` and `δ_meta` **once**, not per query. Re-derivation after writes
   re-charges it.

The structural asymmetry worth remembering: `D_s` has sensitivity 1; `G*` and `B_g` are
per-group and cost `C_u`.

### Per query

1. **Compatibility check** (`privacy_compatibility_check.cpp`) plus the safe-value-expression
   rules: no Boolean→numeric path, no partial or error-generating functions, totalised
   division, equality-only column-vs-column joins on released tables.
2. **Resolve the template** (measure expression + grouping keys) and load its frozen metadata.
   No template, no query.
3. **Pre-aggregate per PU with the filter** → `t(u,g)`, restricted to `g ∈ G*`. Note there is
   **no filterless rewrite in the query path** — freezing per template moves Peter's §3/§4
   machinery (Boolean predicates, active bit, hash sampling) into the metadata build.
4. **Clip to `B_g`** (if kept).
5. **Rung selection**: histogram `n_u^t = Σ_g min(t(u,g), B_g)` over the frozen ladder, add
   Laplace(1/`ε_select`) per bin, and choose `r` by
   `argmin_r [ Σ_b noisy_b · max(0, f^(b+0.5) − D_s/f^r) + |G*| · (D_s/f^r) / ε_value ]`.
   Everything after the noised histogram is post-processing. **Cache `r` per (query, session)**
   so a rerun does not re-charge `ε_select`.
6. **ℓ1-clip** each PU's vector to `D_r = D_s / f^r`, via the window scale factor
   `min(1, D_r / n_u^t)` inside the per-PU pre-aggregation.
7. **Sum per group over all of `G*`** — including groups with no passing rows. Do **not** omit
   empty groups the "usual SQL way" as §5 of the note suggests: `G*` is public and frozen, so
   omitting a group because its filtered value is zero reveals that no PU in it passed the
   filter, which reintroduces a per-query data-dependent key set. Empty groups must come out as
   noise around zero.
8. **Add Laplace(`D_r / ε_value`)** per group, splitting `ε_value` across the `c` aggregates
   (`FinalizeDPLaplace` already does this), then the AVG ratio projection if needed.

Budget: `ε_select + ε_value` per query, `ε_meta` once for the session.

### How do we know the grouping key for a whole dataset?

As written above the metadata is per (measure, grouping) template, which is a real problem: you
cannot materialise metadata for grouping keys nobody has asked for yet. Three answers, and the
third makes the question go away.

1. **Declare the templates** in DDL, alongside `PRIVACY_KEY` / `PRIVACY_LINK`, and restrict
   analysts to them. Honest, matches Peter's "template family" language, and is how OLAP cubes
   work — but it kills ad-hoc querying.
2. **Build on first use**, charging `ε_meta` when a new template appears. The set of built
   templates depends on query *text*, which is the analyst's own input, so it leaks nothing.
   But the amortisation becomes `T·ε_meta + N·ε_select` for T templates, which only beats
   Wilson's `N·ε_bounds` when N ≫ T — fine for dashboards, poor for exploration.
3. **Drop the grouping dependence entirely.** This is the right answer, and it falls out of two
   results already measured:

   - `D_s` came out **identical across every grouping tried** — 5, 27, 80 and 395 groups all
     gave 8,388,608 at f = 2. Not a coincidence: with `B_g` not binding,
     `n_u = Σ_g a(u,g)` is just PU `u`'s **total** contribution to the measure, and refining a
     partition cannot change a sum over it. Drop `B_g` and it holds by construction, not
     empirically.
   - `B_g` can be dropped at no measurable utility cost (§13), and per-query τ-thresholding
     replaces the frozen `G*` as the release gate (above).

   Together those remove both grouping-dependent quantities. What is left is **one scalar per
   measure — a crowd bound on the per-PU total contribution — with no grouping key anywhere.**
   The ℓ1 argument is unaffected: a PU's released vector still has norm `min(n_u, D_r) ≤ D_r`
   regardless of how the rows are grouped. Only `|G*|` enters the rung objective, and that is
   the query's own output group count, known at plan time, not metadata.

That collapses the dataset phase from five steps to two: materialise per-PU **totals** per
measure (no grouping key), and release the crowd bound on them through a noised τ. Since it is
one number per numeric column, it can be precomputed for the whole schema and maintained with
the table — the same shape of statistic as the MinMax stats Peter noted we already have on all
base columns, just DP-released once.

The remaining limit: measures that are *expressions* (`l_extendedprice * (1 - l_discount)`)
cannot be precomputed per column. Those fall back to build-on-first-use, or to a loose bound
composed from the column bounds. Worth measuring which of the two is better before choosing.

### Where this plugs into smooth-sensitivity SASS

Steps 1–6 are a **`Λ`-derivation**, and that is the whole point of the exercise. To use it with
the smooth-median release rather than a Laplace sum, replace steps 7–8 with the existing SASS
path and set the public output domain from `D_r` instead of from an analyst constant — via the
relation already used for the current bounds,
`Λ = D_r × ceil(N_PU / m)`, so `D_r` substitutes for `dp_count_bound` / `dp_sum_bound`. That
removes the query-dependence of `Λ` that Peter's 6 June objection targets, and it generalises
`dp_sass_private_range` (currently exponential-mechanism quantiles for the average release
only) to the median release. **Untested — see below.**

## Adaptive clipping: choosing the bound by the tradeoff, not by a quantile

Every rule above targets a fixed quantile or a fixed count. The optimal bound is neither: it
depends on the ratio of noise to group total, so it moves with **ε**, with the **number of
PUs**, and with **grouping granularity**. Fixed rules ignore all three and drift away from
optimal as any of them changes:

| axis, held otherwise fixed | fixed-α rule | adaptive |
|---|---|---|
| ε from 0.1 → 10 (ClickBench) | 1.4× → **20.0×** oracle | 1.6× → **1.0×** |
| grouping 395 → 7 groups (TPC-H) | 1.0× → **17.8×** | 1.0× → **1.0×** |
| PUs 10k → 1M (TPC-H sf10) | 1.0× → **15.7×** | **1.0×** throughout |

The rule: from the same noisy histogram ApproxBounds already builds, minimise
`estimated clipped mass + noise`, weighting the noise term by the **median group total**
(clipping removes a roughly constant *fraction* of every group, but noise is the same
*absolute* amount in each, so the median group sets the tradeoff). The median group size
comes free from the per-group PU counts partition selection already releases. Costs nothing
beyond what ApproxBounds spends. Ranks first on TPC-H (2.8 mean rank, 1.1× oracle) and
StackOverflow (1.9, 1.1×), and beats ApproxBounds on ClickBench (2.5× vs 4.5×).

**Its one weakness, and why the obvious fix fails.** On cells where group totals span ~200×
(ClickBench: 922 / 12,006 median / 2,523,201) it lands at ~2× oracle rather than ~1×, because
the constant-fraction assumption breaks — heavy PUs concentrate in the large groups, so the
global clip fraction (9.4% at the oracle bound) far overstates the median group's (≈1%).
Estimating the clip loss **per group** is the right target — with the true per-group clip
loss the rule is 1.0–1.4× everywhere, better than global in every cell — but it cannot be
estimated privately: a 2-D (group × bin) count histogram gives 5.8–18.4× where the global
rule gives 1.0–1.1×, and no choice of bin representative (lower edge, geometric midpoint,
upper edge) fixes it. Supplying true per-group *masses* does not help either, so the blocker
is the within-bin shape, which differs per group and is precisely what a count histogram
discards. Same wall as certifying tail mass, one level down: aggregate quantities are
privately estimable, per-group tail quantities are not.

## Histogram-based τ-thresholding (Dandan, 11 Aug)

Reuse the per-group histogram already released for bound selection to decide group
existence: release group `g` iff `max_i c̃_{g,i} ≥ τ`. Since the histogram is already DP,
the decision is post-processing, so partition selection costs no extra budget. τ solves
`(1 − ½e^{−(τ−1)/b})(1 − ½e^{−τ/b})^{B−1} = (1−δ)^{1/C_u}` with `b = 2C_u/ε_b`.

**It works, and the gain is exactly the freed ε_η.** End to end at C_u = 1, ε = 1 split
0.25/0.25/0.5 (standard) vs 0.25/0.75 (hers):

| | groups released | value ε | median error |
|---|---|---|---|
| TPC-H, standard | 100.0% | 0.50 | 0.5% |
| TPC-H, hers | 98.8% | 0.75 | **0.4%** |
| ClickBench, standard | 100.0% | 0.50 | 1.1% |
| ClickBench, hers | 100.0% | 0.75 | **0.8%** |
| StackOverflow, standard | 91.0% | 0.50 | 2.8% |
| StackOverflow, hers | 84.8% | 0.75 | 2.7% |

27% and 20% better where groups are large; a wash on StackOverflow where the lost groups
cancel the saving. In our implementation `ε_η = ε/(c+1)`, so for a single aggregate τ is
eating half the budget and the ceiling is nearer 50% than the 33% above.

**Applicability is set by C_u, not by group size.** τ scales linearly in `C_u/ε_b`:
C_u = 1 gives τ = 138 (works on ~350-PU groups); C_u = 10 gives τ = 1,567 (needs a few
thousand). On TPC-H, with 12k+ PUs per group, it releases 98.7–100% at every C_u tried.

**Three edge cases.**

*It keys on distribution shape, not group size.* The rule needs `max_i c_i ≥ τ`, and the
modal bin holds a fraction f of the group. At C_u = 1, τ = 138: a **400-PU** group with all
PUs in one bin releases 100% of the time, while a **2,000-PU** group spread uniformly is
suppressed 0% of the time. Worst case f = 1/B, so guaranteed release needs `n_g ≥ B·τ`
(8,849 here). Measured f on real data is far better than worst case — median 0.88 for small
count measures (ClickBench, StackOverflow), 0.33–0.49 for wide-valued sums, and 0.17 for a
heavy-tailed measure (StackOverflow `views`), which is where it is most expensive.

*The histogram must count distinct PUs, not rows.* The δ analysis assumes a singleton group
has counts (1, 0, …, 0). If the histogram bins rows, one PU with ~200 rows in a bin gives
`P(release) = 99.98%` against a target δ of 1e-6 — the guarantee is gone. Per-PU
pre-aggregation before binning is load-bearing, not an optimisation.

*The joint histogram does **not** degrade the bounds.* Its sensitivity is `2C_u` rather than
2, so it might be expected to cost bound quality at C_u > 1. Measured, the selected bound is
identical at C_u = 1, 5 and 10 on both datasets — bin counts are large enough to absorb 10×
noise. So the only cost is the threshold itself.

*Systematically stricter, never looser.* Since `max_i c_i ≤ n_g` and `τ_max > τ_single`, the
rule can never release a group the standard rule would suppress. The gain is purely the
freed budget, and it is largest for single-aggregate queries (where `ε/(c+1)` is half the
budget) and shrinks as the number of aggregates grows.

## Half-dataset splitting (Dandan, 11 Aug)

Randomly partition PUs into two disjoint halves, evaluate `t/2` attributes on each. A PU
then affects `C_u·t/2` cells instead of `C_u·t`, and parallel composition across the halves
doubles the per-cell budget to `2ε/(C_u t)`. The accounting is correct; the utility is not.

**SUM / COUNT: exactly neutral, then strictly worse.** The cell value is `V/2`, so the
answer must be rescaled by 2, and `2·Lap(B/2ε₁) = Lap(B/ε₁)` — the doubled budget is
*exactly* cancelled by the rescaling. Measured: identical noise (0.023% either way), and
subsampling then adds **8.2×** the error.

**AVG: same cancellation** (the ratio is scale-free, so the relative noise is unchanged),
plus sampling error — 3.8× worse.

**Median: a real but modest win**, and only in a window. The smooth sensitivity grows by
~1.6× when the data halves while the budget doubles, so the net is ~0.85×:

| m | endpoint term | SS full | SS half | err full | err half |
|---|---|---|---|---|---|
| 256 | 2,912,797 | 1,506,602 | 1,497,136 | 68.9% | 36.0% |
| 1024 | 5 | 280,916 | 454,852 | 12.7% | **10.5%** |
| 4096 | 0 | 150,142 | 262,692 | 7.3% | **6.3%** |
| 16384 | 0 | 61,597 | 120,901 | 3.7% | 6.0% |

The apparent 2× at m = 256 is inside the Λ-dominated regime where the release is unusable
anyway. It reverses at m = 16384 where the lanes get too thin. General rule: splitting helps
exactly when the sensitivity grows slower than the budget doubles.

## Cross-attribute bound correspondence (Dandan, 11 Aug)

Store a frozen mapping between attributes' contribution bins, pay for one attribute's bound
and derive the rest. Offset `d = bin(a₂) − bin(a₁)` per PU, on TPC-H:

| filter | price→quantity: d, corr | price→count: d, corr |
|---|---|---|
| no filter | −11.0, 1.00 | −15.0, 0.98 |
| shipmode AIR | −11.0, 0.99 | −15.0, 0.95 |
| + returnflag=R | −11.0, 0.99 | −15.0, 0.85 |
| + quantity<10 | −10.0, 0.97 | **−12.0, 0.62** |

It survives filters *independent* of the attributes and breaks under filters *correlated*
with them: filtering on quantity shifts price→count by 3 bins, i.e. a derived bound wrong by
**8×**, with the correlation collapsing from 0.98 to 0.62. Silent, and analyst-steerable —
picking a filter correlated with `a₁` is enough to corrupt `a₂`'s bound.

### Half-dataset: the rest of the picture

- **More parts is worse.** Splitting into k parts gives k× the per-cell budget and the same
  exact cancellation: `k·Lap(B/(k·ε₁)) = Lap(B/ε₁)` for any k. Measured noise-only error is
  flat at 0.046% for k = 1, 2, 4, 8, 16, 64; only the subsampling error grows, as ~√k.
- **τ-suppression is neutral**, the cost she deferred. Halving the PUs halves every group,
  but the doubled budget also halves τ, so the release test is the same inequality scaled by
  ½. Measured, it is very slightly *favourable*: StackOverflow 91.4% → 93.7%, ClickBench
  region 17.5% → 21.7%.
- **The median gain equals the Λ-fraction of the smooth sensitivity.** At m = 64 the SS is
  41,201,809 full against 41,224,952 half — identical, because it is entirely the
  `exp(−βm/2)·Λ` endpoint, which does not depend on how much data there is. So the split is
  free and the budget doubling is pure gain (0.53×) — on an answer that is 1884% wrong. As
  m grows the endpoint dies, SS comes from the lane gaps which *do* widen when halved, and
  the gain decays: 0.51× (m=128), 0.50× (256), 0.70× (1024), 0.96× (4096), 1.60× (16384).
  The 2× and usability are mutually exclusive.

### Cross-attribute correspondence: correlation is necessary, not sufficient

| pair | corr (unfiltered) | worst shift under a filter | derived bound error |
|---|---|---|---|
| TPC-H price→quantity | 1.00 | 0 bins | 1× |
| ClickBench count→width | 0.92 | 0 bins | 1× |
| TPC-H price→count | **0.98** | **2 bins** (under `quantity<10`) | **4×** |
| StackOverflow count→views | 0.47 | 2 bins (under `views>500`) | 4× |

Price→count has 0.98 correlation and still breaks, so correlation does not predict safety.
What predicts it is whether the *filter* selects on the ratio `a₂/a₁`: `quantity<10` is
proportional to price but not to count, so it decouples them. The correspondence is safe
when the ratio is near-constant **by construction** (a schema property, e.g. price =
quantity × bounded unit price) and unsafe when it varies across rows — and since the analyst
picks the filter, the unsafe case is steerable.

### τ statistics: the max is already the best simple choice

Six statistics calibrated to the same null (`P(release | singleton) ≤ 1e-3`, B = 64,
Lap(8)), scored by the fraction of real groups released:

| statistic | τ | TPC-H | StackOverflow | ClickBench |
|---|---|---|---|---|
| **max (Dandan)** | 83.4 | **100.0%** | **86.8%** | **12.5%** |
| sum of top 3 | 154.3 | 100.0% | 86.2% | 12.0% |
| sum of top 8 | 262.6 | 100.0% | 85.5% | 11.2% |
| sum of all bins | 283.6 | 100.0% | 76.5% | 8.0% |
| # bins ≥ 3b | 7.0 | 99.6% | 2.0% | 0.8% |
| # bins ≥ 2b | 12.0 | 74.0% | 0.7% | 0.4% |

The max wins because the null is "one bin at 1, the rest pure noise" and real groups have a
dominant bin (measured modal fraction 0.33–0.88). Combining counts does not reduce τ.

## Noise mechanism: when Gaussian beats Laplace

Independent of the bound, the noise distribution is a free choice. The switching rule turns
out to be a single quantity.

Under an ℓ1 clip to `B` the L1 sensitivity is `B`, so Laplace at `B/ε` gives std `√2·B/ε`.
Under a **joint ℓ2 clip** (normalise each aggregate by its own bound, concatenate, clip the
vector's ℓ2 norm) the L2 sensitivity is `B/√k_eff` where

    k_eff = (‖v‖₁ / ‖v‖₂)²   — the *effective* number of groups a PU spreads across

and ρ-zCDP Gaussian gives `σ = √(c/k_eff) / √(2ρ)` with `ε = ρ + 2√(ρ·ln(1/δ))`. Hence

    Gaussian / Laplace  =  3.75 / √(c · k_eff)      →  **Gaussian wins when c·k_eff > 14**

(at ε = 1, δ = 1e-6; the constant is `√(2ln(1.25/δ))/√2` and is nearly flat in ε — 3.78 at
ε=0.5, 3.75 at ε=1, 3.85 at ε=2 — so there is no ε crossover for a single scalar.)

**Measured k_eff on TPC-H** (per-PU contribution vectors, AIR filter):

| grouping | groups | median groups/PU | median k_eff |
|---|---|---|---|
| year | 7 | 5 | 3.8 |
| quarter | 27 | 8 | 5.6 |
| month | 80 | 9 | 6.4 |
| month × priority | 400 | 9 | **6.7** |

`k_eff` **saturates around 6–7** and barely moves from 80 to 400 groups, because a customer's
spend is dominated by a few months — refining the grouping splits it unevenly, so effective
dimensionality does not grow. So the ℓ2 route alone never reaches 14 on this data.

**But the two routes multiply.** With `k_eff = 6.7`:

| aggregates c | verdict |
|---|---|
| 1 | Laplace 1.46× better |
| 2 | tied (1.03×) |
| 3 | **Gaussian 1.19× better** |
| 4 | **Gaussian 1.37× better** |
| 6 | **Gaussian 1.68× better** |

AVG alone is two cells (sum + count), and a query with SUM, COUNT and AVG is four — so the
common multi-aggregate case sits where the *noise formula* favours joint ℓ2 clipping and
Gaussian.

**Measured end-to-end, it does not.** Four aggregates on TPC-H month (SUM price, SUM
quantity, SUM discounted price, COUNT), normalised per-PU vectors over all 4×80 cells giving
median `k_eff = 24.1` — far above the `3.75²/c = 3.5` the formula requires:

| mechanism | error |
|---|---|
| per-aggregate ℓ1 clip + Laplace | **1.227%** |
| joint ℓ2 clip (R=1.0) + Gaussian | 1.272% (0.96×) |
| joint ℓ2 clip (R=1.5) + Gaussian | 1.938% |
| joint ℓ2 clip (R=2.0) + Gaussian | 2.589% |

The formula predicts Gaussian 2.6× better here and it comes out 0.96× — a wash. The noise
analysis is right; the **bias** cancels it. ℓ2 clipping preferentially shrinks *concentrated*
PUs and spares spread ones, while ℓ1 treats equal totals equally, so at matched noise the ℓ2
arm carries a different and here larger bias. **A noise-std comparison is not a mechanism
comparison** — the same lesson as the fixed-budget-split trap, in a different guise.

## The budget split — and a correction to the τ-reuse result

**Bound selection needs almost no budget.** Sweeping ε_b at ε_total = 1:

| ε on bounds | TPC-H bound picked | TPC-H error | StackOverflow error |
|---|---|---|---|
| 0.50 (`bounded-sum.h` default) | 2,097,152 | 0.51% | 10.26% |
| 0.10 | 2,097,152 | 0.28% | 7.38% |
| **0.05** | 2,097,152 | **0.27%** | **7.29%** |
| 0.01 | 2,097,152 | 0.25% | 7.22% |

The selected bound is *identical at every split* on TPC-H — it is a choice among ~15 log₂
bins on counts in the thousands, so noise cannot move it. Google's `SetEpsilon(epsilon/2)`
default therefore over-spends by roughly 2×. This is a tuning fix available to both
mechanisms, not a differentiator. Below ~0.02 the selection destabilises (StackOverflow
wobbles between bins 16/32/64), so ~0.05 is the safe operating point.

**This overturns the τ-reuse gain.** Reusing the histogram for τ forces ε_b to stay large,
because `τ ∝ 2C_u/ε_b`. Jointly optimising the split at C_u = 1:

| scheme | ε_b | τ | released | error |
|---|---|---|---|---|
| TPC-H, τ reuse | 0.10 | 346 | 98.8% | 0.30% |
| TPC-H, **separate τ** (ε_b=0.05, ε_η=0.05) | 0.05 | 263 | 98.8% | **0.30%** |
| StackOverflow, τ reuse | 0.50 | 69 | 88.3% | 2.72% |
| StackOverflow, τ reuse | 0.10 | 346 | 28.5% | 1.80% |
| StackOverflow, **separate τ** (ε_b=0.05, ε_η=0.1) | 0.05 | 132 | 87.2% | **2.42%** |

**τ-reuse trades ε_η for ε_b at roughly 1:1, so it is a wash.** The 20–27% gain measured
earlier was an artefact of a fixed 0.25/0.25/0.5 split in which ε_η was over-funded.
τ-thresholding is cheap — ε_η ≈ 0.05–0.1 suffices — so freeing it buys little, while the
reuse costs the values by pinning ε_b high. On TPC-H the two land exactly level; on
StackOverflow the separate τ is better on both released groups and error.

## Cap the votes, not the values — measured (and a methodological correction)

Under an ℓ1 clip a PU's released vector has ℓ1 norm ≤ B **regardless of how many groups it
touches**, so `Laplace(B/ε_v)` is ε_v-DP with no `C_u` anywhere. `C_u` is needed only for the
τ **votes**, whose count vector has ℓ1 sensitivity `C_u`. Splitting the two:

- values: no group cap, ℓ1-clip to B, `Laplace(B/ε_v)`
- votes: rank-cap each PU to `C_u` groups, `Laplace(C_u/ε_η)`, Wilson τ
- bounds: histogram of per-PU norms, one bin per PU, `Laplace(2/ε_b)`

**Correction to every earlier `C_u` number in this document.** Those runs scored the release
against the *capped* truth, which forgives exactly the error truncation causes. The analyst
asked for the uncapped answer; Google's paper calls the difference `Error_{C_u}`. Rescored
against the true answer (ε_b = 0.05, ε_η = 0.1, ε_v = 0.85):

| dataset | C_u | cap values **and** votes | cap **votes only** | gain |
|---|---|---|---|---|
| TPC-H quarter | 1 | 73.58% | **0.10%** | 736× |
| TPC-H quarter | 5 | 21.61% | **0.10%** | 216× |
| TPC-H month | 1 | 76%* | **0.29%** | — |
| TPC-H month | 5 | 27.49% | **0.29%** | 95× |
| TPC-H month | 19 | 0.29% | 0.30% | tie |
| StackOverflow | 1 | 40.98% | **7.12%** | 5.8× |
| StackOverflow | 5 | 13.08% | **5.29%** | 2.5× |

(*mass kept 23%, so the bias floor is ~77%.) The two coincide only when `C_u` is large enough
that no truncation happens.

This dissolves the "`C_u` tension" recorded earlier — that small `C_u` is forced on
small-group data while large `C_u` is needed to keep the mass. With vote-only capping you
take `C_u = 1` for the cheapest τ **and** keep 100% of the values. StackOverflow at `C_u = 1`
then gives 87.6% of groups released at 7.12% error, the best configuration measured anywhere
in this work.

## Head to head with Google DP, and where the gain actually comes from

ε = 1, scored against the **uncapped** truth. Google DP: ApproxBounds on per-(PU,group)
partials, `C_u` truncation, `Laplace(C_u·U/ε_v)`, 1/3 split. Ours: adaptive bound on per-PU
norms, ℓ1 clip with no value cap, `Laplace(B/ε_v)`, vote-capped τ, split 0.05/0.10/0.85.

| dataset / grouping | C_u | Google DP | ours | gain |
|---|---|---|---|---|
| tpch price/month | 1 / 5 / 19 | 89.51% / 49.41% / 3.57% | 0.30% / 0.30% / 0.29% | 301× / 165× / 12× |
| tpch price/quarter | 1 / 5 / 19 | 88.14% / 43.15% / 1.19% | 0.10% / 0.21% / 0.10% | 880× / 210× / 12× |
| tpch count/month | 1 / 5 / 19 | 89.51% / 49.45% / 4.21% | 0.35% / 0.35% / 0.35% | 256× / 142× / 12× |
| StackOverflow count/month | 1 / 5 | 44.36% / 38.51% | 7.13% / 5.15% | 6.2× / 7.5× |
| ClickBench count/date | 1 / 4 | 4.37% / 13.85% | 0.75% / 0.73% | 5.8× / 19× |

**Decomposed** (tpch price/month), turning on one change at a time:

| cumulative change | C_u = 5 | C_u = 19 |
|---|---|---|
| Google DP as shipped | 49.37% | 3.58% |
| + tuned budget split | 49.40% (1.0×) | 1.42% (2.5×) |
| + **ℓ1 clip instead of C_u truncation** | **0.29% (168×)** | **0.29% (12.4×)** |
| + adaptive bound instead of ApproxBounds | 0.29% (—) | 0.29% (—) |

> **CORRECTED BELOW — see "Google DP's best configuration".** The table above runs Google DP
> at a fixed `C_u` and a fixed 1/3 split. Tuned over both, Google reaches 1.34% and the
> honest gap is **4.6×**, not 12–880×.

**The ℓ1 clip is essentially the entire gain.** Budget tuning is worth 2.5× but only where
truncation bias is not already dominating, and the adaptive bound adds nothing on this data —
ApproxBounds picks the same bound. So the defensible single claim is: **replace `C_u` random
truncation with an ℓ1 clip to a per-PU norm bound, and keep `C_u` only for the τ votes.**

## Debiasing the clip loss

The noisy histogram of per-PU norms already paid for by `ε_b` also estimates the **total**
mass `M`. Since the released group sums `s_g` are DP outputs, `est_L = max(0, M_est − Σ s_g)`
is the clipped mass, and redistributing it as `s_g + share_g · est_L` is **pure
post-processing** — no extra ε.

Two things make it work, and one unmakes it.

**Bin width is free and decisive.** The histogram's L1 sensitivity is 2 regardless of how
many bins it has, so refining from base 2 (11 bins) to base `2^(1/8)` (75 bins) costs
nothing and cuts the geometric-midpoint bias from +2.17% to +0.023%. Coarse debiasing is
*worse than not debiasing* (2.22% vs 0.50%); fine debiasing gives 0.275%.

**It moves the optimal bound down 2 rungs** (2^21 → 2^19), which is the point — correcting
the bias lets you clip harder and pay less noise. Verified independently: 0.5002% → 0.2753%,
**1.82×**.

**But the gain is mostly an artefact of ε_b = 0.5.** The fine histogram needs budget, and
without debiasing that budget is better spent on the values:

| ε_b | TPC-H no-debias | TPC-H debias | SO no-debias | SO debias |
|---|---|---|---|---|
| 0.50 | 0.5048% | **0.2667%** (1.89×) | 10.32% | **5.67%** (1.82×) |
| 0.25 | 0.3390% | **0.2157%** (1.57×) | 8.87% | 9.07% (0.98×) |
| 0.10 | 0.2799% | 0.2719% (1.03×) | 7.40% | 15.11% (0.49×) |
| 0.05 | 0.2673% | 0.3946% (0.68×) | 7.34% | 25.81% (0.28×) |
| 0.02 | **0.2570%** | 0.7096% (0.36×) | **7.10%** | 56.67% (0.13×) |

Jointly optimising the split on both sides: **0.2157% vs 0.2570% (1.19×) on TPC-H and 5.67%
vs 7.10% (1.25×) on StackOverflow.** Real, but ~1.2×, not 1.8×.

**Methodological rule, now established twice** (here and for τ-reuse): *a DP mechanism
comparison at a fixed budget split is not evidence.* Both gains looked like ~1.8× at a fixed
split and shrank to ~1.2× or to nothing once the split was optimised for both arms.

## Both mechanisms fully tuned: 2.8×–4.8× (superseded — see HONEST HEADLINE below)

> These numbers are measured on **coarse groupings where τ never binds**, and they use a
> budget split that returns an empty answer on finer groupings. See *The tuned budget split
> silently destroys the key set* and *HONEST HEADLINE* below for the corrected comparison.

Tuning `C_u` **and** the budget split for *both* sides, scored against the uncapped truth:

| dataset | Google DP best | ours best | gap |
|---|---|---|---|
| TPC-H price/month | 1.33% (C_u = 19) | **0.28%** (C_u = 5) | **4.8×** |
| StackOverflow count/month | 16.29% (C_u = 5) | **5.76%** (C_u = 2) | **2.8×** |
| ClickBench count/date | 1.33% (C_u = 2) | **0.48%** (C_u = 5) | **2.8×** |

**This supersedes every earlier gain figure in this document.** The 12×–880× numbers were
measured against a Google DP pinned at a fixed `C_u` and a fixed 1/3 budget split; both are
free parameters a real deployment would tune. The defensible claim is a consistent **2.8×–4.8×**
across three very different data shapes.

## Where the 4.6× on TPC-H comes from

`C_u` is a free parameter Google would tune, and so is its budget split. Doing both, on
TPC-H sf1 (max `k_u` = 28):

| C_u | split | released | error |
|---|---|---|---|
| 5 | 1/3, 1/3, 1/3 | 100.0% | 49.42% |
| 19 | 1/3, 1/3, 1/3 | 98.8% | 3.57% |
| 14 | 0.05 / 0.05 / 0.90 | 98.8% | 3.64% |
| **19** | **0.05 / 0.05 / 0.90** | **98.8%** | **1.34%** |
| 28 | 0.05 / 0.05 / 0.90 | 97.5% | 1.97% |

**Google DP's best is 1.34% against our 0.29% — a gap of 4.6×**, not the 12–880× obtained
against a fixed-`C_u`, fixed-split Google. Its optimum sits at `C_u = 19`, trading truncation
bias against the `C_u·U` noise.

**The remaining gap is one number, and it is arithmetic.** With the budgets matched, the gap
must equal the sensitivity ratio:

| quantity | value |
|---|---|
| Google's sensitivity, `C_u·U` (C_u=19, U=524,288) | 9,961,472 |
| our sensitivity, `B` | 2,097,152 |
| ratio | **4.75×** |
| measured gap | **4.6×** |

Google bounds **cells** and multiplies by `C_u`; we bound the **norm** directly. `C_u·U` is
the norm of a hypothetical PU with `C_u` cells all at the cell bound — but the true maximum
per-PU norm is **2,403,585**, so `C_u·U` overstates the sensitivity by 4.1×. That slack *is*
the contribution. (Our B = 2,097,152 sits just below the true max, so we clip slightly.)

**This is the fourth gain in this document to shrink under a tuned comparison** (after
τ-reuse, debiasing, and the `C_u` scoring fix). The pattern is consistent enough to state as
a rule: *report the baseline's best configuration, not its default.*

## Two clean negatives

**Free post-processing buys nothing.** Non-negativity projection, James–Stein shrinkage and
SURE-tuned smoothing, applied to the released group vector at the tuned operating point:
**1.001×**. JS shrinks by `c = 1 − (m−3)σ²/‖x−μ‖²`, and the *between-group* spread far
exceeds the noise, so `c ≈ 1`. Non-negativity never binds because no group is near zero. It
pays only when noise approaches the between-group spread — i.e. when the mechanism is
already badly mis-tuned (measured 0.85× narrow / 0.25× wide on a `C_u`-truncating baseline at
`C_u` = 19, which is exactly such a case).

**Gaussian/zCDP loses.** Under the ℓ1 clip `Δ₂ = Δ₁ = B` exactly — attained when a PU puts
all its mass in one group — so ℓ1 clipping gives *no* L2 advantage in the worst case; the
`B/√k` intuition is the typical case, not the sensitivity. Enforcing an explicit L2 clip
makes `Δ₂ = B₂`, but at ε_agg = 0.5, δ = 1e-6 Gaussian needs `Δ₂ ≤ Δ₁/4.23` (zCDP) or
`Δ₁/3.21` (analytic) merely to break even. With each mechanism's bound tuned separately:

| month, 80 groups | error |
|---|---|
| **ℓ1 clip + Laplace** | **0.408%** |
| L2 clip + Gaussian, analytic (Balle–Wang) | 0.383% (0.94×) |
| L2 clip + Gaussian, zCDP | 0.494% (1.21× worse) |
| ℓ1 clip + Gaussian | 1.504% (3.69× worse) |

Only the analytic-Gaussian calibration edges ahead, by 6% (and 10% at 400 groups) — not
worth requiring δ for. The switching rule is `c · k_eff > 14`, and measured `k_eff` saturates
at 6–7 on this data, so it takes 3+ aggregates to flip.

## PRIVACY BUG in the ℓ1 clip as originally written — fixed, and the fix is free

The clip was specified as `n_u = Σ_g min(t(u,g), B)`, then `released = min(t,B) · min(1, B/n_u)`.
**That is a signed sum, not a norm, and it does not bound sensitivity.** Two failure modes:

| construction | `n_u` | released ‖·‖₁ vs B |
|---|---|---|
| `k` cells alternating `+B, −B` — they cancel | 0 → scale = 1, PU released **unclipped** | `k`× (400× at k=400) |
| 2 cells `+B, −B−e` | `−e` → scale = `B/n_u` < 0, diverges | 10¹⁵× at e=1e−12, **unbounded** |

Mode 1 makes sensitivity grow linearly in the number of groups a PU touches — reintroducing
exactly the `C_u` dependence the mechanism claims to remove.

**It fires on an ordinary query.** `SUM(price shipped − price returned)` — net revenue after
returns — by (customer, month) on sf10, 30.4M cells, 24.7% negative, at `B = 2²¹`:

| | as written | with fix |
|---|---|---|
| PUs with `n_u < 0` | 13,266 | — |
| PUs exceeding the bound | **393,053 (39.3%)** | **0** |
| worst ‖released‖₁ / B | **2.6×** | 1.000000000× |

A 2.6× sensitivity violation means the release is 2.6ε-DP, not ε-DP. A second signed measure
(all-negative cells) keeps ‖·‖₁ = B but **flips the released sign** — every group reported
with the wrong sign, a silent correctness failure.

**Fix:** `n_u := Σ_g |clip(t, −B, B)|`. On non-negative data this is *bit-identical*
(`min(t,B) ≡ |clip(t,−B,B)|` for `t ≥ 0`), so **every utility number in this document stands
unchanged**. The ℓ1 clip is not in `src/` yet, so this never shipped. Every measure tested
here was non-negative, which is why 20 commits of experiments never surfaced it.

## The tuned budget split silently destroys the key set

The headline split (`ε_b`=.05, `ε_η`=.10, `ε_v`=.85) was tuned on one 80-group query where τ
never binds. Starving `ε_η` from 1/3 to 0.10 raises τ by 3.33× at every `C_u` — at `C_u`=19,
τ goes **979 → 3,491**. Groups with 979 < PUs < 3,491 are released by Google's split and
killed by mine. On ordinary sf10 queries:

| query | groups | median PUs/group | released (ε_η=1/3) | released (ε_η=0.10) |
|---|---|---|---|---|
| month\|nation, acctbal≥8000 | 2,095 | 2,779 | 2,025 | **20** (−99%) |
| day\|region, acctbal≥8000 | 12,630 | 875 | 1,117 | **0** |
| day\|nation | 63,150 | 963 | 24,929 | **0** (empty result) |

`day|nation` returns *literally nothing*. Any comparison must either match `ε_η` or tune it
per query for both sides — and a mechanism that returns an empty answer must be scored as
100% error, not excluded from the average.

## 4.65× on a τ-binding query (superseded — the baseline was still not fully tuned)

> Superseded by *THE GAP IS 1.36×* below. The Google arm here is missing three legal
> improvements (top-`C_v` selection, rescale-with-re-clip, decoupled `C_e`/`C_v`) and its vote
> histogram is untruncated. Kept for the record because the `C_u`-decoupling reasoning below is
> what the audit then falsified.

Scoring rule that charges for suppression: relative ℓ1 over the **true** key set, a suppressed
group counted as released 0. Both sides tuned over `C_u ∈ {1,2,5,10,19,30,50,72}` × 8 budget
splits (64 configs each). `SUM(price)` by month|nation, acctbal≥8000, sf10 — 2,095 groups,
181,532 PUs, 5.5M cells, max `k_u` = 72:

| | error | best config | released |
|---|---|---|---|
| Google DP | **19.34%** | `C_u`=30, (0.002, 0.300, 0.698) | 1,969/2,095 |
| ours (ℓ1 clip) | **4.16%** | `C_u`=**1**, same split | 1,980/2,095 |
| | **4.65×** | | |

**The mechanism reduces to one structural fact: our value noise does not depend on `C_u` at
all.** Google must *buy* `C_u` to limit truncation bias — at `C_u`=19 it still loses 40%, and
its optimum `C_u`=30 costs `C_u·U` = 15,728,640 in noise, of which 15.56pp of the 19.34% is
irreducible truncation + cell-clip **bias**. We set `C_u` = 1 (cheapest possible τ) because
`C_u` enters only the vote histogram, and pay `B` = 4,194,304 — a **3.75× sensitivity ratio**
that lands as a 4.65× error ratio.

So the gain *grows* with grouping fineness: ~2.4× on a coarse 80-group query (where each PU
touches few groups and `C_u` truncation is nearly free for Google), 4.65× here at max `k_u`=72.

**Correction to method:** an earlier version of this run computed our vote counts from the
*untruncated* per-group PU counts while scaling noise by `C_u` — not a valid sensitivity. With
votes properly truncated to `C_u` groups per PU, ours goes 3.08% → 4.16% and the gap 13.0× →
4.65×. The vote histogram must be truncated even though the values are not.

## δ IS UNDER-ACCOUNTED — and worst exactly where we wanted to operate

The ε accounting survives audit. Write `D' = D ⊎ {u}`; the release is adaptive sequential
composition of ApproxBounds (`ε_b`), the vote histogram (`ε_η`), and the values (`ε_v`):

```
P[M(D) = (b, ĉ, ŷ)] = P[M_b(D)=b] · P[M_η(D)=ĉ] · P[M_v(D,b)=ŷ]
```

The `M_v` factor is evaluated at the **same released `b`** on both sides, which settles the
subtle point: `B`'s data-dependence is paid entirely by `ε_b` and never reappears as an extra
sensitivity term. Publishing values only for `g ∈ S` is post-processing and costs nothing.
Verified numerically — with `B` held fixed, max ‖v(D) − v(D\u)‖₁ = **4,194,304.000 = B exactly**,
attained by 7,434 PUs, never exceeded, independent of `k_u`.

**The δ is wrong.** `ĉ` and `ŷ` are indexed by `G(D) ⊊ G(D')`, and a PU creates a group by
having a **value** there, not a vote. So `|G_new| = k_u`, but Wilson's τ inverts a union bound
over only the `C_u` groups where the PU votes:

```
δ_actual = 1 − (1−p₁)^{C_u} (1−p₀)^{k_u−C_u}      shortfall ≈ k_u / C_u
```

| C_u | actual δ / charged δ |
|---|---|
| **1** | **53.6×** (1.61e−5 vs 3.0e−7) |
| 5 | 13.5× |
| 30 | 2.3× |

**The utility win — pick `C_u`=1 — is exactly what maximises the uncharged δ.** Fix: gate
released groups on having at least one truncated vote (`released &= votes ≥ 1`), so `|G_new| ≤ C_u`
with count exactly 1 and τ's union bound is right. Measured: charged δ ratio → 1.000×, and on
real data the gate never fires (19 of 2,095 groups have zero truncated votes at `C_u`=1, and they
clear τ=48.8 with probability 2.2e−7). **The fix is free.**

## THE GAP IS 1.36×, NOT 4.65× — the structural claim is dead

`taubinding_headtohead.py` also left *Google's* vote histogram untruncated while truncating its
values — invalid sensitivity, and it flattered Google. Fixing that costs Google 20.0% → 21.2%.
But three further improvements are legal and were missing, and together they dwarf it:

| Google configuration | error |
|---|---|
| baseline with DP-valid truncated votes | 20.94% |
| \+ **top-`C_v` selection** (keep each PU's largest cells, not a random `C_v`) | 14.18% |
| \+ **rescale to the PU's true total, then re-clip to `U`** | 6.06% |
| \+ **decouple `C_e` (votes) from `C_v` (values)** | **5.51%** |
| ours (ℓ1 clip), fully retuned | **4.04%** |
| | **1.36×** |

**Rescale-with-re-clip is the big one (2.34×) and it is unambiguously legal.** Google pays
`C_v·U` noise whether or not a PU's contribution actually reaches it. Multiplying the kept cells
by `total_u/kept_u` and re-clipping each to `U` leaves ≤ `C_v` cells each ≤ `U`, so `Δ₁ = C_v·U`
is untouched — measured max ‖v_u‖₁/(`C_v·U`) = **1.000000**, i.e. it exactly fills the budget
Google was already buying. (Rescaling *without* the re-clip is illegal — measured violation up
to 13.7× — and buys only 0.36pp anyway.)

**Decoupling `C_e` from `C_v` kills the structural story.** The vote histogram and the value
histogram are two releases with two sensitivities; nothing forces one parameter. Google sets
`C_e`=1 for the cheapest τ — exactly what we do — and `C_v`=10 independently. So *"our value
noise is `C_u`-free, so we can afford `C_u`=1 while Google cannot"* is **false**: Google gets the
same cheap τ. What remains is only the sensitivity ratio, `C_v·U` = 5,242,880 vs `B` = 4,194,304
= **1.25×**, landing as a measured 1.36× once clip bias (2.51% vs 0.75%) is added.

**The scoring rule is not the source of the gap** — retuning each arm separately under nine
different metrics gives 1.23×–1.45×, and no metric makes Google win:

| metric | Google | ours | gap |
|---|---|---|---|
| relative ℓ1 over the true key set (used here) | 5.48% | 4.05% | 1.35× |
| relative ℓ1 over the intersection of released sets | 4.63% | 3.43% | 1.35× |
| each judged on its own released set | 4.64% | 3.45% | 1.34× |
| RMSE / mean(truth) | 8.17% | 6.34% | 1.29× |
| median per-group relative error | 3.71% | 2.56% | 1.45× |

**Gaussian is now asymmetric and favours Google.** For us `Δ₂ = Δ₁ = B`, so Gaussian loses
(13.27% vs 4.06%; an explicit L2 clip gives 4.13%, still no gain). For Google at large `C_v`,
`√C_v·U` genuinely beats `C_v·U`: 14.18% → 11.62%.

**This is the sixth gain in this document to shrink under a properly tuned baseline.** The rule
stands and should be the methodological headline of the writeup: *a DP mechanism comparison is
evidence only if the baseline was tuned as hard as the proposal.*

## The gain does NOT grow with grouping fineness — sweep against the fixed baseline

Reproduced the 1.34× independently, at the same configuration the steelman found
(`C_e`=1, `C_v`=10), then swept grouping granularity. TPC-H sf10, `acctbal ≥ 8000`, every arm
tuned over `C_e` × `C_v` × 7 splits (`attacks/fineness_sweep.py`):

| grouping | groups | max `k_u` | median PUs/group | Google | +top | +rescale | ours | gap |
|---|---|---|---|---|---|---|---|---|
| year | 7 | 7 | 163,565 | 0.02% | 0.02% | 0.02% | 0.01% | 1.81× |
| month | 84 | 72 | 69,976 | 0.76% | 0.73% | 0.42% | 0.17% | **2.40×** |
| month\|priority | 420 | 104 | 17,100 | 3.45% | 2.13% | 1.18% | 0.91% | 1.30× |
| month\|nation | 2,095 | 72 | 2,779 | 15.95% | 12.76% | 5.62% | 4.20% | 1.34× |
| day | 2,526 | 176 | 4,386 | 21.12% | 21.08% | 6.77% | 6.08% | 1.11× |
| week\|nation | 9,050 | 130 | 1,038 | 99.97% | 99.93% | 99.90% | 99.92% | — |
| day\|region | 12,630 | 176 | 875 | 100.0% | 99.99% | 99.99% | 100.0% | — |

Two conclusions, both against the earlier draft:

**The fineness hypothesis is dead.** Against the *unfixed* Google the gap did grow with fineness
(5.27× at month, 5.21× at day). Against the fixed one it *shrinks* — 2.40× → 1.11×. The rescale
trick repairs truncation bias precisely where truncation was worst, which is exactly the fine
groupings, so it flattens the curve. The earlier reading was an artifact of the missing baseline
improvement.

**Below ~1,000 PUs per group, both mechanisms return nothing at ε=1.** `week|nation` and
`day|region` sit at ~100% error for every arm: τ suppresses essentially every group. The
comparison is only meaningful above that floor, and no clipping geometry rescues it — it is a
partition-selection limit, not a sensitivity limit.

**Honest range: 1.1×–2.4×, median ~1.3×.**

## Four more clean negatives, one of them provable

**Finer bound grids buy nothing (1.009×).** Google's effective bound `C_v·U` lives on a fine
grid (any integer × a power of two) while ours is `B = 2^j`, so we should be paying a rounding
penalty of up to 2×. Measured with log-base 2, √2, 2^(1/4), 2^(1/8) — every grid converges on the
same `B` = 2²², and 2^(1/8) is *worse* (5.29%) because 512 bins raise the ApproxBounds threshold
enough to push `B` up a notch. The threshold itself grows only logarithmically
(24.88/`ε_b` at 64 bins → 26.96 at 512), so fine bins are cheap; they just have nothing to win
here. `attacks/bound_grid.py`.

**The ℓ1 clipping geometry cannot matter — this one is exact, not empirical.** Five geometries at
identical sensitivity `‖v_u‖₁ ≤ B` (proportional scaling, ℓ1-ball projection / soft-threshold,
per-PU water-filling cap, greedy keep-top, flat `B/k_u` cap):

| geometry | error | clip bias |
|---|---|---|
| proportional | 4.223% | 0.753% |
| softthresh | 4.295% | 0.753% |
| waterfill | 4.222% | 0.753% |
| keeptop | 4.230% | 0.753% |
| uniformcap | 5.124% | 2.688% |

The first four have *bit-identical* bias, and necessarily so: on non-negative data every
budget-saturating geometry removes exactly `max(0, ‖t_u‖₁ − B)` from each PU, and because all
per-group deficits share a sign,
`Σ_g |tot_g − truth_g| = Σ_u max(0, ‖t_u‖₁ − B)` — independent of *which* cells the mass came
from. Geometry is invisible to any total-absolute-error metric; only budget **saturation**
matters, which is the whole reason `uniformcap` (`Σ min(|t|, B/k_u)` ≪ `B` for skewed PUs) loses.
It would matter for per-group *relative* metrics. `attacks/clip_geometry.py`.

**Fractional votes: 1.018×.** Replacing random truncation in the vote histogram with a fractional
`C_u/k_u` vote in every group a PU touches keeps ℓ1 vote sensitivity at `C_u` (so τ is unchanged)
while removing the binomial sampling variance — a strict improvement in principle, worth nothing
in practice: at 2,779 PUs/group the mean count is 77 against τ = 48.8, so groups clear τ either
way. `attacks/vote_geometry.py`. Top-`C_u` votes are actively *worse* (5.01%) — value-ranked
votes concentrate the key set on groups that were already safe.

## The adaptive bound rule is not needed — use ApproxBounds on the norm histogram

The winning configuration above does **not** use the adaptive objective rule from the section
above. It runs plain **ApproxBounds over the per-PU *norm* histogram** — same Google
primitive, different input. This is strictly better as a proposal:

- The adaptive rule **as written in this document is numerically broken**: the objective sums
  over all 64 log2 bins, and empty high bins carry `Laplace(2/ε_b)` noise times
  `mid₆₃ = 2^63.5 ≈ 1.3e19`, swamping both numerator and total mass. Argmin then always picks
  `B = 2`. A verbatim implementation scores **50.3%**, not 0.30%. Reproducing 0.30% needs
  clamping noisy counts at 0 *and* a public bin cap ≤ ~2²² — a load-bearing, undocumented
  hyperparameter. (This code happened to use `np.unique` over occupied bins only, which is why
  it never bit here.)
- The rule also consumes `median_group_total`, a **non-private** data-dependent quantity never
  charged to the ε budget.
- Google's own ApproxBounds, given `ε_b` = 0.002, lands within 3% of the fine-grid oracle
  bound. Starving `ε_b` is the whole trick, and it is free.

The mechanism contribution is therefore *only* the change of clipping geometry — bound the
per-PU norm, keep `C_u` for the votes — with no new bound-selection machinery.

## WHERE THIS LANDED — read this section first

After correcting the baseline six times and finding two privacy bugs in our own mechanism, the
defensible claim is:

**Bound the per-PU ℓ1 norm and clip to it, instead of bounding cells and multiplying by `C_u`.
Against a fully tuned Google DP this is worth 1.1×–2.4× (median ~1.3×) on grouped SUM.**

Two required fixes, both free:
1. `n_u := Σ_g |clip(t, −B, B)|`, never `Σ_g min(t, B)` — the latter is not a norm and loses ε-DP
   entirely on signed measures (39.3% of PUs over bound on an ordinary net-revenue query).
2. Gate released groups on having ≥1 truncated vote, or δ is understated by ~`k_u/C_u` (53.6× at
   the `C_u`=1 we want).

What the gain actually is: after Google gets rescale-with-re-clip, both mechanisms reduce to
*clip each PU's total to a bound, add Laplace(bound/ε)*. The residual is (a) a slightly tighter
bound, `C_v·U`/`B` = 1.25×, and (b) that top-`C_v` truncation **misallocates** a PU's mass onto
its largest groups, which ℓ1 clipping never does — 2.51% vs 0.75% bias. That is the contribution.
It is real, small, and does not grow with grouping fineness.

What measured as nothing, after tuning both sides: adaptive bound selection (ties ApproxBounds,
and is numerically broken as documented), finer bound grids (1.009×), ℓ1 clipping geometry
(provably 1.000× for total-error metrics), fractional votes (1.018×), free post-processing
(1.001×), Gaussian/zCDP (loses for us; *helps* Google), τ-reuse (wash), half-dataset splitting
(exact cancellation), per-group clip estimation (5.8–18.4× worse). The only other real gain is
debiasing the clip loss, 1.19–1.25×.

**Methodological finding, and arguably the most transferable result here: six separate gains in
this document evaporated once the baseline was tuned as hard as the proposal.** 880× → 12× →
4.8× → 4.65× → 1.36×. A DP mechanism comparison at a fixed budget split, a fixed `C_u`, or
against a library's default configuration is not evidence.

## THE 1.3× IS MEASURED AT TPC-H'S BEST CASE FOR GOOGLE

Two adversarial re-derivations of the 1.36× came back. Both reproduced it; both found the
*framing* wrong, in opposite directions. Net: the number is right and the conclusion drawn from
it was not.

**First, two corrections to our own harness.**

1. **The split grid capped `ε_η` at 1/3, which structurally under-tunes Google.** Google's τ
   scales with `C_u`, so it needs a large `C_u` (to kill truncation bias) *and* a large `ε_η` (to
   afford the τ that `C_u` buys) — a combination the grid could not express. This is this
   document's own rule failing on this document's own harness, for the seventh time. Re-running
   with `ε_η` up to 0.6:

   | grouping | old grid | widened grid |
   |---|---|---|
   | month | 2.40× | **2.13×** |
   | month\|priority | 1.30× | 1.30× |
   | month\|nation | 1.34× | 1.35× |
   | day | 1.11× | **1.17×** |

   Honest range **1.17×–2.13×**, replacing 1.1×–2.4×. Google gains most on `day` (21.12% →
   14.60%), where it wants `C_v`=30.

2. **`taubinding_headtohead.py` truncates votes incorrectly** — `keep` is built from indices into
   the `pi`-sorted arrays and then applied to the *unsorted* `gi`/`val`, so at `C_u`=1 a PU can
   get up to 9 votes and 26.6% of PUs exceed the cap. That is a 9× ε violation in the vote
   channel of the harness that produced the 4.65×. **`fineness_sweep.py` is clean** — verified
   directly, max votes per PU equals `C_u` exactly and 0 PUs over cap at `C_u` ∈ {1,2,5,30} for
   both random and top ranking — so every number in the sections above stands.

**Then the finding that matters.** Google's rescale moves most of every PU's mass onto that PU's
top-`C_v` cells. The group-level bias of doing so is small *only if PUs disagree about which of
their own groups is biggest*, so the misallocation cancels across many PUs. TPC-H ship dates are
uniform, so which of a customer's 30 months is largest is essentially noise — the best possible
case for the trick. Tilting each PU's allocation toward a shared direction while holding fixed
**every quantity either mechanism's accounting reads** (per-PU totals, per-PU norms and therefore
`B`, the (PU,group) incidence and therefore `k_u`, both vote histograms and τ, and Σtruth):

| alignment | Google | +top | +rescale | ours | gap |
|---|---|---|---|---|---|
| **1.07 (real TPC-H)** | 12.31% | 9.61% | 5.59% | 4.13% | **1.35×** |
| 3.81 | 14.85% | 11.00% | 9.41% | 4.06% | **2.32×** |
| 7.58 | 23.58% | 17.70% | 17.81% | 4.07% | **4.35×** |
| 12.04 | 46.23% | 19.27% | 22.00% | 4.23% | **4.56×** |

*(alignment = HHI of each PU's argmax group × K; 1.0 means PUs disagree entirely.)*

**Ours is flat at 4.06–4.23% across the entire range.** That is the real structural statement,
and it is sharper than the `C_u`-free claim it replaces: **our error depends only on the per-PU
norm distribution, which the perturbation preserves; Google's additionally depends on how each PU
spreads its mass across groups.** The ℓ1 clip never redistributes, so it cannot be wrong about
where the mass went.

**How aligned is real data?** Measured on every dataset available, restricted to PUs with
`k_u ≥ 5` — the ones truncation actually moves:

| dataset | alignment | PUs with `k_u ≥ 5` |
|---|---|---|
| TPC-H price by month\|nation | **1.07** | 99.9% |
| StackOverflow posts by month | 1.41 | 5.3% |
| StackOverflow score by month | 1.49 | 5.3% |
| ClickBench hits by date | 2.07 | 0.0% (31 users) |
| ClickBench hits by region | 13.02 | 0.0% (40 users) |

This does **not** establish that real workloads are aligned where it matters. TPC-H is the only
dataset here in which PUs genuinely spread across many groups, and it sits at 1.07 — essentially
the theoretical floor. StackOverflow and ClickBench show more alignment (1.4–13) but almost no
wide-spread PUs, so truncation barely engages there at all.

**The defensible statement is therefore narrower than either previous headline:** on the only
available dataset with wide per-user spread, the gap is 1.17×–2.13×, and that dataset has the
most Google-favourable allocation structure achievable. A workload with both wide spread and
genuine alignment — seasonal retail, event data around releases — would show 2×–4.5×, but no such
dataset was tested. **Getting one is the single highest-value next experiment.**

**Three further corrections from the refutations**, all against claims recorded above:

- **"The gap equals the sensitivity ratio" is wrong.** Decomposed at both optima: noise-term ratio
  1.086×, bias-term ratio 2.176×. The 1.25× sensitivity ratio explains almost none of it — the gap
  is essentially *all* redistribution bias, which is exactly what the alignment probe then
  confirmed by moving the bias alone.
- **Decoupling `C_e` from `C_v` is not load-bearing** — worth 0.10pp at the rescale rung
  (5.552% → 5.451%), not the 0.55pp claimed. It matters only at the pre-rescale rung.
- **The steelman's rescale re-introduced the signed-value bug** this document already found and
  fixed in the ℓ1 clip: `min(v·f, U)` caps only from above, and on signed data violates `C_v·U` by
  3.14×. Fixed here to two-sided `clip(·, −U, U)`; identical on non-negative data.

## Dandan's follow-ups, 12 Aug — all four are the same cancellation

`attacks/dandan_followups.py`. Every one of these splits a budget across `k` sub-mechanisms and
recombines. The recombination never repays the split, because Laplace/exponential error scales as
`1/ε` while any accompanying reduction in data re-inflates the relative error by exactly the same
factor.

**(1) "AVG needs no rescaling, so it escapes the cancellation."** It doesn't — and the reason is
worth stating precisely, because the intuition is a good one. Halving the PUs and doubling the
per-attribute budget halves the noise scale on the SUM (16,000,000 → 8,000,000), but it also
halves the true SUM (416.7e9 → 208.3e9). Relative error is unchanged: 0.0446% vs 0.0441%
= **0.988×**. No rescale is written down, but the **halved COUNT in the denominator is the
rescale**. And the half then carries 0.254% sampling error — 5.8× the noise it was trying to
save.

**(2) Restricting ApproxBounds to `c₂−c₁+2` candidate bins.** The threshold is
`−ln(2(1 − P^(1/2n)))/ε_b`, which is **logarithmic in the bin count**:

| bins | 2 | 4 | 16 | 64 | 256 | 512 |
|---|---|---|---|---|---|---|
| threshold × `ε_b` | 21.42 | 22.11 | 23.50 | 24.88 | 26.27 | 26.96 |

Collapsing 64 candidate bins to 4 lowers it by **11%**. And the budget it would save is not
there to save: **the tuned optimum is `ε_b` = 0.002 in every sweep in this document, 0.2% of ε.**
Making automatic bounding entirely free is worth 0.2%. The frozen-correspondence machinery — a
parser that picks a bounding strategy per query and per attribute, plus stored exception sets —
is a large amount of work for at most that.

**(3) Combining `t` per-group histograms to lower τ.** τ genuinely does drop, which is the
appealing part: **48.8 → 46.4 → 41.8 → 32.6** for `t` = 1, 2, 4, 8 (each test must fire with
probability `δ_η^(1/t)`, so each log-term is divided by `t`). But τ is not the decision-relevant
number. The smallest group actually *released* — where all `t` tests pass with probability ½ —
goes the other way: **48.8 → 50.0 → 57.1 → 80.5**. Splitting `ε_η` across `t` tests multiplies
each test's noise by `t`, and requiring all `t` to pass costs more than the lower threshold buys.
The root cause: all `t` histograms estimate the *same* quantity — how many PUs are in the group —
so measuring it `t` times at `ε_η/t` is strictly worse than measuring it once at `ε_η`. An OR
instead of an AND loses too (false positives then need `δ/t`, raising τ). Reusing the
bound-selection histograms for free doesn't rescue it either: at `ε_b` = 0.002 their noise scale
is `C_u`/0.002 = 500 against a threshold near 48.

**(4) Half-dataset splitting with a global-sensitivity median.** Worth testing, because by the
rule *"splitting helps exactly when the sensitivity grows slower than the budget doubles"* a
global-sensitivity median should be the ideal case — its sensitivity is 1 rank regardless of `n`,
so it doesn't grow at all. Implemented the exponential mechanism on rank utility over a public
range (pure ε-DP, no δ) and swept ε and `n`:

| n | ε | full | half (2ε) | ratio | subsampling alone | dominated by |
|---|---|---|---|---|---|---|
| 1,971 | 0.002 | 95.35% | 92.92% | 0.97× | 1.43% | DP noise |
| 1,971 | 0.02 | 8.86% | 7.06% | **0.80×** | 1.43% | DP noise |
| 1,971 | 0.2 | 1.41% | 1.79% | 1.27× | 1.43% | subsampling |
| 181,532 | 0.002 | 0.77% | 0.84% | 1.09× | 0.14% | DP noise |
| 181,532 | 0.02 | 0.08% | 0.17% | 1.97× | 0.14% | subsampling |
| 181,532 | 2.0 | 0.0005% | 0.148% | **290×** | 0.14% | subsampling |

Same shape as the smooth-sensitivity result: a ~1.25× win in a narrow window where DP noise
dominates, and catastrophic loss (1.3×, 20×, 290×) once the mechanism is accurate. **The window
sits exactly where the median is too noisy to be worth releasing.** So the global-sensitivity
median does not rescue splitting — and it shows the earlier ~0.85× smooth-sensitivity win was not
splitting working, but smooth sensitivity growing sub-linearly over a narrow range of `m`.

**The one direction from this batch that is not dead:** none of these attack the real bottleneck.
`ε_b` is 0.2% of the budget and τ's floor is set by a single count measurement that cannot be
beaten by subdividing it. If the ~1,000-PUs-per-group floor is to move, it has to come from
somewhere other than budget re-allocation.

## THE τ FLOOR IS AN ℓ1 ARTIFACT — Gaussian votes break it (up to 4.5×)

The `week|nation` / `day|region` rows where *every* arm returned ~100% error are not a sensitivity
limit and not a budget-allocation problem. They are the ℓ1 geometry of the **vote** channel, and
the algebra says so exactly. With votes truncated to `C_e`, a PU spread over `k_u` groups puts
only `C_e/k_u` of a vote in each, so a group's expected vote count is `n_g·C_e/k_u` against a
threshold `τ ≈ C_e·ln(1/2δ_η)/ε_η`. **`C_e` cancels**, leaving

```
a group is releasable  iff  n_g / k_u  ≳  ln(1/2δ_η) / ε_η   ≈  30–45
```

That is why the tuner always lands on `C_e`=1 and why nothing helps: `week|nation` has
`n_g`/`k_u` = 8.0 and `day|region` 5.0. No re-allocation of ε moves a ratio that ε barely enters.

**The fix is to change the noise geometry, not the budget.** Gaussian lost badly on the *value*
channel because the ℓ1 clip makes `Δ₂ = Δ₁ = B` exactly — a PU may put all its mass in one group.
The vote channel is the opposite: a PU voting 1 in each of its `k_u` groups has

```
ℓ1 sensitivity = k_u        but        ℓ2 sensitivity = √k_u
```

so Laplace *must* truncate while Gaussian can let every PU vote everywhere and pay only `√k_u`.
Both arms given the same total δ for partition selection (Laplace spends it all on the threshold;
Gaussian must split it between mechanism and threshold, a real cost to Gaussian):

| grouping | `n_g/k_u` | Laplace | Gaussian | gain | groups released |
|---|---|---|---|---|---|
| month\|priority | 164 | 0.92% | 0.87% | 1.06× | 415 → 415 of 420 |
| month\|nation | 39 | 3.93% | 3.04% | 1.29× | 1,987 → 2,021 of 2,095 |
| day | 25 | 5.22% | 3.78% | 1.38× | 2,415 → 2,444 of 2,526 |
| **week\|nation** | 8.0 | 73.22% | **16.42%** | **4.46×** | 2,943 → **8,493** of 9,050 |
| **day\|region** | 5.0 | 96.77% | **26.73%** | **3.62×** | 545 → **11,735** of 12,630 |

The gain is monotone in `k_u/n_g` and vanishes exactly where the theory says the floor stops
binding — a strong internal check. Two queries that previously returned essentially nothing now
return 94% of their key set. `attacks/vote_gaussian.py`.

**The floor is provable, not asymptotic — and no Laplace budget split can clear it.** The worry
about the argument above was that "`C_e` cancels" holds only in the limit. Expanding Wilson's τ for
small `δ_η` gives `τ ≈ 1 + (C_e/ε_η)·[ln(1/2δ_η) + ln C_e]`, so the release condition is

```
n_g / k_u  >  τ(C_e)/C_e  ≈  (1/ε_η)·[ln(1/2δ_η) + ln C_e]
```

`C_e` cancels except for a residual `+ln C_e` that makes larger `C_e` strictly worse. Computing
the **exact** τ confirms `τ/C_e` is monotone increasing in `C_e`, so **`C_e` = 1 is optimal
exactly**, and the floor is `ln(1/2δ_η)/ε_η` — a function of `ε_η` alone:

| `ε_η` | 0.3 | 0.5 | 0.6 | 0.9 | 0.99 |
|---|---|---|---|---|---|
| floor `τ/C_e` at `C_e`=1 | 44.7 | 27.2 | 22.9 | 15.6 | **14.3** |

Even at `ε_η` = 0.99 — leaving essentially nothing for the values — the floor is 14.3, so
`week|nation` (`n_g/k_u` = 8.0) and `day|region` (5.0) **stay fully suppressed under every Laplace
split that exists**. The two queries Gaussian votes rescue are provably unrescuable by budget
re-allocation, which is the strongest form of the answer to Dandan's (3).

**The crossover is `k_u` ≈ 4.3, and it barely moves.** Solving for where the two release
thresholds cross — Laplace releases iff `n_g > k_u·τ(1)`, Gaussian iff
`n_g > 1 + √C_e·√(2ln(1.25/(δ/2)))·Φ⁻¹(1−(δ/2)/C_e)/ε_η`:

| `ε_η` | 0.2 | 0.3 | 0.5 |
|---|---|---|---|
| crossover `k_u`, δ=1e−6 | 4.5 | 4.4 | 4.3 |
| crossover `k_u`, δ=1e−9 | 4.3 | 4.2 | 4.2 |

Both thresholds scale as `1/ε_η`, so the crossover is nearly invariant to the budget *and* to δ.
**Gaussian votes win whenever a privacy unit can touch about five or more groups** — which is
almost every grouped query with a per-user privacy unit. The Laplace `C_e` truncation is the
wrong default, not merely a suboptimal one. (This is far below the 30–60 a first guess suggests:
Laplace's cost is *linear* in `k_u` while Gaussian's is `√k_u`, so the `√(2ln(1.25/δ))` ≈ 5.3
constant is repaid almost immediately.)

**Implementation warning for the port — the sensitivity must not read the data.** The simulation
uses `√(min(C_e, k_max))`, which is safe there only because the `C_e` grid is capped at the true
`k_max`, making `min(C_e, k_max) = C_e` always. In a deployment `C_e` is a public setting
(`dp_max_groups_contributed`) that a user may set *above* the true `k_max`, and then reading
`k_max` from the data understates sensitivity: at public `C_e` = 200 against a true `k_max` = 72,
`√200` = 14.14 but `√72` = 8.49, a **1.67× understatement** — the release would be
`1.67·ε_η`-DP. **`src/` must use `√C_e` from the public setting and never a data-read `k_max`.**

**This is a partition-selection change, so Google can adopt it too** — it is a contribution to the
mechanism, not to the gap. Google DP's library uses Laplace for partition selection (Wilson et
al.), so it is not something it does today, but nothing stops it. Giving Gaussian votes to *both*
arms and retuning everything:

| grouping | gap, Laplace votes | gap, Gaussian votes | Google | ours |
|---|---|---|---|---|
| month\|nation | 1.35× | 1.48× | 5.58% → 4.55% | 4.13% → 3.08% |
| day | 1.17× | 1.24× | 6.32% → 4.66% | 5.41% → 3.77% |
| week\|nation | — (both ~100%) | 1.13× | 99.90% → **18.45%** | 99.92% → **16.39%** |
| day\|region | — (both ~100%) | **0.85×** | 99.99% → **22.59%** | 100% → 26.69% |

**So the honest reading is that Gaussian votes are worth far more than the ℓ1 clip is.** They
convert two queries from *unanswerable* to 94% key-set recovery, and they do it for either
mechanism. The ℓ1-clip gap stays at 1.2×–1.5× and **inverts to 0.85× on the finest grouping**,
where large `C_e` lets Google decouple a small `C_v`=10 for values and its rescale works well —
on TPC-H's minimum-alignment data, which is its best case.

Ranking the session's findings by size, honestly: Gaussian votes (up to 4.5×, both arms) ≫ the
ℓ1 clip (1.2×–2.1×, ours only, and it can invert) > everything else (≤1.02×).

**Caveat to carry:** the δ under-accounting found earlier applies here too and is *worse*, since
a PU votes in up to `C_e`=88 groups while holding values in up to `k_u`=176. The vote-support
gate (`released &= votes ≥ 1`) is still the fix and is still free.

**This is the constructive answer to Dandan's (3).** She asked whether combining evidence across
`t` per-group histograms could lower τ. Splitting the budget `t` ways cannot (above). But
changing the *noise geometry* of the single histogram lowers the effective floor by 4–5× on
exactly the queries that motivated the question.

## GEOMETRY-MATCHED NOISE — the generic statement, and 4.2×–6.0× vs Google DP as published

The findings above stop being a grab-bag once stated as one rule. A grouped DP release is several
independent channels. For each, take **one PU's contribution vector to that channel**. Laplace
pays its ℓ1; Gaussian pays its ℓ2 × `√(2ln(1.25/δ))`. So Gaussian is right for a channel iff

```
‖·‖₁ / ‖·‖₂  >  √(ln(1.25/δ))      ≈ 3.75 at δ = 1e-6
```

| channel | one PU's vector | ℓ1 | ℓ2 | ratio | → |
|---|---|---|---|---|---|
| values, under the ℓ1 norm clip | mass over groups | `B` | `B` | **1.0** | Laplace |
| values, Google (`C_v` cells ≤ `U`) | ≤ `C_v` cells at `U` | `C_v·U` | `√C_v·U` | `√C_v` | Gaussian if `C_v` > 14 |
| votes / partition selection | 1 in each of `k_u` | `k_u` | `√k_u` | `√k_u` | **Gaussian** if `k_u` > 14 |
| bounds histogram over per-PU norms | exactly one bin | 1 | 1 | **1.0** | Laplace |
| bounds histogram over per-cell values | ≤ `C_v` bins | `C_v` | `√C_v` | `√C_v` | Gaussian if `C_v` > 14 |

**The ℓ1 clip is precisely what forces ratio = 1 on the value channel.** It permits a PU to
concentrate all its mass in one group, so the worst case is fully concentrated and there is no ℓ2
advantage to buy. Same distribution, opposite geometries, opposite answers — which is why Gaussian
measured 3.7× *worse* on values and 4.5× *better* on votes. It also retro-explains the earlier
"Gaussian/zCDP loses" result, which was measured on the value channel only and wrongly generalised.

**The vote channel's accounting is verified, not argued.** Neighbour simulation on sf10: measured
max ‖Δ‖₂ equals `√(min(C_e, k_max))` to a ratio of **1.000 at every `C_e`** ∈ {1,5,30,72}; ℓ1/ℓ2 =
8.49 at `C_e`=72, comfortably over the 3.75 rule threshold. The Gaussian τ never released a
singleton in 4M trials against a charged 6.9e−9. The classical σ is valid for ε ≤ 1 and the tuner
picks `ε_η` ∈ [0.2, 0.6]; Balle–Wang would be tighter, so this is the conservative choice.

**Against Google DP as published** — Wilson et al. 2020 and the library: ApproxBounds over
per-cell values, **one** `C_u` randomly truncating values and votes together, Laplace on every
channel. (The rescale-to-true-total trick and the `C_e`/`C_v` decoupling are this project's own
inventions from adversarial review; they belong in a discussion section, not the baseline.)

| grouping | `n_g/k_u` | published | package | gain |
|---|---|---|---|---|
| month | 972 | 0.92% | 0.17% | **5.34×** |
| month\|priority | 164 | 4.35% | 0.86% | **5.04×** |
| month\|nation | 39 | 12.79% | 3.04% | **4.21×** |
| day | 25 | 16.17% | 3.80% | **4.25×** |
| week\|nation | 8.0 | 98.83% | 16.46% | **6.00×** |

**4.2×–6.0×, median 5.0×.** `day|region` did not finish — it OOM-killed the machine;
`geometry_matched.py` now carries a `--max-cells` guard.

**Both numbers must be reported.** Against a Google DP we improve ourselves — rescale plus
decoupled `C_e`/`C_v`, neither published anywhere — the value-channel gap is 1.2×–1.5× and
*inverts* to 0.85× on the finest grouping. The defensible framing is: *the package is 5× better
than deployed Google DP; roughly half of that survives against improvements to Google DP that we
had to invent ourselves, and the vote-channel half survives regardless because Google's Laplace
partition selection cannot be fixed by re-allocating budget.*

## WHEN DOES THIS APPLY? StackOverflow and ClickBench say: only for concentrated `k_u`

`attacks/dataset_profile.py`. The quantity I had been using, `n_g / max k_u`, is a caricature. The
exact one is per group: with votes truncated to `C_e`, a PU spread over `k_u` groups contributes
`min(C_e,k_u)/k_u` of a vote to each group it touches, so

```
E[votes in g] = Σ_{u ∈ g} min(C_e, k_u)/k_u          ("effective vote count")
```

and `g` is releasable iff that clears the arm's threshold. `n_g / eff` is the **harmonic-mean
`k_u` of the group's members** — exactly what Laplace truncation throws away. Both arms share the
vote count and differ only in threshold, so `C_e` must be tuned **separately for each** (pinning
`C_e` = `k_max` is catastrophic for Gaussian on heavy-tailed `k_u`, and my first version of this
profile did exactly that — the untuned-baseline error again, this time against my own result):

| query | groups | `k_u` med/p90/max | `n_g/eff` | Laplace rel. | `C_e` | Gaussian rel. | `C_e` |
|---|---|---|---|---|---|---|---|
| tpch month\|nation | 2,095 | 30 / 45 / 72 | 30.4 | 2,025 | 1 | 2,044 | 34 |
| tpch day | 2,526 | 55 / 93 / 183 | 58.2 | 2,506 | 3 | 2,520 | 55 |
| **tpch week\|nation** | 9,050 | 48 / 77 / 130 | 49.5 | **0** | any | **8,480** | 55 |
| **tpch day\|region** | 12,630 | 55 / 93 / 176 | 58.3 | **0** | any | **11,600** | 55 |
| so posts\|month | 190 | 1 / 3 / 156 | 1.9 | 165 | 2 | 162 | 3 |
| so posts\|day | 5,107 | 1 / 4 / 2,126 | 3.0 | 6 | 1 | 0 | — |
| so comments\|month | 182 | 1 / 3 / 154 | 2.1 | 164 | 1 | 158 | 3 |
| so badges\|month | 165 | 1 / 4 / 154 | 2.0 | 165 | 3 | 165 | 2 |
| cb hits\|region | 3,238 | 1 / 1 / 54 | 1.0 | 665 | 1 | 484 | 1 |
| cb hits\|date\|region | 7,564 | 1 / 1 / 58 | 1.5 | 1,006 | 1 | 695 | 1 |
| cb hits\|url | 2,019,483 | 1 / 6 / 1,067 | 7.0 | 1,626 | 1 | 692 | 1 |

**Three regimes, and the middle one was invisible until now.**

1. **Floor-binding, concentrated `k_u` → Gaussian is the difference between nothing and
   everything.** `week|nation` and `day|region` release **0 groups under Laplace at every `C_e`**
   against 8,480 and 11,600 under Gaussian. The earlier 4.46× understated this: measured as
   released key set it is not a ratio at all.
2. **Not floor-binding → Gaussian buys ~1%.** `month|nation` 2,025 → 2,044, `day` 2,506 → 2,520.
   (The earlier 1.29×–1.38× *error* gains come from which groups get released, not how many.)
3. **Heavy-tailed `k_u` → Gaussian LOSES, by up to 2.4×.** Every StackOverflow and ClickBench
   query. `cb hits|url` is the extreme: 1,626 groups under Laplace vs 692 under Gaussian.

**The predictor is the shape of the `k_u` distribution, not its scale.** TPC-H has
median 30–55 against max 72–183 — every customer spreads widely, so truncation to `C_e`=1 costs
*every* PU a factor of ~30–55 and Gaussian's `√C_e` is cheap by comparison. StackOverflow and
ClickBench have median `k_u` = 1 with max 154–2,126: **most users touch one group and lose nothing
to truncation, while Gaussian must pay `√C_e` sized for the tail.** Laplace truncation is nearly
free on a heavy tail and ruinous on a concentrated distribution.

**Design rule for the port:** compare median `k_u` against `max k_u`. Choose Gaussian votes when
`k_u` is concentrated and above the ≈4.3 crossover; choose Laplace when `k_u` is heavy-tailed
(median ≈ 1). Both statistics are data-dependent, so a deployment must either take them from the
public `C_e` setting or spend budget to estimate them — **an unresolved accounting question, and
the most important open item for the implementation.**

**Scope limit, stated plainly:** the vote-channel result applies to workloads where privacy units
genuinely spread across many groups. Of the three datasets available, only TPC-H is such a
workload. This does not make the result narrow — user-level analytics over time buckets is exactly
this shape — but the claim must be conditioned on `k_u` concentration rather than asserted
generally.

## THE 4.5× IS A RESONANCE, NOT A PLATEAU — the flip band is ~3.5× wide and TPC-H fits inside it

The `k_u` scope limit above is **necessary but not sufficient**, and the shape of the remaining
condition is what makes the result narrow. Both release rules are thresholds on the *same*
effective vote count `eff(C) = Σ_u min(C,k_u)/k_u`, so writing `k_h = n_g/eff(1)` for the
**harmonic-mean `k_u`** of a group's members, a group is released iff

```
Laplace :  n_g  >  k_h · τ_L(1)          Gaussian:  n_g  >  thr_G(C*)
```

Groups that **flip** — the entire source of the gain — are those with `thr_G(C*) < n_g < k_h·τ_L(1)`,
a window of multiplicative width

```
band  =  k_h·τ_L(1) / thr_G(C*)  =  √k_h / ρ ,      ρ = (thr_G(1)−1)/(τ_L(1)−1) ≈ 2.02
```

**≈ 3.5× at `k_h` = 49.** Everything above the band both arms release; below it, neither. Both
thresholds carry the same `1/ε_η`, so changing ε **slides** the band along `n_g` without widening
it. Three consequences, all measured (`attacks/band_map.py`, `band_synth.py`, `metric_audit.py`;
the flagship is reproduced on a 5-nation replica of `week|nation`, structurally identical —
`n_g/eff` 49.3 vs 49.5, 4.47× vs the published 4.46× — at 1.8M cells instead of 9.0M):

**1. The gain is the share of the key set inside one band, so it is maximal iff every group is the
same size.** TPC-H is exactly that: `n_g` p95/p5 = **2.1×** (`week|nation`), 2.0× (`day|region`),
2.2× (`month|nation`) — narrower than the 2.9–3.5× band, so *the whole key set flips at once*.
Real data is not: SO `posts|month` 219×, `posts|day` 8.3×, CB `hits|region` 584×,
`hits|date|region` 234×. Measured share of the key set that can flip: **94% on TPC-H
`week|nation`, 0.0% on every StackOverflow and ClickBench query.**

**2. Holding `k_u` fixed at 49 — far above the crossover — dispersion alone destroys the gain.**
Synthetic bipartite key sets, every PU in exactly 49 groups, group sizes lognormal(0,σ), value 1
per cell (so a group's total *is* its `n_g`):

| `n_g` p95/p5 | 1.2 | 3.7 | 14 | 50 | 209 | 581 |
|---|---|---|---|---|---|---|
| gain, total ℓ1 | **2.82×** | 2.25× | 1.97× | 1.76× | 1.60× | **1.47×** |
| gain, mean per-group rel. err | **2.94×** | 1.93× | 1.47× | 1.26× | 1.15× | **1.08×** |

So the condition is `k_u` concentrated **and** `n_g` concentrated. The second half was invisible
because TPC-H is uniform by construction.

**3. Each query resonates at one ε, about a factor 2 wide.** Same query, same data, tuned at each ε:

| ε | 0.1 | 0.25 | 0.5 | 1.0 | 2.0 | 4.0 |
|---|---|---|---|---|---|---|
| `week\|nation` gain | 1.00× | 1.00× | 1.01× | **4.53×** | 1.63× | 1.42× |
| `month\|nation` gain | 1.00× | **3.59×** | 2.54× | 1.26× | — | — |

The published table — gain rising monotonically as `n_g/k_u` falls — is a **slice at ε = 1** of a
per-query resonance, not evidence that fine groupings benefit. `month|nation` is a 3.6× query at
ε = 0.25 and a 1.3× query at ε = 1. Below the window both arms return ~100%; above it, the gain
decays to the leftover-budget residual.

### Is the total-ℓ1 metric hiding anything? On TPC-H no — because TPC-H cannot tell metrics apart

Re-tuning **both** arms separately under six metrics on the flagship query (ε=1):

| metric | Laplace | Gaussian | gain |
|---|---|---|---|
| total rel. ℓ1 (as published) | 0.7338 | 0.1608 | 4.56× |
| median per-group rel. err | 1.0000 | 0.1102 | 9.07× (degenerate: Laplace releases <50%) |
| mean per-group rel. err | 0.7480 | 0.1971 | 3.79× |
| mean capped at 1 | 0.7476 | 0.1970 | 3.79× |
| nRMSE | 0.8276 | 0.2237 | 3.70× |
| **p95 per-group rel. err** | 1.0000 | 0.9866 | **1.01×** |
| **ℓ1 on the intersection of released sets** | 0.2227 | 0.1468 | **1.52×** |

The newly released groups are *not* junk here: released rel. err p50 0.102 / p95 0.44, only 3.4%
above 0.5, and the median error is flat across true-size quintiles (0.12/0.10/0.10/0.10/0.10).
Release is only mildly size-biased (69.9% of the smallest quintile, ~100% of the rest). **But this
is unfalsifiable on TPC-H**: its group totals span p95/p5 = 2.1×, so a total-mass metric and a
per-group metric are near-identical by construction. Two checks that break it:

- **Inject realistic group-size skew** (lognormal, p95/p5 = 169×; vote structure untouched):
  ℓ1 gain 4.55× → **3.33×**, mean per-group rel. err gain → **1.18×**, p95 → **1.00×**. 19% of
  Gaussian's released groups are then *worse than silence* (rel. err > 1) and the smallest
  quintile's median error is 1.74.
- **The intersection number is not a vote-geometry effect at all.** 1.52× is exactly the value
  budget ratio 0.598/0.398 = 1.50: on the groups both arms release, Gaussian is ahead only because
  Laplace had to buy a lower τ with ε.

**The metric issue is real but it belongs to the whole document, not to this result.** On ClickBench
`hits|date|region` the tuned release scores **ℓ1 = 0.086** while its mean per-group error is
**0.90** — 6.8% of groups released, covering 95.1% of the mass. Every headline in this file is a
total-ℓ1 number, and total ℓ1 reports 8.6% error for a release that answers 7% of the key set.

### Off TPC-H it loses, end to end

`dataset_profile.py` compared release counts; this is the full pipeline with error, both arms
tuned (`metric_audit.py --query`):

| query | `k_h` | ℓ1 Laplace | ℓ1 Gaussian | gain | released |
|---|---|---|---|---|---|
| so posts\|month | 1.9 | 0.3666 | 0.3687 | **0.99×** | 162 → 162 of 190 |
| so posts\|day | 3.0 | 0.8998 | 0.9964 | **0.90×** | 636 → **21** of 5,107 |
| cb hits\|date\|region | 1.0 | 0.0859 | 0.0939 | **0.91×** | 512 → 437 of 7,564 |

Gaussian votes lose on *every* metric on *every* non-TPC-H query tested. Stated plainly: **of the
three datasets available, the vote-geometry result is positive on one, and that one is synthetic
and uniform.**

### The crossover, derived and confirmed — and the downside is bounded

Since both thresholds are `1 + const/ε_η`, the ε a group *needs* is `const/eff`, so the whole
comparison is one ε-cost ratio, independent of ε and of `n_g`:

```
f  =  min_C [(thr_G(C)−1)/eff(C)]  /  min_C [(τ_L(C)−1)/eff(C)]        f < 1 ⇒ Gaussian wins
```

For a homogeneous group `f = ρ/√k_h` exactly. `ρ` is astonishingly stable: **2.09** (δ=1e−3),
**2.02** (1e−6), **2.01** (1e−9) — so **the crossover is `k_h = ρ² ≈ 4.1`, i.e. 5 in integers,**
and the best Gaussian can ever do is `√k_h/ρ`. Monte-Carlo (20k trials, the smallest `n_g` each
arm releases ≥50% of the time) reproduces it to two digits:

| `k_u` | 1 | 2 | 3 | 4 | 5 | 6 | 8 | 20 | 50 |
|---|---|---|---|---|---|---|---|---|---|
| measured `n_g*` ratio G/L | 1.99 | 1.44 | 1.19 | 1.04 | 0.94 | 0.86 | 0.75 | 0.55 | 0.34 |
| predicted `ρ/√k` | 2.02 | 1.47 | 1.22 | 1.07 | 0.96 | 0.88 | 0.77 | 0.52 | 0.35 |

Three deliverables from this:

- **`k_h` must be the harmonic mean, not the mean and not the max.** Four `k_u` shapes with mean
  20: all-20 → `f` = 0.52; half-39/half-1 → 2.02; 5%-at-381 → 2.02; 1%-at-1901 → 2.02.
  A heavy tail buys *nothing*: the PUs with `k_u`=1 already vote at full weight under truncation.
- **The adversarial case is exactly `k_h < 4`, and it costs a factor `ρ`.** With `k_h` ≈ 1 the
  tuner drives Gaussian to `C_e`=1, where `eff` is identical for both arms and `f = ρ` *exactly* —
  Gaussian needs **2.02× the ε** for the same key set (2.09× at δ=1e−3, where it is worst).
- **…which also means the downside is bounded and the upside is not.** `C_e`=1 is always available,
  so a wrongly-chosen Gaussian never costs more than ρ ≈ 2.1× in ε, while a wrongly-chosen Laplace
  costs `√k_h/ρ`, unbounded (7.9× at `k_h`=250). For the port: **choose Gaussian iff harmonic-mean
  `k_u` > 4.1**, and prefer it when the statistic is uncertain — but that statistic is
  data-dependent, which is the same unresolved accounting question flagged above.

**Honest headline:** Gaussian votes are right for the vote channel and the crossover math holds,
but "4.2×–6.0×" is the peak of a resonance measured on the one dataset whose group sizes are
uniform. Conditioned on all three requirements — `k_h` > 4, `n_g` spread narrower than `√k_h/ρ`,
and ε inside the query's window — the honest range off the resonance is **1.1×–1.6×**.

## END-TO-END OFF TPC-H — and the unifying condition for the whole approach

`attacks/cross_dataset.py` runs the real pipeline (bound selection, clipping, partition selection,
value noise) on StackOverflow and ClickBench, scored as before. Every arm tuned over its own
`C_e` × `C_v` × split:

| query | groups | `k_u` med/max | published | ours-Laplace | ours-Gaussian | gain |
|---|---|---|---|---|---|---|
| so posts\|month | 190 | 1/156 | 32.65% | 36.36% | 36.61% | **0.90×** |
| so posts\|month score | 190 | 1/156 | 57.92% | 47.26% | 48.07% | 1.23× |
| so posts\|day | 5,107 | 1/2126 | 91.27% | 89.26% | 99.73% | 1.02× |
| so comments\|month | 182 | 1/154 | 43.56% | 47.63% | 47.14% | **0.92×** |
| so badges\|month | 165 | 1/154 | 8.52% | 5.90% | 6.22% | 1.45× |
| cb hits\|region | 3,238 | 1/54 | 6.13% | 5.87% | 6.10% | 1.04× |
| cb hits\|date\|region | 7,564 | 1/58 | 9.98% | 8.68% | 9.15% | 1.15× |
| cb width\|region | 3,238 | 1/54 | 6.50% | 6.36% | 7.02% | 1.02× |

**Gaussian votes lose on all eight**, exactly as `dataset_profile.py` predicted from the `k_u`
shape — the analytic prediction and the end-to-end measurement agree, which is the strongest
check either has had. And on two queries **the whole package is worse than Google as published**
(0.90×, 0.92×). Range off TPC-H: **0.90×–1.45×**, against 4.2×–6.0× on TPC-H.

**Why — and this unifies the two channels into one condition.** Google's value sensitivity is
`C_v·U` (ApproxBounds over *cells*); ours is `B` (over per-PU *norms*). We win iff `C_v·U / B` is
large:

| query | `k_u` med/max | `U` (cells) | `B` (norms) | `B/U` | `C_v·U/B` at `C_v`=1 |
|---|---|---|---|---|---|
| so posts\|month | 1/156 | 4 | 4 | 1.0 | 1.00 |
| so comments\|month | 1/154 | 4 | 2 | 0.5 | 2.00 |
| so badges\|month | 1/154 | 4 | 8 | **2.0** | 0.50 |
| cb hits\|region | 1/54 | 64 | 64 | 1.0 | 1.00 |
| tpch month\|nation | 30/72 | 524,288 | 4,194,304 | 8.0 | 0.12 |

When `k_u` is heavy-tailed, **Google sets `C_v` = 1 and loses almost nothing** (the median user
touches one group), so its sensitivity is just `U` — and `U ≈ B`, since a median user's norm *is*
their single cell. We gain nothing, and where the tail pushes `B` above `U` (badges: `B/U` = 2.0)
we are strictly **worse**. On TPC-H, `B/U` = 8 looks bad for us in isolation, but Google is
*forced* to `C_v` ≈ 10 because truncating a customer spread over 30–55 months to one month would
discard ~97% of their mass — so `C_v·U` = 10`U` beats `B` = 8`U` only by 1.25×, the sensitivity
ratio measured earlier.

**So the single condition governing this entire line of work is:**

> **The mechanism helps iff Google is *forced* into a large `C_u`, which happens iff `k_u` is
> concentrated — every privacy unit spreading across many groups. It is the same condition for
> the value channel (`C_v·U` vs `B`) and the vote channel (truncation cost vs `√C_e`).**

That is a much more useful claim than any of the ratios: it says *in advance* which workloads to
expect gains on, it is measurable from public schema knowledge plus one cheap statistic, and it
explains why the same mechanism gives 6.0× on TPC-H and 0.90× on StackOverflow.

**Honest summary across everything measured:** 4.2×–6.0× where `k_u` is concentrated (TPC-H),
0.90×–1.45× where it is heavy-tailed (StackOverflow, ClickBench), and — separately — the
difference between an empty answer and 94% key-set recovery on the two queries where Laplace
partition selection returns nothing at any budget.

## THE GAIN IS A RESONANCE, NOT A PLATEAU — and the SASS port is a loss

Two adversarial reports plus my own independent re-measurement. This supersedes the scoping in
the two sections above.

**1. The gain is a window in ε.** Retuning *both* arms at every ε on `month|nation` (my own run,
independent of the agent's):

| ε | 0.10 | 0.25 | 0.50 | 1.00 | 2.00 | 4.00 |
|---|---|---|---|---|---|---|
| Laplace | 100.0% | 54.51% | 9.56% | 3.91% | 3.04% | 1.26% |
| Gaussian | 99.96% | 16.45% | 6.64% | 3.10% | 2.78% | 1.24% |
| **gain** | 1.00× | **3.31×** | 1.44× | 1.26× | 1.09× | 1.02× |

The agent's independent sweep of the flagship query agrees in shape: 1.00× (0.1), 1.00× (0.25),
1.01× (0.5), **4.53×** (1.0), 1.63× (2.0), 1.42× (4.0). **Each query has its own resonant ε**, and
the published "gain grows as `n_g/k_u` falls" table is a slice at ε=1 through several different
resonances — not evidence about grouping fineness at all.

**2. The mechanism of the resonance, which subsumes the earlier `k_u` condition.** Both rules
threshold the same effective count, so the groups that flip are those with
`thr_G(C*) < n_g < k_h·τ_L(1)` — a band of fixed width `√k_h/ρ` ≈ **3.5×** in `n_g`. The gain is
therefore *the share of the key set lying inside one 3.5×-wide window*. TPC-H's group sizes span
only **2.1×** (p95/p5), narrower than the band, so ~94% of its key set flips at once; StackOverflow
and ClickBench span **8.3×–584×**, so **0.0%** of their key sets can flip on any query tested.

A controlled synthetic proof separates this from `k_u`: fixing every PU at `k_u`=49 (far above
crossover) and varying **only** group-size dispersion moves the gain 2.82× → **1.47×** (ℓ1) and
2.94× → **1.08×** (per-group) as `n_g` p95/p5 goes 1.2 → 581. **So `k_u` concentration is
necessary but not sufficient** — my scope condition was one-sided. The full condition is:

> harmonic-mean `k_u` > 4.1, **and** group sizes dispersed by less than ≈3.5×, **and** ε
> positioned so the band overlaps the key set.

TPC-H satisfies all three by construction — uniform group sizes, uniform `k_u`, and ε=1 happens to
land the band on `week|nation`. Nothing else tested does.

**3. The crossover is the harmonic mean, and a heavy tail buys nothing.** `ρ` = 2.02 at δ=1e−6
(2.09 at 1e−3, 2.01 at 1e−9), so the crossover is `k_h` = `ρ²` ≈ **4.1**. Four shapes with *mean*
`k_u` = 20: all-20 → `f`=0.52 (Gaussian wins); half-39/half-1 → 2.02; 5%-at-381 → 2.02;
1%-at-1901 → 2.02 (Gaussian loses identically). **A useful asymmetry for the port:** `C_e`=1 is
always available, so a wrong *Gaussian* choice costs at most `ρ` ≈ 2.1× in ε, while a wrong
*Laplace* choice is unbounded (7.9× at `k_h`=250). **Prefer Gaussian under uncertainty.**

**4. The metric that produced every headline in this document is unsafe on skewed data.** On
`cb hits|date|region` a tuned release scores **ℓ1 = 8.6%** while its *mean per-group* error is
**90%** — it releases 6.8% of groups covering 95.1% of the mass. Injecting realistic skew into
TPC-H (`n_g` p95/p5 = 169×, vote structure untouched) takes the ℓ1 gain 4.55× → 3.33× but the mean
per-group gain to **1.18×** and p95 per-group to **1.00×**, with 19% of Gaussian's releases worse
than silence. On unskewed TPC-H the result *does* survive six metrics (4.56× ℓ1, 3.79× mean
per-group, 3.70× nRMSE, released-group p50 error 0.10 and flat across size quintiles) — but TPC-H
cannot test the failure mode, because its group totals span only 2.1×.

**5. The SASS port loses under this document's own scoring rule.** The transfer agent measured a
73.8× key-set gain inside genuine smooth-sensitivity accounting (verified: exact NRS envelope,
64 lanes, reproduces `dp_smooth_median_noise_scale` to 16 digits, and matches the built extension
across 12 configs). The refutation confirmed the floor arithmetic (28.63/43.66/74.66 at c=1/2/4)
and reproduced the key-set gain (68.7× vs 73.8×) — **but that metric charges released groups zero
error**, so the median machinery is dead code for every number in it. Under relative ℓ1 with
suppressed groups charged full error:

| query | Laplace votes | Gaussian votes |
|---|---|---|
| month\|nation, c=1 | 252.5% | **288.3%** |
| month\|nation, c=2 | 100.0% | **382.9%** |
| month\|nation, c=4 | 100.0% | 122.3% |

**Why it inverts, and this is the interesting part:** Laplace's τ grows *linearly* in `C`, so
Laplace can always buy total suppression — releasing nothing scores 100%. Gaussian's threshold
grows only as `√C`, so **it cannot fall back to silence**; the very property that wins the key set
forces it to release groups whose value error exceeds their mass. Releasing more is worse when the
value channel is broken.

**And the SASS value channel is broken independently**, in a pincer: at small `C_v` the rank cap
biases every group by `1 − C_v/k_u` (≈98% on these groupings); at large `C_v`,
`ε_cell = ε/((c+1)C_v)` drives `β` so small that the NRS envelope collapses to its sentinel term
`2Λe^{−65β}`, needing `ε_cell ≳ 3` (i.e. ε ≳ 6) to decay. The built extension agrees — its best
config on the *coarsest* grouping is 102% rel-ℓ1. **`month|nation` already releases 96.6% of its
key set at c=1 and still scores 252%: τ-suppression is not the bottleneck in SASS.** Fixing the
vote geometry there optimises a channel that is not binding.

**Two corrections to the transfer agent's own claims**, both found by the refutation: "SASS is
worse off than `dp_standard`" is false — `FinalizeDPLaplace:2790` uses the *identical*
`ε/(k+1)`, so the comparison was against an idealised `dp_standard` that nothing implements (the
untuned-baseline error, inverted). And the plan's structural change is wrong: `BuildRankCapFilter`
ANDs every spec, so a second `RankCapSpec` applies `min(C_e,C_v)` — it tightens rather than
decouples.

### Revised honest summary

| setting | gain |
|---|---|
| TPC-H, at each query's resonant ε | 3.3×–6.0× |
| TPC-H, off resonance | 1.0×–1.6× |
| TPC-H with realistic group-size skew, per-group metrics | 1.00×–1.18× |
| StackOverflow / ClickBench, end-to-end | **0.90×–1.45×** (sign flips) |
| smooth-sensitivity SASS, end-to-end | **loses** (252% → 288%) |

The durable results are the **negative and structural** ones: the ℓ1/ℓ2 rule itself; the proof
that Laplace partition selection has an immovable `ln(1/2δ_η)/ε_η` floor; the `k_h` ≈ 4.1
crossover with its asymmetric risk; and the finding that total-ℓ1 hides a release answering 7% of
a key set. **The utility headline does not survive as a general claim.**

## FIVE MORE LEVERS TRIED — four dead, one free but not a differentiator

**Dual clip: buy ℓ2 slack on the value channel by forbidding concentration — 0.93×.** The rule
said values are Laplace-only because a PU may concentrate all mass in one group (`Δ₂ = Δ₁ = B`).
But concentration is a *choice*: cap each cell at `b` **then** norm-clip to `B`, giving
`‖v‖₁ ≤ B` and `‖v‖_∞ ≤ b`, hence `‖v‖₂ ≤ √(B·b)`. Gaussian then wins iff `B/b > ln(1.25/δ)` ≈ 14.
Google has no equivalent knob — its per-cell cap `U` must also carry the bound. It still fails,
and the sweep shows exactly why:

| `b` | B/1 | B/4 | B/16 | B/64 | B/256 |
|---|---|---|---|---|---|
| error | 13.36% | 7.29% | **4.33%** | 10.98% | 61.63% |

A squeeze: the noise needs `B/b` > 28 to beat Laplace, but bias explodes by `B/64`. On TPC-H the
per-PU norm is only ≈5.5× the max cell, so there is no room between "cap bites" and "ℓ2 pays".
It would open on data where a PU's norm greatly exceeds its largest cell. `attacks/dual_clip.py`.

**MSE-optimal `B` instead of ApproxBounds — 1.010×.** Adversarial review found ApproxBounds is a
max-finder and costs *Google* 2× on `U`; the same criticism applied to our `B` yields nothing —
the MSE objective selects the **same bound** ApproxBounds does (4,194,304). Our bound was already
at its optimum.

**Debiasing the clip loss — 0.854×, now a loss.** Estimating the clipped mass from the noisy norm
histogram and adding it back proportionally *hurts*, because the clip removes mass from the
whales' groups specifically, not proportionally. The 1.19×–1.25× recorded earlier was measured
against the old baseline and **does not survive**. That was the last unvalidated positive in this
document. `attacks/bound_and_debias.py`.

**Marginal reconciliation — 0.791×.** Composite keys are a 2-D grid, so release the grid plus both
marginals and solve for the closest consistent table (Hay et al.); reconciliation is free
post-processing. It loses at every budget share, and the diagnostic says why: the `month|nation`
grid is **99.8% dense**, so marginals carry almost no information the cells lack, while the 3-way
split costs real noise. This predicts where it *would* pay — sparse grids — which is a different
regime from the one we are in. `attacks/marginal_reconcile.py`.

**Count-conditioned shrinkage — 1.02×–2.15×, free, but Google gets it too.** We already pay `ε_η`
for a noisy per-group PU count and use it only for τ. It is strongly correlated with the SUM, so
regressing the released sums on the released counts and shrinking toward the fit is pure
post-processing. Plain James–Stein failed (1.001×) because between-group spread swamps the noise;
*conditioned on the count* that spread mostly vanishes:

| query | corr | ours | +regr | gain | Google | +regr | gain |
|---|---|---|---|---|---|---|---|
| so posts\|month | 0.986 | 36.48% | 16.99% | **2.15×** | 33.69% | 16.72% | **2.01×** |
| so posts\|month score | 0.561 | 47.19% | 44.03% | 1.07× | 59.83% | 48.36% | 1.24× |
| cb hits\|region | 0.982 | 5.81% | 5.69% | 1.02× | 6.19% | 6.10% | 1.01× |

The gain tracks the correlation, and **Google gains as much or more**. So it is a genuine free
improvement to the Wilson-et-al. line — nobody does it today, and it is worth publishing on its
own — but it moves both arms and does not widen the gap. (On pure COUNT queries the correlation is
near-tautological, so the 2.15× there should not be quoted as typical.)

## WHERE THE ARGUMENT ACTUALLY STANDS

Against **Google DP as published** — Wilson et al. and the library, one `C_u`, Laplace everywhere —
we win **3.2×–6.0×** on TPC-H. That claim is intact and is the one a reader can check against a
real system.

Against the **steelman**, we win 1.2×–1.5×. But the steelman is not a system that exists: the
rescale-to-true-total trick and the `C_e`/`C_v` decoupling were invented *in this project's own
adversarial review*. Reporting only that number would be self-flagellation rather than science.

**And the steelman is fragile in a way our mechanism is not.** From the alignment probe, holding
every marginal fixed and tilting only how each PU spreads its own mass:

| alignment | 1.07 (real TPC-H) | 3.81 | 7.58 | 12.04 |
|---|---|---|---|---|
| Google + rescale | 5.59% | 9.41% | 17.81% | **22.00%** |
| ours | 4.13% | 4.06% | 4.07% | **4.23%** |
| gap | 1.35× | 2.32× | 4.35× | **4.56×** |

**Ours is flat across the entire range; the steelman degrades 4×.** Rescale works by assuming a
PU's dropped mass belongs in its largest groups — true when users disagree about which group is
biggest, false under any seasonality or shared trend. So the honest headline is not one number:

> Against deployed Google DP, 3.2×–6.0×. Against the best Google DP we could construct, 1.2×–1.5×
> on uniformly-allocated data and 2.3×–4.6× as soon as privacy units share allocation structure —
> because the ℓ1 clip never redistributes mass, and every truncation-based method must.

## DANDAN'S ADAPTIVE BASE WORKS — 1.68× worst case, and my earlier dismissal was an artifact

Her 13 Aug proposal: use the **filterless `U`** (frozen metadata) to shrink the histogram *range*,
then spend the saved bins on *resolution* — keep ≤64 bins and the same `ε_b`, but make the base
`(U_hi/U_lo)^(1/64)` instead of 2.

**This is not what `attacks/bound_grid.py` tested.** That made the base finer over a *fixed* range
`[1, 2^45]`, so base 2^(1/8) needed 360 bins and paid a higher ApproxBounds threshold. Her version
holds the bin count at 64 and buys resolution by narrowing the range — a materially different, and
better, proposal. It measured 1.009× my way and **1.68× hers**.

**Why my measurement missed it.** The error curve in `B` has a basin only **1.3× wide** (within
10% of optimal for `B` ∈ [0.88, 1.18]·`B*`), while a base-2 grid has **2.0× spacing** — *coarser
than the basin*. So base-2 lands wherever the data's maximum happens to sit relative to a power of
two, and TPC-H happens to sit at a lucky spot. Rescaling the measure by `s` ∈ [1,2) sweeps every
alignment:

| scheme | s=1.00 | 1.15 | 1.32 | 1.52 | 1.74 | 1.95 | worst | spread |
|---|---|---|---|---|---|---|---|---|
| base 2 (width 2.00) | **3.53%** | 5.68% | 6.00% | 5.60% | 5.24% | 4.96% | 6.00% | **1.70×** |
| range 2³², 64 bins (1.41) | 4.19% | 4.18% | 4.18% | 4.17% | 4.16% | 4.15% | 4.19% | 1.01× |
| range 2¹⁶, 64 bins (1.19) | **3.58%** | 3.58% | 3.58% | 3.58% | 3.58% | 3.58% | **3.58%** | **1.00×** |

**Worst case 6.00% → 3.58% = 1.68×; mean over alignments 5.17% → 3.58% = 1.44×.** The fine grid is
*exactly* alignment-invariant, as it should be. My single-point measurement at `s`=1.00 landed on
base-2's best alignment — **the same one-lucky-operating-point error this document has caught six
times, committed again.**

**Two refinements to her mechanism, from the measurements.**

1. **Don't also switch to an MSE objective.** ApproxBounds targets the *maximum*, but the
   error-optimal `B*` = **0.61 × the true max** — so it aims at the wrong target and two offsets
   happen to cancel. Aiming at `B*` directly is worse, because it must be estimated from the noisy
   histogram: mean |log₂(B/B*)| is 0.079 (max-finder, base 2), **0.066** (max-finder, fine grid),
   0.128–0.334 (MSE). **Keep the max-finder; just make its grid finer.**
2. **Range 2¹⁶ beats range 2³².** Too wide a range wastes resolution below anything that occurs;
   too narrow and the bottom bin clips. 2¹⁶ (width 1.19) was best here.

**Why it does not need the filterless `U` specifically.** Any public upper bound sets the range —
the filterless max is one source, but so is a domain constant. On this query the filterless `U`
(7,244,710) is only 1.01× the filtered max, so the frozen-metadata machinery is not what earns the
1.68×; the finite range is. That makes the result *simpler* than the proposal: it needs one public
scale, not a metadata-correspondence system.

## DANDAN'S FROZEN GROUP SET — 8.2×, the largest gain found in this project

Her 13 Aug proposal: release the group set **once** from the filterless data under its own
`(ε₀, δ₀)`, freeze it as `G_fix`, and let every later filtered query skip τ for those groups.

**It is sound, and the reason is worth stating precisely: partition selection protects group
EXISTENCE, not values.** Once `G_fix` is a released (hence public) object, conditioning later
releases on it is post-processing. A frozen group holding a single user after filtering is still
safe — its value carries the usual `Laplace(B/ε_v)`. This is exactly the public-partition model
FLEX/Chorus assume and that `dp_elastic` already relies on, except *obtained legitimately* rather
than assumed. Her own worry — that freezing rare groups would leak them — is handled by
construction: a group that cannot clear `τ₀` never enters `G_fix`.

**There are two gains, and the second is the larger.** Groups in `G_fix` are never suppressed;
and `ε_η` is no longer needed for them, so the entire partition-selection budget — 30–60% of ε —
moves to the value channel.

**The structural reason it works:** the filterless query has the *most* users per group and clears
τ most easily; filtered queries have the fewest and suffer most. Freezing transfers the easy
query's key set to the hard ones. On `week|nation` (τ-binding, filter `acctbal ≥ 8000`), `G_fix`
built at `ε₀`=1, `δ₀`=1e-4 captured **100% of groups** — the filterless query has 5.5× more users
per group, so nothing is marginal there.

**Fair comparison, charging `ε₀/N` to the frozen arm so both spend the same total per query:**

| N queries | query ε | τ only | with `G_fix` | gain |
|---|---|---|---|---|
| 1 | 0.000 | 73.31% | 100.00% | **0.73×** |
| 2 | 0.500 | 73.31% | 17.98% | **4.08×** |
| 5 | 0.800 | 73.31% | 11.05% | 6.63× |
| 20 | 0.950 | 73.31% | 9.40% | 7.80× |
| 100 | 0.990 | 73.31% | 9.06% | **8.09×** |

**Break-even at N = 2.** At N=1 it correctly loses (you paid twice for one answer). On the
non-τ-binding `month|nation` the gain is 1.83×, entirely from the freed `ε_η`.

**This is larger than the Gaussian-votes fix (4.46×) and than anything else in this document.** The
two compose rather than compete: `G_fix` handles groupings seen before, Gaussian votes handle novel
ones — and Gaussian is what you want for the *first* query, which is exactly where `G_fix` cannot
help.

**Three costs to state honestly.** One `G_fix` is needed **per grouping key**, and the space of
groupings is large — this is an amortisation over *repeated* queries on the *same* grouping, not
over all queries. `G_fix` is only valid while the data is static; after writes it must be re-paid
(the metadata-refresh problem already open in this document). And it answers a slightly different
question — the released key set is the *filterless* one, so a group empty after filtering returns
noise centred on zero rather than being absent. That is arguably the more useful answer for a
dashboard, but it is not the same object τ-thresholding returns.

## Dandan's 14 Aug follow-ups: positional storage is unsafe, and the public bound is optional

**Storing row positions instead of group values does not work.** She proposed making the filterless
query deterministic in content *and tuple order*, then storing only row positions (run-length
compressed) and re-running the filterless query to recover the groups. Measured:

| grouping | groups | as values | as positions | filterless re-run |
|---|---|---|---|---|
| month | 84 | 0.7 KB | 0.3 KB | 3.35 s |
| month\|nation | 2,100 | 21.7 KB | 8.2 KB | 4.13 s |
| day\|region | 12,630 | 160.3 KB | 49.3 KB | 3.38 s |
| day\|nation | 63,150 | **838.7 KB** | 246.7 KB | 3.86 s |

Two reasons not to: the storage being optimised is under 1 MB even at 63,150 groups, while the
recovery costs **3–4 s of full-scan on every filtered query**; and the determinism premise is false.
**Four runs of the same `GROUP BY` with no `ORDER BY` returned four distinct orderings**, and
changing `threads` from 2 to 4 changed it again — so positional storage would silently return the
*wrong* groups. (This also independently confirms the row-order nondeterminism an earlier audit
flagged in `taubinding_headtohead.py`.) An explicit `ORDER BY` would fix correctness but adds a
sort to every recovery.

**Peter's objection about join queries is real, and the fix is not a two-stage bound.** Tested
splitting `ε_b` between a coarse base-2 pass to locate the magnitude and a fine 64-bin pass in a
window around it — no public bound needed:

| scheme | needs public bound? | median `B` | error |
|---|---|---|---|
| base 2, full type range (current) | no | 4,194,304 | **3.96%** |
| 1-stage fine grid, public `U` | **yes** | 4,307,730 | 3.97% |
| 2-stage: coarse then fine | no | 5,558,445 | 5.22% |

Two-stage **loses** — halving an already-tiny `ε_b` raises the ApproxBounds threshold enough to
move the selected bin. But the first row is the real answer: **at TPC-H's alignment base-2 and the
fine grid are identical (3.96% vs 3.97%)**. The fine grid's value is *insurance* against unlucky
alignment (1.70× worst case), not a gain at any given alignment. So a query with no public bound
simply keeps base-2 and accepts the alignment lottery — it does not break.

**And when the bound is worth having, it comes from the same place as the group set.** The
filterless `U` is itself a DP release that can be frozen and reused exactly like `G_fix`, so one
entry in the persistent table carries *both* the releasable groups and the bound range for that
(filterless query, grouping) pair. Dandan's two proposals share one mechanism.

## THE PERSISTENT TABLE, INVESTIGATED — it works, with a statically decidable condition

Four arms, each spending the same total per query; the frozen arms additionally pay `ε₀/N` with
N=20. `goog-frozen` is a fair arm, not a strawman — Privacy on Beam (`SelectPartitions` →
`PublicPartitions`) and Tumult (`get_groups` → `KeySet`) both support it.
`attacks/frozen_vs_google.py`.

| filter | frozen-but-absent | goog-τ | goog-frozen | ours-τ | ours-frozen | vs published |
|---|---|---|---|---|---|---|
| none | 0 | 4.25% | 3.52% | 0.95% | **0.79%** | **5.4×** |
| PU: acctbal≥8000 | 0 | 13.50% | 9.76% | 3.95% | **2.28%** | **5.9×** |
| PU: acctbal≥9900 | 9 | 99.99% | 65.37% | 100.0% | **44.77%** | 2.2× |
| GRP: ship≥1996 | 1,200 | 1.72% | 2.23% | 0.44% | 0.94% | 1.8× |
| GRP: ship≥1998 | 1,800 | 0.72% | 2.99% | 0.24% | 1.19% | **0.61×** |
| GRP: ship≥1998 & nation<5 | 2,027 | 0.68% | 10.28% | 0.22% | 4.62% | **0.15×** |

**The failure mode is real and it is not subtle.** A filter on the *privacy unit* removes users but
leaves the groups, so τ binds harder and freezing wins 5–6×. A filter on the *grouping key* deletes
whole groups — up to 2,027 of 2,085 frozen groups no longer exist, get released anyway, and
contribute pure noise. Freezing then loses by 5–21× against its own τ baseline, and by 6.7× against
Google. It hits `goog-frozen` just as hard (0.68% → 10.28%), so this is inherent to freezing, not to
our clip.

**The fix is free, and it is the interesting part.** When the filter constrains the grouping key,
*which frozen groups survive is determined by the predicate alone* — no data access — so `G_fix`
can be pruned publicly before use:

| filter | `G_fix` | pruned | frozen-raw | frozen-pruned | τ | gain |
|---|---|---|---|---|---|---|
| ship≥1996 | 2,085 | 885 | 0.94% | **0.39%** | 0.44% | 1.14× |
| ship≥1998 | 2,085 | 285 | 1.18% | **0.23%** | 0.24% | 1.02× |
| nation<5 | 2,085 | 417 | 2.37% | **0.80%** | 0.86% | 1.07× |
| both | 2,085 | 57 | 4.61% | **0.21%** | 0.21% | 1.00× |

Pruning turns a 5–21× loss into a small win or a wash. **With it, the frozen set is never harmful.**

**Why the gain is small on group-side filters, and this is the unifying point:** those filters
leave few groups but each still holds many users, so τ was never binding and removing it buys
nothing. **Freezing pays exactly when τ binds — i.e. when the filter thins the users inside groups
rather than deleting groups.**

**The applicability rule is decidable from the query text**, which suits a rewriter-based system:
*does the `WHERE` clause constrain columns that appear in, or functionally determine, the grouping
key?* If no → use `G_fix` directly, expect a large win. If yes → prune `G_fix` by the predicate
first, expect a wash. Never use it unpruned.

**Answering "how much vs Google DP":** 5.4×–5.9× against Google as published where τ binds, falling
to ~1.0× where it does not. Against Google using *its own* frozen-partition feature the gap is
4.5× (no filter) and 4.3× (acctbal≥8000) — and that residual is the ℓ1 clip, which carries its own
scope condition. The frozen table itself is worth 1.2×–2.9× to *either* mechanism.

## `C_u` CANNOT BE FROZEN — and this is the real case for the ℓ1 clip

Dandan asks (14 Aug) whether `C_u` must also be estimated privately and frozen, since it is
"relatively stable". The premise is right about one quantity and wrong about the one that matters,
and the consequence reframes result #5. `attacks/cu_automatic.py`.

**The data statistic *is* stable. The optimal parameter is not.** Across six filters on
`month|nation`, median `k_u` is **30 in every single case** and max `k_u` is 66–72 — exactly the
stability she describes. But Google's error-optimal `C_v` ranges **21–55, a 3× spread**, and is
nowhere near either statistic:

| filter | PUs | median `k_u` | max `k_u` | best `C_v` | Google | ours |
|---|---|---|---|---|---|---|
| none | 999,982 | 30 | 72 | **55** | 3.24% | 0.96% |
| acctbal≥4000 | 545,077 | 30 | 72 | **55** | 4.79% | 1.55% |
| acctbal≥8000 | 181,532 | 30 | 72 | **34** | 13.42% | 3.98% |
| acctbal≥9500 | 45,320 | 30 | 66 | **21** | 81.49% | 61.99% |
| mktseg=AUTOMOBILE | 200,165 | 30 | 68 | **55** | 13.02% | 3.54% |
| nation<5 | 199,738 | 30 | 68 | **55** | 2.55% | 0.85% |

`C_v` is not an estimate of `k_u` — it is a bias/variance tradeoff point that depends on the group
sizes, on `U`, and on how close the query sits to the τ floor. It therefore moves with the
*filter*, which is precisely what frozen metadata cannot track.

**And getting it wrong is brutally expensive, asymmetrically so** (penalty vs each filter's own
best `C_v`):

| filter | `C_v`=1 | 5 | 13 | 21 | 34 | 55 | 89 |
|---|---|---|---|---|---|---|---|
| none | **29.9×** | 25.9× | 18.1× | 11.0× | 3.2× | 1.0× | 1.6× |
| nation<5 | **38.0×** | 33.0× | 23.0× | 13.9× | 4.0× | 1.0× | 1.6× |
| acctbal≥8000 | 7.2× | 6.3× | 4.4× | 2.7× | 1.0× | 1.1× | 1.7× |
| acctbal≥9500 | 1.2× | 1.1× | 1.0× | 1.0× | 1.0× | 1.1× | 1.1× |

Under-estimating costs up to **38×**; over-estimating costs at most 1.7×. So a safe automatic
system must over-estimate `C_v` — and then pay the `C_v·U` noise for a bound it never uses. (The
`acctbal≥9500` row is flat only because τ-suppression dominates everything there.)

**This is the strongest case for the ℓ1 clip, and it is not a utility argument.** The norm clip has
**no `C_v` at all**: sensitivity is `B` however many groups a PU touches. Combined with the other
two results, a fully automatic system needs:

| parameter | Google | ours |
|---|---|---|
| value bound | `U` — ApproxBounds, automatic | `B` — ApproxBounds, automatic |
| `C_v` (values) | **must be chosen; 3× unstable; 38× if wrong** | **does not exist** |
| `C_e` (votes) | must be chosen | **provably 1**, and absent entirely under a frozen group set |

So the answer to "can we determine `C_u` automatically and freeze it" is: **for our mechanism the
question does not arise**, and for Google's it cannot be answered by freezing, because the quantity
that is stable is not the quantity that is needed.

**Result #5 should be re-framed on this basis.** Its utility margin against a hardened Google is
1.2×–1.5× and scope-limited — but it removes a parameter that is unfreezable, query-dependent, and
worth up to 38× when misspecified. For an *automatic* system that is the more valuable property,
and it is the argument that survives the prior-art check intact.

## All-or-Frozen (Dandan, 15 Aug): the analysis is right, the rule is inert

Her §2 replaces K per-group release tests with one global AND — release the query-specific group
set iff *every* group passes, else fall back to `G_fix`. **The privacy argument is correct.**
Conventional per-group thresholding leaks if *any* of a PU's ≤`C_u` new groups passes (an OR,
needing `ρ_τ ≤ 1−(1−δ)^{1/C_u} ≈ δ/C_u`); All-or-Frozen needs *all* of them to pass (an AND, so
`Pr ≤ ρ_τ^m ≤ ρ_τ`, worst case `m`=1, needing only `ρ_τ ≤ δ`). My implementation of her exact
threshold reproduces her predicted gap to the digit: `τ_PG − τ_AF` = 6,158 measured against her
`(C_u/ε_B)·log C_u` = 6,158. `attacks/all_or_frozen.py`.

**But it never fires — 0 out of 200 trials, at every setting tested**, including `C_u` from her own
70% rule and `ε_B` swept to 0.9 (i.e. 90% of the entire budget on the bounding histogram, against a
tuned optimum of 0.002):

| grouping | `ε_B` | `C_u` | `b` | `τ_AF` | per-group pass | **P[AllPass]** |
|---|---|---|---|---|---|---|
| month (84 groups) | 0.05 | 72 | 1,440 | 24,885 | 10.3% | **0.0%** |
| month | 0.90 | 37 | 41 | 710 | **98.81%** | **0.0%** |
| month\|nation (2,095) | 0.90 | 37 | 41 | 710 | 92.0% | **0.0%** |

**The reason is structural, not probabilistic, and that is the useful finding.** At a 98.81%
per-group rate over 84 groups, independent failures would give `0.9881^84` ≈ 36% AllPass. We see
0%. The explanation: 98.81% of 84 is exactly 83 — **one specific group fails every single time**.
The smallest group's largest histogram bin is **14** for `month` and **1** for `month|nation`,
while the threshold must sit at ≈17 noise scales (`τ_AF ≈ b·ln((e^{1/b}+B−1)/2δ)` ≈ 17.3`b` at
B=64, δ=1e-6). For that group to clear it you would need `b` < 0.81, i.e.
**`ε_B` > 37** — thirty-seven times the entire query budget.

**So a single permanently-small group vetoes the mechanism forever.** It is not a tuning problem
and no budget allocation fixes it: real group-size distributions have a long tail, the AND is taken
over the minimum, and the minimum is always tiny. The rule is correct and inert.

**What would have to change.** The AND must cover every group a PU could have created, and since we
cannot know which those are it covers all of them — so the minimum group has veto power. Any fix
has to break that link: restricting the AND to a subset needs to know publicly which groups are
large (circular), and blocking the group set only works if a PU's new groups cannot span blocks,
which `C_u` > 1 does not guarantee. I do not see a repair that keeps the OR→AND saving.

**Repair search — three directions, and the diagnosis moves off the AND entirely.**

**A. The real defect is the *reuse*, not the AND.** Her test is `max` over B=64 bins of a histogram
whose sensitivity is `C_u`, so two penalties stack: sensitivity `C_u` instead of 1, and a
max-over-bins term worth another `ln B`. A **dedicated distinct-PU count at `C_e` = 1** has neither:

| scheme | sensitivity | noise scale | threshold |
|---|---|---|---|
| reuse bounding histogram, per-group | 72 | 1,440 | 31,043 |
| reuse bounding histogram, All-or-Frozen | 72 | 1,440 | 24,885 |
| **dedicated distinct-PU count, `C_e`=1** | **1** | **2.5** | **33.8** |

**736× lower threshold**, for `ε_η` = 0.4 of budget. Reusing the histogram saves that budget and
pays 736× for it — a very bad trade, and it is what makes the mechanism inert. "Free" statistics
are not free when they are the wrong statistic: group *existence* is a question about privacy
units, and answering it with a histogram of *values* imports a sensitivity and a bin-count penalty
that have nothing to do with the question.

**B. The bloat that motivates §2 is already solved.** Measured frozen-groups-absent-from-the-query:
PU-side filters leave **0 and 9 of 2,084** — there is no bloat for All-or-Frozen to remove.
Group-side filters leave 1,800–2,027, and public predicate pruning removes them *exactly*
(4.61% → 0.21%), with no privacy cost, no AND, and no fallback branch.

**C. Narrowing the AND to groups not already in `G_fix` does not work.** Tempting, since a `G_fix`
group's existence is already public — but whether it *survives the filter* is not. Omitting it from
the compact output reveals that no PU in that group passed the filter, which is a one-PU fact. The
only leak-free version releases all of `G_fix` padded with noise, i.e. the bloat we started from.

**D. Blocking the AND by a PU-determined key component — valid, and the best available repair.**
The AND must cover every group a PU could create. But when part of the grouping key is
*functionally determined by the privacy unit* — a customer has exactly one nation — all of that
customer's groups share it, so the AND only has to cover one block. Verified directly: **max
distinct blocks touched by one PU = 1** over 300 sampled PUs. A tiny group in another nation can
then no longer veto everything. This reduces the AND from `K` groups to roughly `k_u`.

**With both repairs applied — dedicated count *and* blocking — it fires on 20%:**

| `ε_η` | τ | per-group pass | blocks all-pass | mass compacted |
|---|---|---|---|---|
| 0.10 | 132.2 | 99.0% | **20.0%** | 20.0% |
| 0.40 | 33.8 | 99.0% | **20.0%** | 20.0% |
| 0.60 | 22.9 | 99.0% | **20.0%** | 20.0% |

**Note it is completely insensitive to budget** — 20.0% at every `ε_η`. That is the final
diagnosis: *the AND fires for a block iff that block contains no group below τ*, which is a
property of the data, not of the budget. 20 of 25 blocks contain a group with fewer than 40 PUs,
and the smallest group has **1 PU** — a group that can never pass any valid threshold, by
construction, since that is exactly what thresholding exists to suppress. Those blocks are
permanently vetoed at any ε.

For an AND over a block to fire even half the time you need a per-group pass rate of **99.18%** at
84 groups per block, or 97.72% at 30. The block size cannot be reduced below `k_u`, because a PU
spans that many groups by definition — so the AND is inherently over a PU's whole footprint, and
fires only where that entire footprint is dense.

**And after repair A the AND is worth exactly zero.** `τ_PG − τ_AF` = `(C_u/ε)·log C_u`, and at
`C_u` = 1 the two thresholds are *identical* (`1 − (1−δ)^{1/1}` = `δ`). Since the τ-floor proof
shows `C_e` = 1 is exactly optimal, the OR→AND saving exists only in configurations the mechanism
should never be in. **The idea is correct, inert as specified, and unnecessary once the statistic
is chosen correctly.**

## THE FULL STACK — 4.8x-6.4x vs Google DP as published across six filters

`attacks/full_stack.py`, `month|nation` on sf10, matched total budget (frozen arms pay `eps_0/N`,
N=20). All filters are PU-side, which is where the applicability rule permits using the frozen set
directly; group-side filters need predicate pruning first and are covered in
`attacks/frozen_vs_google.py`. The ORACLE arm has the error-optimal bound *and* free partition
selection — not implementable, it bounds what any further tuning could reach.

| filter | published | +frozen | ours | ours+froz | ours+all | ORACLE | vs pub | headroom |
|---|---|---|---|---|---|---|---|---|
| acctbal>=8000 | 13.54% | 9.83% | 4.06% | 2.26% | **2.27%** | 1.62% | **5.96x** | 1.41x |
| acctbal>=9500 | 82.54% | 20.25% | 61.98% | 8.67% | **8.71%** | 2.91% | **9.47x** | 2.99x |
| acctbal<0 | 20.97% | 13.80% | 9.57% | 4.33% | **4.39%** | 2.19% | **4.78x** | 2.01x |
| mktseg=AUTOMOBILE | 13.06% | 8.37% | 3.53% | 2.11% | **2.09%** | 1.52% | **6.24x** | 1.38x |
| mktseg=BUILDING | 13.19% | 8.26% | 3.50% | 2.07% | **2.06%** | 1.55% | **6.40x** | 1.33x |
| acctbal>=9500 & AUTO | 100.0% | 65.14% | 100.0% | 45.55% | **42.02%** | 6.48% | 2.38x | **6.49x** |

**Median 6.10x, and it does not depend on the filter being a numeric threshold** — the two
categorical `mktsegment` filters give 6.24x and 6.40x, in line with the 5.96x threshold case. An
earlier version of this table had only two filters on one column; this is the widened one.

**Headroom tracks selectivity, and that localises what is left to win.** 1.33-1.41x on ordinary
filters, 2.0-3.0x as selectivity tightens, 6.5x on the compound filter. Since the oracle differs
from the stack in exactly two ways — a perfect bound and free partition selection — and bound
selection is already within 1.35x at normal selectivity, **the remaining headroom is almost
entirely the price of partition selection**, not the value channel.

**The compound filter is where everything breaks.** `acctbal>=9500 AND AUTOMOBILE` leaves ~9,000
customers: Google as published scores 100.0% (releases nothing usable) and so does our arm without
freezing. Only the frozen key set rescues it at all, 100.0% -> 45.55%, and even then the oracle is
at 6.48%. This is the regime the tau-floor proof describes, and it is where a frozen key set stops
being an optimisation and becomes the only thing that works.

**Shrinkage remains a wash** (2.26 vs 2.27, 8.67 vs 8.71, 2.11 vs 2.09) except on that compound
filter, 45.55% -> 42.02%, i.e. it helps only where noise dominates the signal entirely. It stays
redundant once freezing is in.

**One correction folded in:** `full_stack.py` originally computed frozen-set membership only over
groups *present* in the filtered query, so frozen-but-absent groups were never released and their
noise was never charged. Harmless for these PU-side filters (0 absent groups) but it would have
flattered any group-side filter. Now charged, as in `frozen_vs_google.py`.

## ALL-OR-FROZEN, COMPREHENSIVE — it can only be used when it is not needed

`attacks/all_or_frozen_full.py`. Eight queries across TPC-H, StackOverflow and ClickBench, three
variants: as-specified (max over 64 bins of the reused bounding histogram), +A (dedicated
distinct-PU count at `C_e`=1), +A+D (... and the AND taken per block, where a block is a
PU-determined grouping-key component).

| query | groups | min support | median | as-spec | +A | +A+D | mass | veto |
|---|---|---|---|---|---|---|---|---|
| tpch month | 84 | **192** | 765,457 | 0.0% | **100%** | 100% | 100% | min >= tau |
| tpch month\|nation | 2,095 | 1 | 27,340 | 0.0% | 0.0% | **20.0%** | 20.0% | 20/25 blocks |
| tpch month\|priority | 420 | 10 | 171,025 | 0.0% | 0.0% | 0.0% | 0.0% | min < tau |
| tpch day\|region | 12,630 | 12 | 7,152 | 0.0% | 0.0% | 0.3% | 0.3% | 5/5 blocks |
| so posts\|month | 190 | 1 | 4,140 | 0.0% | 0.0% | 0.0% | 0.0% | min < tau |
| so comments\|month | 182 | 1 | 3,620 | 0.0% | 0.0% | 0.0% | 0.0% | min < tau |
| cb hits\|region | 3,238 | 1 | 6 | 0.0% | 0.0% | 0.0% | 0.0% | min < tau |
| cb hits\|date\|region | 7,564 | 1 | 4 | 0.0% | 0.0% | 0.0% | 0.0% | min < tau |

**As specified it fires on none of the eight.** With repair A it fires on exactly one — `tpch
month`, the only query whose smallest group has more than one privacy unit (192). With repair D it
additionally reaches 20% of blocks on `month|nation` and 0.3% on `day|region`.

**The `veto` column is fully predictive, and no simulation is needed to compute it.** The outcome
is decided entirely by *min group support vs tau*: a group whose support is hopelessly below the
threshold fails with probability `1 - delta` every time, so it vetoes its scope permanently. This
is why the earlier budget sweep found the result identical at every `eps_eta` from 0.1 to 0.9.

**And that gives the decisive statement.** Any valid threshold must satisfy
`P[1 + Lap >= tau] <= delta`, so `tau` is far above 1 — meaning **a group containing a single
privacy unit can never pass, at any budget, by construction**. Five of the eight queries have min
support exactly 1. But the existence of single-PU groups is *precisely why partition selection
exists at all*: if no group had one user, nothing would need suppressing.

> **All-or-Frozen therefore requires that the query contain no singleton group — and a query with
> no singleton group does not need partition selection in the first place. The mechanism can only
> be used where it is not needed.**

`tpch month` confirms this exactly: it is the one query where the rule fires, and it is also the
one query where tau suppresses nothing, so the compact and frozen group sets coincide anyway.

**Recommendation: do not pursue section 2.** The privacy analysis is correct and the OR->AND
reframing is genuinely clever, but the AND is taken over a scope that always contains its own
counterexample. Repair D (blocking by a PU-determined key component) is worth keeping as an idea
in its own right — it is a valid way to shrink any per-query conjunction — but it cannot rescue
this one.

## All-or-Frozen: privacy verified by simulation, and replace-one answered

`attacks/all_or_frozen_privacy.py`. Before endorsing her analysis I checked it the way I check my
own — by neighbour construction rather than by reading. Two privacy bugs were found in *our*
mechanism this session by exactly this step, after the argument had already convinced me.

**Her section 2.8 bound holds.** `D' = D + u*` with `u*` creating `m` singleton groups; the leak is
`Pr[compact branch | D']`, since that is when the new groups are exposed:

| m | Pr[AllPass \| D] | Pr[AllPass \| D'] | measured leak |
|---|---|---|---|
| 1 | 0.999990 | 0.00000000 | 0.00e+00 |
| 2 | 0.999975 | 0.00000000 | 0.00e+00 |
| 4 | 0.999965 | 0.00000000 | 0.00e+00 |
| 8 | 0.999980 | 0.00000000 | 0.00e+00 |

*Resolution caveat:* 200k trials against a charged `δ` = 1e-6 expects 0.2 hits, so observing zero
confirms the bound is not grossly violated but cannot confirm it is tight. The worst case is at
m = 1 as she states, and `ρ_τ^m` decreasing in `m` is visible in the construction.

**The gap her draft does not cover is also fine.** `AllPass` can flip 1→0 without any new group
being created — `u*` merely joining an existing marginal group — which changes the output *domain*
and is therefore observable. That bit is a function of the noisy histogram alone, so it should be
post-processing of an `ε_B`-DP release. Measured on a deliberately marginal group:

- `Pr[AllPass]` = 0.067465 vs 0.072170, ratio **1.0697** against `e^{ε_B}` = 1.6487 → OK
- `Pr[fallback]` = 0.932535 vs 0.927830, ratio **1.0051** → OK

Both well inside the bound, in both directions. **Her accounting is complete.**

**Replace-one adjacency (her stated future work): sound, but strictly worse.** Swapping a PU means
one PU's counts leave while another's arrive, so the histogram sensitivity doubles to `2·C_u`, the
noise scale goes 16.0 → 32.0 and `τ_AF` goes **276.5 → 553.0 (2.00×)**. Every group must clear a
threshold twice as high. The AND argument itself survives — entering the compact branch still
requires every newly created group to pass — so nothing becomes unsound. An already inert rule
simply becomes more inert.

**With this, section 2 is settled.** The analysis is verified (including the branch bit), the
adjacency question is answered, the mechanism is measured across 8 queries and 3 datasets, the
root cause is proven, and three repairs have been explored. Nothing material remains open.

## SECTION 1 (EM selection of C_u): the machinery works, but p = 0.7 is the wrong target

`attacks/em_cu.py`, `attacks/em_cu_vs_manual.py`. An earlier version of this section endorsed the
proposal on the strength of ONE grouping. Widened to nine queries across three datasets it is more
interesting: **the mechanism is sound and nearly free, but her target fraction is badly chosen, and
fixing it is a one-line change.**

**Her `Delta_q <= 1` argument is correct, verified by construction.** Removing a PU with `k_u` = k
shifts `K-k+1` cumulative counts at once, so *releasing* `(F(1)..F(K))` would have l1 sensitivity
up to `K` — but the EM scores each candidate separately:

| removed PU | counts changed | l1 change | **max_r \|dq\|** |
|---|---|---|---|
| `k_u`=1 | 80 of 80 | 80 | **1.0** |
| `k_u`=30 | 51 of 80 | 51 | **1.0** |
| `k_u`=70 | 11 of 80 | 11 | **1.0** |

**The EM itself is essentially deterministic and nearly free.** With ~180k PUs, `F` jumps by
thousands between adjacent candidates, so all 15 draws agree at every budget from 0.010 to 0.250.
Spending more only hurts, since it comes from the query. **~0.01 of eps is enough.**

**But the p-quantile of `k_u` is not the error-optimal bound, and p = 0.7 under-shoots.** The
tolerance band around the optimum is severely asymmetric — halving `C_u` costs 2.6x-35x, doubling
costs 1.7x-2.0x:

| query | best | x1/8 | x1/4 | x1/2 | x1 | x2 | x4 |
|---|---|---|---|---|---|---|---|
| tpch month | 55 | 137.7x | 104.0x | **35.3x** | 1.00x | 1.94x | 2.49x |
| tpch month\|nation | 44 | 7.32x | 5.93x | 3.14x | 1.00x | 1.96x | 3.11x |
| tpch month\|prio | 72 | 9.11x | 6.62x | 2.60x | 1.00x | 1.70x | 1.70x |
| so posts\|month | 44 | 1.44x | 1.32x | 1.13x | 1.00x | 1.28x | 1.67x |

So the selection should sit deliberately *above* the optimum, and p = 0.7 sits below it. Sweeping p:

| query | best | p=0.7 | p=0.9 | p=0.95 | **p=0.99** |
|---|---|---|---|---|---|
| tpch month | 55 | **17.75x** | 2.91x | 2.91x | **1.35x** |
| tpch month\|nation | 44 | 1.18x | 1.02x | 1.00x | 1.21x |
| tpch month\|prio | 55 | 1.41x | 1.03x | 1.00x | 1.17x |
| so posts\|month | 44 | 2.02x | 1.56x | 1.31x | 1.22x |
| so comments\|month | 34 | 1.70x | 1.34x | 1.11x | 1.36x |
| cb hits\|region | 1 | 1.02x | 1.00x | 0.99x | 1.00x |
| **worst case** | | **17.75x** | 2.91x | 2.91x | **1.36x** |

**p = 0.99 cuts the worst case from 17.75x to 1.36x** at a typical cost of ~1.2x. Her reasoning
for erring high was right; the data says go much further than 0.7.

**And this is where the mechanism earns its place: no fixed manual value works across datasets.**
Google DP as published requires the analyst to supply `max_partitions_contributed`; the penalty for
plausible guesses, against each query's own optimum:

| guess | tpch month | tpch month\|nation | so posts\|month | cb hits\|region | **worst** |
|---|---|---|---|---|---|
| `C_u`=1 | 177.6x | 7.25x | 2.00x | 1.00x | **177.6x** |
| `C_u`=10 | 136.3x | 5.56x | 1.32x | 2.39x | **136.3x** |
| `C_u`=100 | 1.57x | 1.69x | 1.09x | 5.18x | **5.18x** |
| `C_u`=max | 1.36x | 1.37x | 1.49x | 4.18x | **4.18x** |
| **EM, p=0.99** | 1.35x | 1.21x | 1.22x | 1.00x | **1.36x** |

`C_u`=1 is catastrophic on TPC-H (177x) yet optimal on ClickBench; `C_u`=100 is fine on TPC-H yet
5.2x on ClickBench. **The best fixed choice is 4.18x worst-case; the EM at p=0.99 is 1.36x — a 3.1x
improvement, and it comes from adapting rather than from being cleverer.**

**Verdict: recommend section 1, with p = 0.99 rather than 0.7.** Sound argument, ~1% of budget,
freezable (the `k_u` quantiles are identical across every filter tested), and it beats any fixed
value an analyst could supply.

## BROAD BENCHMARK — 14 queries, 3 datasets: median 1.88x, range 1.05x-9.53x

`attacks/broad_benchmark.py`. `google-best` gives Google DP as published its **optimal `C_u` for
each query** — an upper bound on any Google configuration, since an analyst cannot beat knowing
the answer in advance. `google-EM` is what an automated Google achieves using Dandan's section 1
at p=0.95.

| query | groups | google-best | google-EM | ours | ours+frozen | vs g-best |
|---|---|---|---|---|---|---|
| tpch SUM price / month | 84 | 0.68% | 0.81% | 0.17% | **0.16%** | 4.26x |
| tpch SUM price / mo\|nation | 2,096 | 14.01% | 15.68% | 3.92% | **2.27%** | 6.17x |
| tpch SUM price / mo\|prio | 420 | 10.23% | 12.10% | 3.12% | **1.77%** | 5.78x |
| tpch SUM price / day | 2,526 | 99.57% | 100.0% | 85.12% | **10.45%** | **9.53x** |
| tpch COUNT / mo\|nation | 2,096 | 14.66% | 17.98% | 4.35% | **2.49%** | 5.90x |
| tpch SUM qty / mo\|nation | 2,096 | 12.39% | 12.34% | 5.32% | **3.15%** | 3.93x |
| tpch SUM price / yr\|nation | 175 | 0.69% | 0.62% | 0.36% | **0.33%** | 2.06x |
| so COUNT posts / month | 190 | 42.35% | 44.48% | 34.89% | **26.67%** | 1.59x |
| so SUM score / month | 190 | 62.97% | 64.66% | 46.80% | **37.11%** | 1.70x |
| so COUNT comments / month | 182 | 55.54% | 100.0% | 46.32% | **35.02%** | 1.59x |
| so COUNT posts / day | 5,107 | 92.70% | 100.0% | 89.63% | **56.21%** | 1.65x |
| cb COUNT hits / region | 3,238 | 6.06% | 6.29% | **5.71%** | 7.29% | 1.06x |
| cb COUNT hits / date\|reg | 7,564 | 10.04% | 9.97% | **8.74%** | 12.36% | 1.15x |
| cb SUM width / region | 3,238 | 6.50% | 6.54% | **6.21%** | 9.55% | 1.05x |

**The `k_u`-concentration condition predicts every row.** Median gain by dataset: TPC-H **5.78x**
(users spread over 30-176 groups), StackOverflow **1.62x** (median `k_u` = 1, long tail),
ClickBench **1.06x** (median `k_u` = 1, short tail). This is the same scope condition established
earlier, now confirmed across measures (SUM and COUNT behave alike) and group counts (84 to 7,564).

**New negative: freezing HURTS on unfiltered queries.** On all three ClickBench rows `ours+frozen`
is worse than `ours` (5.71% -> 7.29%, 8.74% -> 12.36%, 6.21% -> 9.55%). The reason is structural
and obvious in hindsight — those queries have no filter, so the filterless group set *is* the
query's own group set, and freezing buys nothing while still paying the amortised `eps_0/N`. **Rule:
freeze only when the query is filtered relative to the frozen query.** An unfiltered query should
skip the frozen path entirely, which is statically decidable.

**`google-EM` tracks `google-best` on TPC-H but fails on StackOverflow** (55.54% -> 100.0% on
comments/month, 92.70% -> 100.0% on posts/day) — the heavy-tailed `k_u` case where the quantile
chases the tail, exactly as the p-sweep predicted.

## Are the bounds post-processing? No — and a third of the TPC-H gain is bound selection

**The bounds are not free.** `U` (Google's per-cell bound) and `B` (our per-PU norm bound) both
come from ApproxBounds, a DP release costing `eps_b`. That is why a three-way budget split exists
at all. Once released, *using* a bound is post-processing, but obtaining it is not.

So: how much of the measured gain is the clipping geometry, and how much is ApproxBounds simply
serving one arm better than the other? Giving BOTH arms oracle bounds (best `U` / best `B` on a
1.19x-spaced grid, `eps_b` -> 0 so the saved budget goes to the values):

| query | DP bounds | | | oracle bounds | | |
|---|---|---|---|---|---|---|
| | g-best | ours | ratio | g-best | ours | ratio |
| tpch SUM price / mo\|nation | 14.09% | 3.95% | **3.57x** | 8.81% | 3.92% | **2.25x** |
| so COUNT posts / month | 34.48% | 36.53% | 0.94x | 30.92% | 30.45% | 1.02x |
| cb COUNT hits / region | 6.20% | 5.76% | 1.08x | 5.91% | 5.61% | 1.05x |

**On TPC-H the advantage drops 3.57x -> 2.25x under oracle bounds.** Google gains 1.6x from a
perfect bound (14.09% -> 8.81%) while we gain essentially nothing (3.95% -> 3.92%). So the honest
decomposition of the TPC-H gain is:

- **~2.25x from the clipping geometry**, which survives perfect bounds on both sides
- **~1.6x from ApproxBounds serving Google's per-cell `U` worse than our per-PU `B`**

That second factor is real but it is a different claim. It has a cause: ApproxBounds is a
max-finder, and the error-optimal per-cell bound is roughly half the maximum, so it is ~2x
suboptimal for `U` by construction. For our `B` it happens to land at 0.96x of the optimum,
because two offsets cancel. **Ours is less sensitive to the bound-selection mechanism than
Google's is** — which is the same parameter-robustness theme as the `C_u` result, not a
sharper clip.

On StackOverflow and ClickBench the ratio barely moves (0.94 -> 1.02, 1.08 -> 1.05): ties either
way, so nothing there depends on bound quality.

**Which number to report:** the benchmark's 1.88x median is against Google *as published*, which
uses ApproxBounds — that is the real system and the right comparison. But the decomposition
belongs alongside it, because a reviewer with an oracle-bounds baseline will find 2.25x on TPC-H,
not 3.57x.

**Grid caveat, learned the hard way:** a first version of this used a 5-point grid spanning 16x,
i.e. 2x spacing — coarser than the 1.3x error basin — and reported ours getting *worse* under
"oracle" bounds. That was a grid artifact, and it biased the test toward Google, whose effective
grid is `C_v x U` and therefore finer by construction. Any oracle-bound comparison needs spacing
below ~1.2x.

## Dandan's 17 Aug questions: low-support prevalence, and frozen vs per-query C_u

**Q(2): are low-support groups common?** Not on TPC-H — and that is the point. Support = distinct
PUs per group; tau at `C_e`=1, `eps_eta`=0.4 is ~34, so anything under ~34 can never pass:

| query | groups | min | p1 | p5 | median | <10 | <34 | <100 |
|---|---|---|---|---|---|---|---|---|
| tpch price mo\|nation | 2,095 | **1** | 465 | 1,279 | 2,779 | 1% | 1% | 1% |
| tpch price month | 84 | **32** | 10,233 | 34,540 | 69,976 | 0% | 1% | 1% |
| tpch price day | 2,526 | 8 | 118 | 592 | 1,092 | 0% | 0% | 1% |
| tpch price mo\|prio | 420 | **1** | 119 | 1,726 | 4,268 | 1% | 1% | 1% |
| so posts month | 190 | 1 | 2 | 5 | 658 | 11% | 13% | 13% |
| so posts day | 5,107 | 1 | 2 | 9 | 35 | 6% | **47%** | 100% |
| so comments month | 182 | 1 | 1 | 3 | 512 | 9% | 9% | 10% |
| cb hits region | 3,238 | 1 | 1 | 1 | 3 | 65% | **79%** | 87% |
| cb hits date\|region | 7,564 | 1 | 1 | 1 | 3 | 73% | **86%** | 92% |

**Her expectation was right about TPC-H and it still does not save the mechanism.** Only ~1% of
groups are low-support there — but the AND requires *zero*, and 1% of 2,095 groups is ~20 groups.
`tpch month` is the single case with a genuinely healthy minimum (32), and it is exactly the one
query where All-or-Frozen fires. On StackOverflow and ClickBench low support is instead the
*majority* (47-86% below tau). So the correct statement is not "small groups are common" but
**"the AND needs none, and one is enough"**.

**Q(1): frozen vs per-query `C_u`.** Frozen = the EM run once on the unfiltered query (giving
`C_u` = 48); per-query = the EM re-run on each query, paying `eps_N + eps_C` = 0.01 every time:

| query | PUs | `k_u` p50 | p95 | per-q `C_u` | frozen | per-query | best |
|---|---|---|---|---|---|---|---|
| PU-side: acctbal>=8000 | 181,532 | 30 | 48 | 48 | **12.36%** | 12.56% | 12.48% |
| PU-side: acctbal>=9500 | 45,320 | 30 | 48 | 53 | **85.14%** | 87.16% | 82.02% |
| PU-side: acctbal>=9900 | 9,182 | 30 | 48 | 57 | 100.0% | 99.99% | 99.98% |
| GRP-side: ship>=1998 | 821,108 | **4** | **8** | **8** | 2.52% | **1.38%** | 0.68% |
| GRP-side: nation<5 | 199,738 | 30 | 48 | 48 | **2.31%** | 2.40% | 2.48% |

**Her hypothesis is right for one kind of filter and wrong for the other, and the distinction is
statically decidable.** A filter on the *privacy unit* removes users, but the survivors still touch
the same number of groups — so the `k_u` distribution is **identical** (p50=30, p95=48) even at
`acctbal>=9900`, which leaves 9,182 of 1,000,000 customers. Per-query selection returns essentially
the same value and is slightly *worse*, because it pays the EM every time. A filter that removes
*groups* collapses `k_u` (p50 30 -> 4, p95 48 -> 8), and there per-query wins clearly: **2.52% ->
1.38%, 1.8x**.

The refined test is not "does the WHERE touch the grouping key" — `nation<5` does, yet behaves
like a PU-side filter, because nation is *functionally determined by the customer*, so restricting
nations removes whole customers rather than shrinking anyone's `k_u`. The rule is:

> **Re-run the `C_u` selection when the filter constrains a grouping-key component that is NOT
> functionally determined by the privacy unit. Otherwise reuse the frozen value and skip the EM.**

That is the *same* PU-determined-key-component test used for pruning `G_fix` and for blocking the
All-or-Frozen AND — one static analysis serves all three.

## WHERE GOOGLE DP WINS — bimodal `k_u`, and the complete map

Deliberate search for queries where Google DP as published beats the l1 clip. Twelve further
StackOverflow and ClickBench queries (`attacks/hunt_google_wins.py`) turned up exactly one real
loss — `so SUM ViewCount / day`, 95.82% vs 99.99% = 0.96x — where both arms are useless anyway.
The interesting result came from constructing the loss deliberately.

**The construction** (`attacks/where_google_wins.py`): most PUs touch one group, so `C_v`=1 is
nearly lossless for Google, plus a fraction `f` of "whales" spread over `W` of 200 groups:

| whale fraction | `W` | google | ours | ratio | winner |
|---|---|---|---|---|---|
| 0.0002 | 20 | 1.32% | 1.30% | 1.01x | tie |
| 0.0002 | 100 | 2.31% | 2.39% | **0.97x** | **GOOGLE** |
| 0.0010 | 20 | 2.44% | 2.26% | 1.08x | ours |
| 0.0010 | 100 | 8.88% | 8.67% | 1.02x | ours |
| 0.0050 | 20 | 7.68% | 7.87% | **0.98x** | **GOOGLE** |
| 0.0050 | 100 | 27.46% | 32.34% | **0.85x** | **GOOGLE** |
| 0.0200 | 20 | 11.21% | 9.18% | 1.22x | ours |
| 0.0200 | 100 | 24.86% | 31.41% | **0.79x** | **GOOGLE** |

**Google wins by up to 1.27x** (31.41 / 24.86) when a small fraction of users spread very widely
while the bulk touch one group. The mechanism is direct: the whales set our `B`, since we must clip
to the largest per-PU norm and every ordinary user then pays that noise — whereas Google simply
truncates each whale to one cell and pays only `U`. Our clip refuses to discard anyone, and here
that is the wrong instinct.

**This completes the map, and the shape of the `k_u` distribution decides all of it:**

| `k_u` distribution | example | outcome |
|---|---|---|
| concentrated **high** — everyone spreads widely | TPC-H (median 30, max 72) | **ours wins 4x-9x** |
| concentrated **low** — everyone touches one group | ClickBench (median 1, max 54) | **tie**, 1.0x-1.15x |
| **bimodal** — most touch one, a few touch many | constructed; `so ViewCount/day` | **Google wins, up to 1.27x** |

The earlier scope condition ("helps iff `k_u` is concentrated") was right but one-sided: it
identified the win region and treated everything else as "no gain". In fact the residual splits
into a tie region and a genuine loss region, and the loss region is exactly where a norm bound is
the wrong summary — a single whale drags `B` up for everybody, which is the cost of never
discarding anyone.

**Practical consequence:** the same statistic that predicts the win predicts the loss, and it is
cheap and public-ish to estimate. A system could choose between norm clipping and truncation per
query from the `k_u` histogram it already builds for `C_u` selection — bimodality favours
truncation, concentration favours the norm clip.

## FIFTEEN `k_u` DISTRIBUTIONS — and an ApproxBounds bug found on the way

`attacks/ku_families.py`. Same PU count, group count and cell-value distribution throughout; only
the **shape of `k_u`** varies.

| `k_u` family | med | mean | max | harm | google | ours | ratio | winner |
|---|---|---|---|---|---|---|---|---|
| constant(1) | 1 | 1.0 | 1 | 1.0 | 1.32% | 1.30% | 1.02x | tie |
| **constant(5)** | 5 | 5.0 | 5 | 5.0 | 1.46% | 1.84% | **0.79x** | **GOOGLE** |
| constant(20) | 20 | 20.0 | 20 | 20.0 | 1.69% | 1.10% | 1.54x | ours |
| constant(60) | 60 | 60.0 | 60 | 60.0 | 1.77% | 1.09% | 1.63x | ours |
| uniform(1,10) | 5 | 5.5 | 10 | 3.4 | 3.74% | 1.52% | **2.45x** | ours |
| uniform(1,60) | 30 | 30.5 | 60 | 12.6 | 3.20% | 2.36% | 1.36x | ours |
| uniform(1,200) | 100 | 100.4 | 200 | 33.0 | 3.21% | 2.77% | 1.16x | ours |
| zipf(1.3) | 6 | 49.5 | 200 | 2.7 | 6.52% | 5.04% | 1.30x | ours |
| **zipf(2.0)** | 1 | 4.2 | 200 | 1.4 | 23.03% | 32.57% | **0.71x** | **GOOGLE** |
| zipf(3.0) | 1 | 1.4 | 200 | 1.1 | 7.22% | 6.64% | 1.09x | ours |
| lognormal(0.6) | 12 | 14.1 | 127 | 9.4 | 5.16% | 5.20% | 0.99x | tie |
| lognormal(1.4) | 12 | 27.8 | 200 | 4.8 | 11.80% | 9.45% | 1.25x | ours |
| **bimodal(0.005,100)** | 1 | 1.5 | 100 | 1.0 | 30.06% | 33.79% | **0.89x** | **GOOGLE** |
| **bimodal(0.02,100)** | 1 | 3.0 | 100 | 1.0 | 33.50% | 49.06% | **0.68x** | **GOOGLE** |
| **bimodal(0.05,60)** | 1 | 3.9 | 60 | 1.1 | 16.72% | 18.35% | **0.91x** | **GOOGLE** |

**Google wins 5 of 15, we win 8, 2 ties.** Two things stand out and neither was predicted by the
earlier "concentrated `k_u`" story:

**1. There is a LEVEL threshold, not just a shape condition.** `constant(5)` is *perfectly*
concentrated — max/median = 1 — and Google still wins 0.79x, while `constant(20)` and
`constant(60)` lose 1.5-1.6x. Sweeping constant `k_u` finds the crossover near **7**: at k_u = 1, 3,
5 Google wins (0.94x, 0.81x, 0.68x) and from k_u = 10 upward we win (1.70x, 1.53x, 1.63x). So the
l1 clip needs privacy units to touch **roughly ten or more groups** before it pays, even when every
unit is identical. Below that, Google's ability to trade truncation for a smaller bound is simply
a knob we do not have.

**2. No simple statistic predicts the winner.** Median, max/median and harmonic mean all overlap
across the win and loss sets — `zipf(2.0)` (median 1, max 200) loses while `zipf(3.0)` (median 1,
max 200) wins, differing only in how *many* PUs sit in the tail. The honest summary is two rules,
not one formula: **`k_u` above ~10 for most units favours the norm clip; a bimodal shape with a
non-trivial whale fraction favours truncation.**

### An ApproxBounds bug in the harness, found by this sweep

`constant(5)` initially reported **B = 2** — the fallback value — because 20,000 PUs with norms all
near 500 split across two log2 bins straddling the boundary, and *neither* half cleared the
count threshold. My `approx_bounds` lacked **Wilson et al.'s relaxation loop**, which retries with
a laxer threshold (multiplying the failure probability by 10 while it stays under 1e-6) precisely
for this case. Without it, a tightly concentrated distribution can silently destroy its own bound.

Fixed: the loop is now implemented, and the last-resort fallback returns the top occupied bin
rather than `2^1`. Effect on this sweep was modest (constant(5) 0.67x -> 0.79x, zipf(1.3) 1.10x ->
1.30x) and the qualitative picture is unchanged. Real-data results are unaffected — TPC-H,
StackOverflow and ClickBench all have norm distributions spread over many bins, so no bin-count
threshold was ever missed there. **But any future synthetic or pre-binned data could have hit it.**

## eps SWEEP, AND A CORRECTION TO THE ApproxBounds "UNAFFECTED" CLAIM

**The benchmark is not an artifact of eps = 1.** An earlier finding (gain 3.31x at eps=0.25 vs
1.26x at eps=1 on one query) suggested the whole table might be a slice through a resonance. It is
not — the *ranking* is stable and only the magnitude moves:

| query | eps=0.25 | eps=0.5 | eps=1 | eps=2 | eps=4 |
|---|---|---|---|---|---|
| tpch SUM price / mo\|nation | 1.45x | 2.28x | 3.50x | 2.39x | 3.76x |
| tpch SUM price / mo\|prio | 1.20x | 2.72x | 2.88x | 2.92x | 3.08x |
| tpch COUNT / mo\|nation | 1.21x | 2.11x | 3.27x | 4.20x | 4.58x |
| so COUNT posts / month | 0.87x | 0.95x | 0.90x | 0.93x | 0.88x |
| cb COUNT hits / region | 1.06x | 1.04x | 1.06x | 1.02x | 1.10x |
| **median** | **1.20x** | 2.11x | 2.88x | 2.39x | **3.08x** |

TPC-H wins at every eps, ClickBench ties at every eps. The gain is *smallest at small eps*
(median 1.20x at 0.25) and grows to 3.08x at eps=4 — the opposite of a peak. Worth stating,
since small eps is the interesting regime for privacy.

**Correction: I claimed the ApproxBounds relaxation fix left real-data results unaffected. It did
not.** The fix materially changed StackOverflow and ClickBench, in *Google's* favour, because
those queries' norm histograms sometimes had no bin clear the threshold. Re-running the whole
benchmark with the corrected routine:

| | before fix | after fix |
|---|---|---|
| median vs google-best | 1.88x | **1.90x** |
| range | 1.05x-9.53x | **1.02x-9.56x** |
| so COUNT posts / month | 1.59x | **1.40x** |

The headline survives, but the claim of "unaffected" was wrong and the individual SO rows moved by
up to 0.2x. Any number in this document produced before that commit should be treated as
approximate on SO/ClickBench.

**And a second correction, to my own diagnostic.** An isolated re-test appeared to show
`so COUNT posts / month` flipping to a 0.91x *loss*. That test accidentally gave Google
**decoupled** `C_e`/`C_v`, which is not published. Under the published coupled baseline the row is
1.40x in our favour; under a decoupled Google it is 0.91x. Both are true of different baselines,
and it is a reminder that the decoupling — invented in this project's own review — is worth about
1.5x to Google on its own.

## ATTACKING THE BENCHMARK: the metric survives, the strong-Google attack lands

`attacks/attack_benchmark.py`. Two attacks on the median-1.90x headline.

**Attack 1 — the metric — FAILS, and per-group metrics are kinder to us in scope.** Every number
in this document is relative total L1, a metric shown earlier to report 8.6% for a release
answering 7% of a key set. Re-tuning each arm separately under four metrics:

| query | total-L1 | mean-rel | median-rel | nRMSE |
|---|---|---|---|---|
| tpch SUM price / mo\|nation | 3.61x | 2.44x | **5.17x** | 2.93x |
| tpch SUM price / mo\|prio | 3.25x | 2.31x | **4.18x** | 2.97x |
| tpch COUNT / mo\|nation | 3.48x | 2.36x | **5.41x** | 2.79x |
| **median, all 8 queries** | **1.23x** | 1.19x | 1.22x | **1.29x** |

All four agree in direction, and median-rel is the *most* favourable on TPC-H — the l1 clip helps
typical groups more than it helps the total. The metric worry is closed.

**Attack 2 — a maximally strong Google — LANDS.** Giving Google every DP-legal improvement found
in this project (top-`C_v` selection, rescale-with-re-clip, decoupled `C_e`/`C_v` — none of them in
Wilson et al. or the library):

| query | vs published | **vs strong** |
|---|---|---|
| tpch SUM price / mo\|nation | 3.61x | **1.38x** |
| tpch SUM price / mo\|prio | 3.25x | **1.30x** |
| tpch COUNT / mo\|nation | 3.48x | **2.33x** |
| tpch SUM price / day | 1.16x | 0.99x |
| so COUNT posts / month | 1.12x | **0.74x** |
| so SUM score / month | 1.28x | **0.80x** |
| cb COUNT hits / region | 1.04x | 0.98x |
| cb COUNT hits / date\|reg | 1.18x | 1.02x |
| **median** | **1.23x** | **1.01x** |

**Against the strongest Google-shaped mechanism the l1 clip is a wash overall** — and it *loses* on
StackOverflow (0.74x, 0.80x), which is the out-of-scope regime the `k_u` condition already flags.

### Two corrections this forces

**1. The 1.90x headline was mostly the frozen key set, not the clip.** This attack has no frozen
arm, and the clip alone measures **1.23x** median against published Google. Recomputing from the
broad benchmark's `ours` column confirms it (~1.28x). Since the frozen key set is prior art
(Tumult `get_groups` + `KeySet`, Privacy on Beam `SelectPartitions`), **the novel component is worth
~1.23x, and the larger number comes from something already shipping.**

**2. A median across queries that violate the scope condition is misleading in both directions.**
Reporting by regime is honest:

| regime | vs published | vs strong |
|---|---|---|
| **in scope** (`k_u` concentrated, >~10): TPC-H mo\|nation, mo\|prio, COUNT | **3.25x-3.61x** (5.4x on median-rel) | **1.30x-2.33x** |
| **out of scope** (`k_u`=1 or bimodal): SO, ClickBench, tpch day | 1.04x-1.28x | **0.74x-1.02x** |

That is the defensible claim: **in its stated scope the l1 clip is worth 3.3x-3.6x against Google
DP as published and 1.3x-2.3x against the best Google-shaped mechanism I could build; outside that
scope it is a wash or a small loss.** The scope condition is not a caveat bolted on afterwards —
it is measurable in advance from the `k_u` histogram.

## AN ACTUAL ADVERSARY — 4M trials against the full stack

`attacks/mia_full_stack.py`. Everything before this verified *sensitivity* by neighbour
construction. This runs an attacker end to end: D and D' differ in one target PU, the attacker sees
one release and guesses which world it came from. Advantage = `Pr[say D'|D'] - Pr[say D'|D]`, which
for an eps-DP release cannot exceed `(e^eps-1)/(e^eps+1)`.

**A1 — value channel, worst-case target** (concentrates its entire budget in one group, then the
attacker thresholds that group's released value):

| B | attacker advantage | bound at `eps_v`=0.6 |
|---|---|---|
| 512 | 0.2587 | 0.2913 |
| 4,096 | 0.2597 | 0.2913 |
| 65,536 | 0.2585 | 0.2913 |

Under the bound, and **scale-free**: identical advantage across a 128x range of B, because the
target's contribution and the noise both scale with B. That is the correct signature — the l1 clip
converts an unbounded contribution into one whose leakage depends on `eps_v` alone.

**A2 — key set with the vote-support gate.** Target creates a group only it occupies; attacker
says D' iff that group is released. Leak 2.5e-07 to 1.0e-06 against a charged `delta` = 1e-06, at
every `C_e` tested. Within budget.

**A3 — key set WITHOUT the gate: the adversary reproduces the delta bug empirically.**

| `C_e` | `k_u` | leak, no gate | leak, gated | charged | over by |
|---|---|---|---|---|---|
| 1 | 30 | 2.10e-05 | 1.00e-06 | 1e-06 | **21.0x** |
| 1 | 72 | 4.70e-05 | 1.25e-06 | 1e-06 | **47.0x** |
| 2 | 72 | 2.88e-05 | 1.25e-06 | 1e-06 | 28.8x |
| 5 | 72 | 1.27e-05 | 1.75e-06 | 1e-06 | 12.8x |

The excess **grows with `k_u` and shrinks with `C_e`**, exactly as the union-bound analysis
predicted. Mechanism: the target holds values in `k_u` groups but votes in only `C_e` of them, so
the `k_u - C_e` unvoted groups each get an independent chance to clear tau on noise alone — chances
tau's union bound never covered. **This is the first demonstration of that bug by an attacker
rather than by inference from the accounting, and it confirms the gate is mandatory, not
cosmetic.**

**What this does not cover:** a single-target membership test under one release. Not tested —
repeated releases against the same frozen state (composition over N queries), an attacker who
knows the other PUs' data exactly, reconstruction rather than membership, or an adversary
targeting `G_fix` itself across refreshes.

## ATTACKS ON THE FROZEN-STATE DESIGN — all four pass, and one near-miss was MC noise

`attacks/mia_frozen_repeated.py`. Generic DP analysis does not cover what the persistent table
adds: reuse, refreshes, and an attacker who knows everything but the target.

**B1 — repeated releases sharing one frozen `G_fix`. Reuse itself leaks nothing.**

| queries | composed `eps_v` | attacker advantage | bound |
|---|---|---|---|
| 1 | 0.6 | 0.2585 | 0.2913 |
| 2 | 1.2 | 0.2872 | 0.5370 |
| 5 | 3.0 | 0.3890 | 0.9051 |
| 10 | 6.0 | 0.5116 | 0.9951 |

Advantage tracks the **composed `eps_v` of the value releases only**. Reusing one `G_fix` across
all of them adds nothing, which is exactly the property the design needs and the reason the
amortisation is sound.

**B2 — omniscient adversary (knows every other PU exactly, subtracts the known part).** Advantage
0.2589 at a target contributing `B`, and **0.2590 at a target contributing 4B** — because the clip
caps it at `B` regardless. Omniscience buys nothing beyond the `eps_v` bound. This is the clearest
demonstration of what the l1 clip is actually for.

**B3 — reconstruction rather than membership.** Posterior sd of the target's value given the
release is 1,167 against a prior sd of 1,183 — a **1.01x** narrowing. The attacker learns
something (that is what `eps` buys) but the value is not recovered.

**B4 — `G_fix` refreshes compose linearly, and must be budgeted.** Re-releasing the frozen set `R`
times gives the attacker `R` independent chances at a target's singleton group. Exactly:

```
Pr[exposed] = 1 - (1-p)^R  <=  R*p  <=  R*delta_0
```

so linear composition is a valid bound — confirmed exactly at R = 1..50, ratio 1.000 throughout.
**A frozen set refreshed on a schedule must budget `R*delta_0`, not `delta_0`.** That is the
concrete accounting answer to Dandan's update/batching question: batching does not change the cost
of any single refresh, it reduces `R`.

**A near-miss worth recording as method.** The simulation first reported R=10 leaking 1.40e-05
against a budgeted 1.0e-05 — an apparent violation. It was Monte Carlo noise: six independent runs
of 500k trials gave 4.0e-06 to 1.4e-05 around an expected 1.0e-05, because only ~5 hits occur per
run. **At `delta` = 1e-06 a simulation needs ~1e8 trials to resolve a 1.4x effect**, so any
delta-scale claim in this document should be checked exactly rather than by MC.

## MULTI-AGGREGATE QUERIES, AND AN ADAPTIVE ATTACKER

`attacks/multi_agg_adaptive.py`, `attacks/adaptive_target_selection.py`.

**C1 — the advantage shrinks with more aggregates, and flattens near 2x.** Every headline in this
document is a *single* aggregate; real queries are not. With `c` aggregates, `eps_v` splits `c`
ways while tau is paid once:

| c | google | ours | ratio | aggregates |
|---|---|---|---|---|
| 1 | 13.52% | 3.89% | **3.48x** | SUM price |
| 2 | 18.93% | 8.02% | 2.36x | + SUM qty |
| 3 | 22.86% | 11.40% | 2.01x | + COUNT |
| 4 | 26.25% | 13.40% | **1.96x** | + SUM discount |

The decay is structural, not a defect: tau is a fixed cost paid once regardless of `c`, so as `c`
grows the shared vote channel shrinks as a fraction of the total and both arms converge toward
pure value-channel noise, where the gap is just the sensitivity ratio. **Report single-aggregate
numbers as an upper end of the range, not as typical.**

**C2 — an adaptive attacker cannot beat composition, tested properly the second time.**

My first attempt was weak and is worth recording as a lesson: it had the attacker sum `k` noisy
observations of the *same* quantity, which is a fixed strategy wearing an adaptive label. It
confirmed composition arithmetic (advantage 0.39 at k=1 falling to 0.10 at k=8) and nothing about
adaptivity.

The real test lets round 1 **choose the target** for round 2: `M` candidate PUs, one present; spend
`eps/2` probing all of them, pick the most suspicious, spend `eps/2` on that one alone. Against a
blind attacker spending the whole `eps` on one fixed target:

| M | adaptive advantage | blind advantage | gain | bound at `eps`=1 |
|---|---|---|---|---|
| 2 | 0.1402 | 0.3929 | **0.36x** | 0.4621 |
| 5 | 0.0672 | 0.3931 | 0.17x | 0.4621 |
| 20 | 0.0165 | 0.3947 | 0.04x | 0.4621 |
| 100 | 0.0057 | 0.3931 | **0.01x** | 0.4621 |

Under the bound everywhere, and **adaptivity actively loses** — badly, and worse as `M` grows —
because the selection round costs budget and its pick is usually wrong. That is the expected
behaviour of a correctly composed mechanism, but it had been assumed rather than measured.

## SQL JOINS AND PU ARRIVALS — both clean

`attacks/joins_and_arrivals.py`. Two distinct things that the word "join" covers.

**D1 — SQL join fan-out is invisible to the bound.** Every experiment in this document queries
`lineitem JOIN orders JOIN customer` with the privacy unit on the far side, so one customer owns
many rows — the classic place sensitivity analysis breaks. Measured: **182 rows per customer at the
max**, across up to 44 distinct orders. Running the *full pipeline on both sides* for the 40
fattest customers:

```
max ||released(D) - released(D\u)||_1  =  4,194,304.000  =  1.000000 x B
```

Exact, not approximate. The fan-out cannot matter because the clip is on the PU's **total**, not on
rows or cells — which is precisely the property row-level DP lacks. (An earlier note in this file
said "4,000 rows"; the measured figure is 182.)

**D2 — a PU arriving between releases is protected by composition.** Target absent at release 1,
present at release 2, both against the same frozen `G_fix`; the attacker differences the two, so
the target's mass appears against two independent noise draws:

| releases | attacker advantage | bound at composed `eps_v` |
|---|---|---|
| 1 | 0.2591 | 0.2913 |
| 2 | 0.2856 | 0.5370 |
| 3 | 0.3239 | 0.7163 |

Differencing buys nothing beyond the composed `eps`.

**The caveat is where Dandan's update question actually lands.** This assumes `G_fix` is *not*
refreshed when the new PU arrives. If an arriving PU creates a group and the frozen set is then
re-released, that is the `R*delta_0` regime measured earlier — the arrival's *values* are protected
by `eps_v`, but the *key set* refresh is a separate charge. Hence: **batching arrivals does not
reduce the cost of any single refresh, it reduces how many refreshes are needed**, which is exactly
why a minimum batch size helps. A monitor that refreshes only when the key set has genuinely
drifted, rather than on a schedule, is where a sparse-vector construction would pay.

## DOES THE C_u SELECTION TRANSFER TO SMOOTH SENSITIVITY? The rule inverts.

`attacks/em_cu_sass.py`, `attacks/em_cu_sass_eps.py`, reusing the validated NRS envelope from
`attacks/sass_vote_geometry.py`.

The Laplace-path recommendation is **overshoot**: undershooting `C_u` discards user data and costs
up to 35x, overshooting merely adds noise and costs at most 2x, so target `p` = 0.95-0.99. That rule
depends entirely on the shape of the error curve, and in SASS the shape is different, because `C_u`
does two things at once:

- rank-caps each PU to `C_u` partials, so small `C_u` biases every group by `1 - C_u/k_u`
- divides the per-aggregate cell budget, `eps_cell = eps/((c+1)·C_u)`, and `eps_cell` sets
  `beta = eps_cell / (2 ln(2/delta_cell))` in the NRS envelope
  `S* = max_{i<=p<=j}(x_j - x_i) e^{-beta(j-i-1)}`

Measured on `month|nation`, sf10, eps=1, c=1:

| `C_u` | `eps_cell` | `beta` | rank-cap bias | median `S*` | total error |
|---|---|---|---|---|---|
| 1 | 0.5000 | 1.6e-02 | 90.3% | 3.20e8 | **666%** |
| 5 | 0.1000 | 3.3e-03 | 64.2% | 7.43e8 | 7,544% |
| 21 | 0.0238 | 7.8e-04 | 15.0% | 8.73e8 | 37,173% |
| 72 | 0.0069 | 2.3e-04 | 0.0% | 9.04e8 | **133,198%** |

**Error rises monotonically in `C_u`, by 200x across the grid.** The bias falls exactly as intended
(90.3% -> 0.0%) but the noise grows so much faster that reducing bias never pays. **The optimum sits
at `C_u` = 1 — the extreme the Laplace path punishes hardest.** So the overshoot rule does not merely
weaken here, it *inverts*.

**The diagnostic is `beta·64`.** The NRS envelope only decays across the 64 lanes once
`beta·(j-i-1)` reaches order 1; below that `S*` saturates on its domain sentinel and the noise is
set by `Lambda` rather than by the data. Sweeping eps confirms this is the whole story:

| eps | best `C_u` | `beta·64` at `C_u`=1 | best error |
|---|---|---|---|
| 1 | 1 | 1.05 | 665% |
| 8 | 1 | 8.42 | 92.0% |
| 64 | **5** | 67.4 | 65.6% |

Only at eps = 64 — sixty-four times a normal budget — does the optimum move off 1, and even then the
release is 65.6% error. **`eps_cell` must satisfy `beta·64 >~ 1`, i.e. `eps >~ (c+1)·C_u/6`, before
`C_u` > 1 is affordable at all.**

**Conclusion for the paper.** The selection *mechanism* transfers unchanged — it is
`(eps_N + eps_C, 0)`-DP and composes as another line item, and the `k_u` statistic means the same
thing. What does not transfer is the **calibration**: `p` = 0.95-0.99 would pick `C_u` = 48 on this
query, which is 3-4 orders of magnitude worse than `C_u` = 1. In SASS the correct target is the
*bottom* of the distribution, not the top.

**And that makes automatic selection close to pointless there**, because the answer is `C_u` = 1
almost regardless of the data — which needs no mechanism and no budget. The honest statement is that
**this is not a calibration problem to be fixed but a symptom: the SASS value channel cannot afford
any cross-group contribution at realistic eps.** That has to be repaired before automatic bound
selection is worth anything on that path, and it is the same pincer found earlier when the
Gaussian-votes port lost end-to-end.

## PRIOR ART FOR C_u SELECTION, CORRECTED BY READING THE PAPERS MYSELF

An agent search concluded the EM selection of `C_u` was fully pre-empted, with verbatim quotes. On
re-reading the two closest papers directly, **the decisive claim is wrong**: those papers select a
*different parameter*. The distinction is the standard DP norm taxonomy (Wilson et al.):

| parameter | meaning | Google's name |
|---|---|---|
| **l0** | how many distinct GROUPS one user may touch | `max_partitions_contributed` = our `C_u` |
| **l-inf** | how much one user may contribute WITHIN one group | `max_contributions_per_partition` |
| **l1** | one user's total across all groups | (our `B`, in the clip work) |

**"Click Without Compromise" (arXiv 2406.02463) selects l-inf, not l0.** Their Theorem 4.2 gives
`Delta_2(f) = sqrt(r_1^2/sigma_1^2 + ... + r_n^2/sigma_n^2)` — a sum over **all n days with no
truncation of how many days a user appears in**. Their Definition 4.1 is "Bounded **Per-day**
Contributions", and the text says *"a natural way to reduce sensitivity is to limit the number of
contributions a user can make **daily**"*. So `r_i` is a per-group magnitude bound; their mechanism
has **no l0 bound at all**. They do use the identical EM quantile with the identical utility function
and the same Smith-2011/Gillenwater lineage — but pointed at a different knob.

**Liu et al. (ICML 2023, arXiv 2206.03008) do bound l0, but incidentally and only where it
coincides with l1.** Their `rand-clip(N, C)` samples `min{||N||_1, C}` items without replacement, so
one parameter bounds every norm at once — hence *"each ||h_i||_r <= C for r = 0, 1, 2, inf"*. Three
qualifications: it is needed only for their **unbounded-domain** sparse-vector algorithm (for bounded
domains they *"do not require bounded l0 norms"*); the setting is histogram/COUNT estimation, where a
user contributing 1 per group has l0 = l1 so the two parameters are the same object; and their
selection score is expected release error with **sensitivity 5C_m/2**, not a rank-distance quantile
with sensitivity 1.

**The other two hits are also volume bounds, not span bounds.** Amin et al. (ICML 2019) prove the
optimal cap is a quantile of the per-user contribution distribution and must be computed privately
— but their `x_i` is records per user, and they leave instantiation as an open question. Google's
DP-SQLP (VLDB 2024) recommends *"the 99th percentile of per-user records… chosen in a DP way"* —
records again.

**So the corrected picture:** every prior work privately selects a bound on per-user *volume*
(records, contributions, l1). Nobody selects the *cross-group span* l0 as its own parameter against
its own quantile, except Liu et al. incidentally in a COUNT setting where l0 = l1. For SUM queries
the two are unrelated — a user can have l0 = 3 and l1 = 10^6 — so the distinction is not cosmetic.

**What is still not ours, and must not be claimed:** the EM quantile itself (Smith 2011, textbook,
shipped in SmartNoise and diffprivlib); the `Delta_q <= 1` observation (Gillenwater's Lemma 7, and
the defining property of the exponential mechanism); and the general principle that a contribution
bound should be a privately-estimated quantile (Amin et al., explicitly).

**Methodological note, and it is the point of this section.** The agent had correct verbatim quotes
and still reached the wrong verdict, because *"contribution bound"* names different parameters in
different papers and the quotes do not disambiguate it. Only the sensitivity expressions do —
whether the bound appears once per group in a sum, or as a cap on the number of groups. **A
prior-art verdict on a parameter must be checked against the sensitivity formula, not the prose.**

## ALL-OR-FROZEN: DANDAN'S QUESTIONS FOUND TWO BUGS IN MY EXPERIMENT

`attacks/all_or_frozen_v2.py`. Her 18 Aug questions were right on both counts.

**Her Q4 is correct: I had the accounting wrong.** My "repair A" scored a dedicated distinct-PU
count with noise calibrated to `C_e` = 1, but **never truncated any PU to `C_e` groups**. Without
truncation one PU affects up to `k_u` group counts, so the sensitivity is `k_u`, not 1 — exactly her
point. Corrected, each PU votes in at most `C_e` of its groups:

| query | raw count (median) | truncated, `C_e`=1 | tau | AllPass |
|---|---|---|---|---|
| tpch month | 69,976 | **2,273** | 33.8 | 0.0% |
| tpch month\|nation | 2,779 | **90** | 33.8 | 0.0% |
| tpch month\|prio | 17,100 | **454** | 33.8 | 0.0% |

Truncation divides the count by roughly `k_u/C_e` — a 30x factor my earlier numbers were missing.
Raising `C_e` does not rescue it, because the threshold rises with it: at `C_e` = 37 the count is
nearly untruncated (65,546 of 69,976) but tau is 1,548.8. AllPass is 0.0% at every `C_e` tested.

**A second bug, found while checking her Q1.** The earlier profile computed support with
`FROM cells c JOIN bins b ON b.g = c.g GROUP BY c.g` — a fan-out join, since `cells` has one row
per (PU, group) and `bins` one per (group, bin). `count(*)` therefore returned
`n_g x occupied_bins`, **inflating every support figure by ~11x**, and the "min support = 1" veto
analysis derived from it was wrong. Corrected medians: 69,976 / 2,779 / 17,100.

**Her Q1 answered: it is not the budget, it is two compounding multipliers.**

| | factor |
|---|---|
| (a) sensitivity `C_u` = 37 on the reused histogram vs `C_e` = 1 on a dedicated count | **37.8x** on tau |
| (b) the statistic is a max BIN count, not the group's support (one PU lands in one bin) | **~3.1x** (bin/support = 0.32) |

`eps_B` = 0.5 here is generous — the tuned optimum for bound selection elsewhere in this document
is 0.002 — so starving the budget is not the explanation. The reused histogram pays `C_u` for a
statistic that is a third of the quantity it is being compared against.

**Her Q2 is a fair correction to my framing.** I wrote that the mechanism "can only be used when it
is not needed". Her point is that All-or-Frozen targets *output compactness*, not extra coverage:
falling back to `G_fix` when a query contains low-support groups is intended behaviour, not
failure. That is right, and my phrasing overstated the case. The measured objection is narrower and
still stands: the AND fires ~0% of the time, so the compact branch is almost never taken and the
compactness benefit is almost never realised.

**Her Q3 tested: sound, but inert on these queries.**

| query | \|G_Q\| | \|G_fix\| | intersection | new | min count in intersection | AND over G_Q | AND over intersection |
|---|---|---|---|---|---|---|---|
| tpch month | 84 | 84 | 84 | 0 | 1 | 0.0% | 0.0% |
| tpch month\|nation | 2,095 | 2,085 | 2,084 | 11 | **0** | 0.0% | 0.0% |
| tpch month\|prio | 420 | 420 | 420 | 0 | 0 | 0.0% | 0.0% |

On PU-side filters `G_Q` is essentially a subset of `G_fix` (0--11 new groups), so restricting the
AND to the intersection removes almost nothing. And the binding constraint is *inside* the
intersection: its smallest truncated count is 0 or 1 against a threshold of 33.8 — a frozen group
that survives filtering with no voting PU. The proposal would matter for a query whose filter
*creates* groups outside `G_fix`, which none of these do.

## PRIOR ART: GOOGLE PUBLISHED PRIVATE l0 SELECTION IN OCTOBER 2025

`arXiv:2510.21684` (Cheu et al., Google), Section 5, verified by reading the PDF:

> "Another approach is to apply a quantile-finding algorithm (Durfee, 2023). For example,
> **max_groups_contributed can be set to, say, the 83rd percentile of the number of groups that a
> DP unit contributes to.**"

That is private selection of the l0 cross-group span bound, as a DP quantile of the per-unit
distinct-group-count distribution, in a `GROUP BY` SQL surface syntax. **The mechanism claim is
dead.** Their motivation is also ours nearly verbatim: *"Previous iterations of federated analytics
placed the responsibility of computing such queries on the data analyst. This a point of friction
for on-boarding."*

Also found: **PipelineDP already ships `PrivateL0Calculator`** (verified from source via the GitHub
API) — *"Calculates differentially-private l0 bound (i.e. max_partitions_contributed)"* — applying
an exponential mechanism over candidate bounds with a bias/variance score
`-0.5*P*sigma - 0.5*sum_u max(min(k_u,B) - k, 0)`, restricted to COUNT and PRIVACY_ID_COUNT.

**What survives, and it is calibration rather than mechanism:**

- They fix **k = 83** for *sample-complexity* reasons, not utility. Our sweep says the utility
  optimum is **p = 0.95--0.99**, that p = 0.7 costs 17.75x worst-case, and that the right p depends
  on whether `C_u` is shared with tau. That is an empirical calibration of a constant they set by a
  different criterion.
- They buy privacy by **splitting the population** (a disjoint Bernoulli sample, so "no DP unit is
  ingested by both autotuning and aggregation"); we pay by composition, at ~1% of eps.
- Their setting is one flat `GROUP BY` over pre-aggregated device uploads; ours is a relational
  engine with joins, where the per-PU group count is a derived quantity.
- The freeze-versus-re-select rule, and the finding that the whole approach **inverts** under smooth
  sensitivity, are not addressed anywhere.

**Also corrected: Smith 2011 does not contain the utility function.** Verified from the author's
PDF: he attributes the method to McSherry & Talwar (*"an exponential-mechanism-based method due to
McSherry and Talwar [MT07]"*) and presents `PrivateQuantile` (Algorithm 2) *"for completeness"* as a
known construction, writing the output density directly with no named utility and no sensitivity
lemma. The `u(X,o)` form and `Delta_u = 1` were formalised later by Gillenwater et al. (Lemma 7).

## JOINT QUANTILES (Dandan, 19 Aug): a real saving, but not on this statistic

`attacks/joint_quantiles.py`. Gillenwater et al. release `m` quantiles jointly at l1 sensitivity 2
rather than splitting eps `m` ways at sensitivity 1 — a factor of `m/2` in effective budget per
quantile. Her suggestion is to use that for multiple candidate contribution bounds. Measured on
real TPC-H `k_u`, the minimum eps **per quantile** for the EM to return the exact answer:

| candidate range `K` | p50 | p95 | p99 |
|---|---|---|---|
| 72 (the true max) | 0.0005 | 0.002 | 0.005 |
| 256 | 0.0005 | 0.002 | 0.01 |
| 4096 | 0.0005 | 0.005 | 0.01 |

**Everything is recoverable at 0.005--0.01 per quantile, so at our 0.01 total budget a naive
`eps/m` split already lands on the exact answer and the joint machinery changes nothing here.** It
would matter for a flatter statistic — a value bound, where adjacent candidates score almost
equally — or at an order of magnitude less budget.

**A correction to my own first pass.** I initially reported that the candidate range `K` mattered by
100x. That came from a synthetic `uniform(1,60)` where 140 candidates sit above the data's maximum,
all scoring the same mediocre value; at small eps their *combined* mass (0.50) beats the sharp peak
(0.49) and the draw drifts high. Real `k_u` has a tail, so no such block of equally-useless
candidates exists and the true effect is about **2x**, as above. The synthetic case is still worth
knowing — it is a genuine failure mode of the EM when the candidate set is much wider than the
data's support — but it is not what happens here.

**A second artifact worth recording.** On a point-mass distribution (`constant(20)`, every unit at
exactly 20) the EM returns a uniform random candidate. `F` jumps from 0 to `N` in one step, so no
candidate sits at the target rank and *every* candidate scores `-N/2` identically. Not a code bug
and not reachable on real data, but it shows the score is only informative where the CDF passes
near the target.

**What I could not do: turn the curve into a mechanism-selection rule.** The `k_u` shape decides
which mechanism wins (worth 0.68x--6x), so a privately released quantile curve ought to let the
system choose. A two-quantile rule ("ours if p50 >= 10; truncation if p99 >= 10*p50") gets only
7 of 13 families right, and the failures are not noise — `constant(5)` (p50 = p99 = 5) and
`zipf(1.3)` (p50 = 6, p99 = 200) are misclassified from the true quantiles, before any privacy is
applied. **Two quantiles are not a sufficient statistic for the decision.** That is an argument for
releasing more of the curve, which is Dandan's point, but I do not have a rule that works and
should not claim one.

## RETRACTION: my criticism of the histogram reuse was wrong

`attacks/verify.py`, `verify2.py`, `verify3.py`. Checking every All-or-Frozen claim against
Dandan's Q1 -- she recalled that histogram thresholding was *not* much worse than Google's, and she
was right.

**1. My tau_AF implementation matches her closed form exactly** (ratio 1.0000 at four settings),
so the threshold arithmetic was never in question.

**2. At MATCHED `C_u`, her threshold is BELOW Google's -- exactly as she remembered.** Both
mechanisms have the same noise scale `C_u/eps`, because a PU touches at most `C_u` groups either
way. She takes a max over `B`=64 bins but needs only `rho <= delta`; Google reads one count but must
divide `delta` across `C_u` groups. The AND saving outweighs the max-over-bins cost:

| `C_u` | her tau_AF | her tau_PG | Google tau | AF/Google |
|---|---|---|---|---|
| 37 | 1,598.5 | 1,932.5 | 1,548.8 | 1.03 |
| 72 | 3,110.6 | 3,880.4 | 3,132.8 | **0.99** |
| 5 | 216.0 | 236.1 | 185.1 | 1.17 |
| 1 | 43.2 | 43.2 | 33.8 | 1.28 |

**RETRACTED: the "736x" figure.** I told her that reusing the bounding histogram costs 736x on the
threshold versus a dedicated count. That compared her histogram at `C_u`=72 against a count at
`C_e`=1 -- **two changes at once**, sensitivity and statistic. Like-for-like the ratio is 0.99--1.28x.
The slogan I attached to it ("a free statistic is not free when it is the wrong statistic") was
therefore unsupported, and what I labelled "repair A" was really *reduce `C_u`*, which is available
to either statistic and has nothing to do with reuse.

**3. The binding constraint is the AND, and it binds for Google's statistic too.** At matched
`C_u`=37 on real data:

| query | statistic | per-group pass | AND |
|---|---|---|---|
| tpch month | her max-bin | 98.8% | **0.0%** |
| tpch month | Google count | 98.8% | **0.0%** |
| tpch month\|prio | her max-bin | 96.4% | **0.0%** |
| tpch month\|prio | Google count | 98.8% | **0.0%** |

**Substituting Google's own statistic into the conjunction does not rescue it.** That is the
cleanest statement available: the conjunction fails on its own terms, independently of which
statistic feeds it and independently of `C_u`. Her design choices are sound; the AND is what does
not survive contact with real group-size distributions.

**4. Her statistic is genuinely weaker on fine groupings, and only there.** At `C_u`=1 with
truncation applied to the histogram (her Q4, applied to her own design):

| query | tau_AF | max-bin median | per-group pass | AND |
|---|---|---|---|---|
| tpch month | 43.2 | 738 | 98.8% | 0.0% |
| tpch month\|prio | 43.2 | 152 | 96.4% | 0.0% |
| tpch month\|nation | 43.2 | **31** | **2.4%** | 0.0% |

On coarse groupings the max-bin clears the threshold comfortably. On `month|nation` the median
max-bin is 31 against a threshold of 43.2, so the statistic itself fails -- this is the one place
the `0.32 x support` ratio actually bites, and it is a fine-grouping effect rather than a general
one.

**Score so far: Dandan has been right three times** -- the missing truncation (Q4), the framing of
the mechanism's purpose (Q2), and now the threshold comparison (Q1). Each of my errors made her
proposal look worse than it is. The surviving objection is narrow and unchanged: the conjunction
almost never fires, so the compact branch is almost never taken.

## Open threads — resume here

Paused 13 Aug 2026, mid-investigation. Nothing in flight is uncommitted; the whole state is this
file plus the scripts under `attacks/`.

- **Two adversarial agents were still running against the 1.36×** when work paused, re-deriving
  the steelmanned Google baseline and the δ argument from scratch. Their results were never read.
  If the 1.36× is going into a paper, redo that check — every previous headline in this document
  fell to exactly this kind of pass.
- **Neither free fix is implemented in `src/`.** The ℓ1 clip does not exist there yet, so
  `n_u := Σ_g |clip(t, −B, B)|` and the ≥1-vote gate must land *with* it, not after.
- **Not yet measured against the fixed baseline:** debiasing the clip loss (was 1.19–1.25× against
  the old one), and whether the 1.1×–2.4× range holds on StackOverflow / ClickBench — the earlier
  cross-dataset numbers all used the unfixed Google.
- **Dandan's histogram-based τ-thresholding is still untested inside this comparison**, and the
  ~1,000-PUs-per-group floor (where every arm returns ~100% error) is exactly the regime it
  targets. That is the most promising remaining direction.
- Everything here is grouped SUM on non-negative measures, static data, single aggregate.
- **Machine limits are real, and were hit twice.** (1) `day|region` (12,630 groups, 10.6M cells)
  OOM-killed the laptop during a full tune — the tuner holds ~100 arrays of cell length. (2) The
  first version of `full_stack.py` drove the load average to **43** with an oracle arm sweeping 60
  bound values x 3 trials x 4 filters = 720 full clip+bincount passes over every cell. Rules that
  follow: keep to `month|nation` (5.5M cells) or smaller, `threads=2`, **one process at a time**,
  and count how many full passes over the cell arrays a script implies before running it — that
  product, not the per-pass cost, is what kills the machine. `geometry_matched.py` and
  `full_stack.py` enforce `--max-cells`; `fineness_sweep.py` does not yet.
- **Attacked, and it shrank** — see "THE 4.5× IS A RESONANCE" above. Sensitivity, threshold and the
  Laplace arm's tuning are settled; the metric is fine on TPC-H but TPC-H cannot test it; the size
  of the gain is set by group-size dispersion and by ε, and off TPC-H the sign flips. What is still
  open there: whether any *sequence* of releases can pay for the harmonic-mean-`k_u` statistic that
  the Gaussian/Laplace choice needs, and whether the same band argument bounds the ℓ1-clip gain too
  (it should — the value channel has its own threshold-free geometry, so probably not, but it has
  not been checked).
- **Do not port yet.** The SASS floor is confirmed (28.63/43.66/74.66 at c=1/2/4) and Gaussian
  votes do recover the key set there — but end-to-end they *lose*, because SASS's value channel is
  independently broken and releasing more is worse. **Fix the SASS value channel first** (the
  `C_v` pincer: rank-cap bias at small `C_v`, NRS envelope collapse at large `C_v`), then re-measure.
  Two free fixes are worth doing regardless: clamp the released SUM/COUNT median to the public
  domain (src clamps only the AVG ratio, which is why rel-ℓ1 reaches 2349), and the vote-support
  gate.
- **Re-examine every headline in this document under a per-group metric.** Total-ℓ1 reported 8.6%
  for a release answering 7% of a key set. All the numbers here are total-ℓ1.

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
