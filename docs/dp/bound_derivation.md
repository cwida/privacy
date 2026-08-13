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

## HONEST HEADLINE — 4.65× on a τ-binding query, both fully tuned

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
