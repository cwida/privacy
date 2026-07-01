# DP mechanisms — implementation reference

This note documents how the three **differential-privacy** modes of the extension
are implemented, and how the code maps to the SIMD-ASA paper and to the DP
literature. It is written for a reader who knows DP; it points at the code only
lightly (`file — Function`) and leans on the paper ("this implements Algorithm 2,
line N") and on Google DP / FLEX / GUPT / Nissim et al. for the parts that are not
this paper's contribution.

The privacy mode is selected by a single setting, `SET privacy_mode = '…'`. This
document only covers the DP modes (`dp_standard`, `dp_elastic`, `dp_sass`).

---

## 1. Overview & threat model

A **privacy unit (PU)** is the entity we protect — one PU = one row of a table
declared `SET PU` (identified by its `PRIVACY_KEY`) plus every row that links to it
through `PRIVACY_LINK` foreign keys. Two databases are **neighbors** when they
differ by one PU (user-level) or by one tuple (row-level); see §6.

All three DP modes rewrite a normal SQL aggregate query, transparently, into a plan
that (i) bounds each unit's contribution, (ii) adds calibrated noise, and (iii) for
grouped queries decides which group keys may be released. They differ in how
sensitivity is derived and in the neighbor relation they protect:

| mode | guarantee | level | sensitivity | partitions | basis |
|---|---|---|---|---|---|
| `dp_standard` | pure ε (ungrouped) / (ε,δ) (grouped) | **user** | global (per-PU contribution clipped to a constant) | private by default | Google DP (Wilson et al. 2020) |
| `dp_elastic`  | (ε,δ) | **row** | smooth elastic sensitivity `2·SES_β` over join max-frequencies | public / enumerated (FLEX) | FLEX (Johnson, Near & Song 2018) |
| `dp_sass`     | (ε,δ) (median) / pure ε (mean) | **user** | smooth sensitivity of the per-lane median | private by default | **this paper** (SIMD-SAA^udp) |

---

## 2. Mapping to the paper and literature

- **`dp_sass` is the paper.** It implements **Algorithm 2 (SIMD-SAA^udp compiler)**,
  the `ss_noised` subroutine, and **Theorem 4.1** (user-level (ε,δ)-DP). §11 walks
  the algorithm line by line.
- **`dp_standard`** is **Google DP** — the bounded-user-contribution SQL mechanism of
  *Wilson et al., "Differentially Private SQL with Bounded User Contribution"* (PoPETs
  2020). It is not in this paper; it is the standard global-sensitivity baseline.
- **`dp_elastic`** is **FLEX** — *Johnson, Near & Song, "Towards Practical Differential
  Privacy for SQL Queries"* (VLDB 2018), the paper's reference [21]. Treated as a
  **row-level** baseline exactly as Google DP treats FLEX in its comparison.

Compiler entry points: `src/compiler/privacy_mechanisms.cpp` —
`CompileDPStandardQuery`, `CompileDPElasticQuery` (both call `CompileDPLaplaceQuery`),
and `CompileDPSampleMedianQuery`.

---

## 3. The DP checker and rejected queries

Before any rewrite, a shared structural check runs
(`src/metadata/privacy_compatibility_check.cpp`, and `CheckDPAggregates` /
`CheckDPAggregateNode` in `privacy_mechanisms.cpp`). It accepts a single aggregation
over a linear FK path to the PU and rejects everything it cannot analyse, failing
loudly rather than releasing something unproven:

- **More than one aggregate node** (i.e. an aggregate over another aggregate's output
  — nested/`E`-type queries). This is exactly the paper's restriction: `T^udp`
  **excludes the expression type `E`** (aggregate-in-predicate / categorical). Such
  queries are rejected, not silently mishandled.
- **Protected columns in `GROUP BY` or `HAVING`** (a protected value would drive the
  released key set / a post-aggregation filter).
- **Unsupported aggregate functions.** `dp_standard`/`dp_elastic` support
  `COUNT`, `COUNT(DISTINCT)`, `SUM`, `AVG`. `dp_sass` additionally supports `MIN`/`MAX`
  (`allow_min_max` is set only for `dp_sass`).
- **`FILTER` on `COUNT(DISTINCT)`** (filtered distinct is not analysed).
- **Window functions, recursive CTEs, and (for DP) CTEs / some correlated subqueries.**
- **Missing required parameters** — e.g. `dp_sum_bound` for `SUM`, `dp_count_bound`
  for `COUNT` over a join, `dp_max_groups_contributed` for a grouped join, and
  `dp_delta` for any query that needs it (§7).
- **Self-joins** beyond the supported factor are rejected (`ValidateDPSelfJoins`).

---

## 4. Parameters and how they map to the paper

| setting | meaning | paper symbol |
|---|---|---|
| `dp_epsilon` | total privacy budget ε | ε |
| `dp_delta` | total failure probability δ (required whenever thresholding / smooth sensitivity is used) | δ |
| `dp_sum_bound` | per-contribution value clip for `SUM`/`AVG` | `L_i, Λ_i` (value range) |
| `dp_count_bound` | per-PU row/contribution clip for `COUNT` over a join | contribution bound |
| `dp_max_groups_contributed` | max output groups one PU may affect | **`C_u`** |
| `dp_sample_lanes` | number of sample lanes a PU is hashed into (`m` is fixed at 64) | subsample assignment (`m`; see §11) |
| `dp_sass_release` | `'median'` (smooth-sensitivity, (ε,δ)) or `'average'` (GUPT mean, pure ε) | release rule |
| `dp_sass_count_output_bound` / `dp_sass_sum_output_bound` | public output domain `[L,Λ]` for COUNT/SUM lane answers | `L_i, Λ_i` |
| `dp_sass_avg_lower/upper_bound`, `dp_sass_minmax_lower/upper_bound` | public output domain for AVG / MIN-MAX | `L_i, Λ_i` |
| `dp_sass_exact_balanced_lanes` | use the disjoint balanced-lane assignment when `dp_sample_lanes = 1` | — |
| `privacy_noise` | `false` zeroes all noise (deterministic testing) | — |
| `privacy_seed` | RNG seed for reproducible noise | — |
| `privacy_min_group_count` | **`dp_elastic` only**: a raw admin support filter (not DP; see §10) | — |

### Recommended settings for benchmarks

We report benchmarks at **`ε = 0.5`** and **`δ = 1/N_PU`** (one over the number of
privacy units). `δ = 1/N_PU` is the usual "cryptographically small relative to the
population" choice. Note this treats `N_PU` as a **public** quantity — see Open
Questions (§16): `δ` must be a fixed public parameter, so `1/N_PU` should be derived
from a *public estimate* of the dataset scale, never from the exact private PU count.

---

## 5. Assumptions

1. **PU identification.** Each PU is uniquely identified by its `PRIVACY_KEY`
   (one row per PU in the PU table). The `C_u = 1` used for single-table grouped
   queries relies on this (a non-unique key spanning groups would need `C_u > 1`).
2. **Public output domains.** All `dp_sass_*_bound` / `dp_sum_bound` / `dp_count_bound`
   values are admin-set, data-independent public constants.
3. **Elastic uses public/enumerated partitions.** `dp_elastic` follows FLEX's model
   in which the analyst enumerates the `GROUP BY` keys; it does not do DP partition
   selection (§10).
4. **Empty-lane fill is public.** The value used to fill empty sample lanes (§14) is a
   per-aggregate-kind constant derived from the public output domain, never from data.
5. **`sample_lanes` bounds per-PU spread.** `dp_sample_lanes` is the maximum number of
   lanes one PU can occupy; the noise scales use this (§11, §12).
6. **Unannotated tables are public (fail-open).** A joined table with no
   `PRIVACY_KEY`/`PRIVACY_LINK`/protected columns is treated as a public side input
   (`is_public_side_table`, `privacy_mechanisms.cpp`) — this requires the analyst to
   annotate *every* private table; an unannotated private table gets no protection.
7. **`N_PU` public** if `δ = 1/N_PU` is used (§16).

---

## 6. User-level vs row-level

- **User-level** (neighbor = add/remove *all* rows of one PU): `dp_standard` and
  `dp_sass`. Each PU's *total* contribution is bounded (clipping + `C_u`), so the
  guarantee is about entities, not tuples.
- **Row-level** (neighbor = change one tuple): `dp_elastic`. FLEX's elastic sensitivity
  `∏(max-frequency along the join chain)` bounds a **single-tuple** change, not a whole
  PU's contribution, so `dp_elastic` protects tuples, not entities. Extending FLEX to
  user-level would need additional per-user statistics (e.g. max rows one user
  contributes per table) and a new sensitivity analysis + proof — **future work** (§16).

---

## 7. Public vs private partitions and τ-thresholding

For a `GROUP BY` query the **set of released group keys** is itself an output, and it
can leak membership: a key backed by a single unit disappears when that unit is
removed.

- **Private partitions** (the key set is data-dependent): the release/suppress
  decision must be privatised. We follow Google DP / Wilson et al.: release a group
  only when its **noised** count of contributing units clears τ. This costs a
  `δ_η` slice (which cannot be zero), so the query is **(ε,δ)-DP**.
- **Public partitions** (the key set is fixed/enumerated, data-independent): releasing
  all keys leaks nothing, so no thresholding is needed and the key set stays pure-ε.

**Mechanism (paper Algorithm 2, lines 3–10; `FinalizeDPLaplace` and
`CompileDPSampleMedianQuery`).** The auxiliary count is `η = COUNT(DISTINCT pu)` per
group. We reserve a threshold budget `(ε_η, δ_η) = (ε/(c+1), δ/(c+1))` for `c`
user aggregates, add `Laplace(C_u/ε_η)` noise to `η`, and release the group only
when the noised `η' ≥ τ`, with

```
τ = 1 − C_u · log(2 − 2·(1 − δ_η)^(1/C_u)) / ε_η          (Wilson/Google; paper line 9)
```

`C_u = dp_max_groups_contributed`. τ ∈ [1, ∞): tiny `δ_η` → τ = ∞ → all groups
suppressed (graceful, no crash).

**Per-mode stance:** `dp_sass` and `dp_standard` do this **automatically for every
grouped query** (private by default), so grouped queries in those modes require
`dp_delta` and are (ε,δ). `dp_elastic` is the FLEX baseline and does **not** do DP
partition selection (§10).

---

## 8. Contribution bounding and what we estimate

**Bounding (all modes).** Per-PU value contribution is clipped to the public domain
(`greatest(least(v, Λ), L)`), and one PU is capped to `C_u` output groups by a
row-number/dense-rank window (`ApplyMaxGroupsContributed`) so it enters the guarantee
rather than merely filtering rows. On a join, `dp_standard` first **pre-aggregates per
PU** (`InsertPuPreAggregation`, grouping by the PU-adjacent FK) and clips each PU's
partial; `dp_elastic` clips per row instead.

**What is estimated vs. public.**
- `dp_standard`: **nothing data-dependent** — sensitivity is the public bound times
  `C_u` (global sensitivity).
- `dp_elastic`: the **max-frequency `mf`** of each join key in each table
  (`ComputeMfK`) is read from the data; the elastic sensitivity is `∏ mf` along the
  chain (§10).
- `dp_sass`: sensitivity is estimated from the released sample answers themselves
  (smooth sensitivity, §12).
- All **output-domain bounds are public** (admin-set), never estimated.

---

## 9. `dp_standard` — global sensitivity (Google DP)

`ClipAndComputeSensitivities → ApplyPerPuClipping`. Each PU's total contribution is
clipped to a fixed constant, so global sensitivity equals that constant
(data-independent, so ungrouped `dp_standard` needs no δ):

- single PU table: per-row clip — `COUNT` sensitivity 1, `SUM` = `dp_sum_bound`;
- join: pre-aggregate per PU, clip the partial to `dp_sum_bound` / `dp_count_bound`,
  then `SUM` the clipped partials — so a join with `COUNT` requires `dp_count_bound`;
- grouped: the vector sensitivity is multiplied by `C_u` (`sens = bound · C_u`).

Noise is `Laplace(sens / ε_value)`. For grouped queries, partition selection (§7)
applies and `dp_delta` is required, making grouped `dp_standard` (ε,δ)-DP; ungrouped
stays pure ε-DP. This is Google DP's bounded-user-contribution mechanism.

---

## 10. `dp_elastic` — FLEX (row-level)

`ClipAndComputeSensitivities` (elastic branch) → per-row clip + `ComputeElasticSensitivity`.
Elastic sensitivity is a per-query local-sensitivity upper bound for equijoins: walk
the FK chain, take each table's max join-frequency `mf`, and multiply. The
**smooth** version (δ > 0) decays the distance-`k` envelope:

```
SES_β = max_{k ≥ 0} [ ∏_i (mf_i + k) · e^(−βk) ],   β = ε / (2·ln(2/δ))
```

`ComputeSmoothElasticSensitivity` searches the FLEX window `k ∈ [0, n/β + margin]`
(no fixed ceiling — a fixed cap would under-noise for small ε/δ; it refuses if the
window exceeds 1e9). Noise is `Laplace(2·SES_β / ε)`, giving (ε,δ)-DP at the **row
level** (see §6).

`dp_elastic` follows FLEX's public/enumerated-partition model — it does **no DP
partition selection**. Its `privacy_min_group_count` setting is a **raw, un-noised
admin support filter**, not DP: under the public-partition assumption it is a utility
convenience; a row-level-DP-safe version would need a noised τ with `C_u = 1`
(deferred, since `dp_elastic` is the FLEX baseline). References: FLEX (Johnson, Near &
Song 2018) and its follow-up Chorus.

---

## 11. `dp_sass` — SIMD-SAA^udp (the paper)

`CompileDPSampleMedianQuery`. This is Algorithm 2. Each PU is hashed into sample
**lanes** of a fixed `m = 64`; per lane we compute the aggregate on that subsample,
then release either the **median** of the 64 lane answers (smooth-sensitivity Laplace,
(ε,δ)) or their **mean** (GUPT-style, pure ε).

**Membership function (paper's `H(pu) mod m`).** With `dp_sample_lanes = 1` and
`dp_sass_exact_balanced_lanes` (the paper's setting) each PU is assigned to exactly
one lane by a balanced, disjoint projection (`GetOrInsertBalancedSampleLaneProjection`)
— disjoint subsamples, as GUPT/the paper assume.

> **Note — lanes are not disjoint when `dp_sample_lanes > 1`.** For `sample_lanes > 1`
> a PU is hashed into up to `sample_lanes` lanes by `DpSampleHash`
> (`sample_hash |= 1 << ((key_hash >> 6i) & 63)`), so the subsamples **overlap** — a
> PU appears in several lanes, and there is currently **no mechanism to force the
> subsets to be disjoint** (the balanced/disjoint assignment exists only for
> `sample_lanes = 1`; two 6-bit chunks can even collide onto the same lane). The DP
> guarantee still holds because the noise scales already charge the overlap
> (`Laplace(sample_lanes·(U−L)/m)` for the mean, `beta_eff = β/sample_lanes` for the
> median — §12), but the disjoint-subsample assumption the SAA analysis relies on is
> not enforced for `sample_lanes > 1`.

**`ss_noised` (median release; paper's subroutine).** Sort the 64 clipped lane
answers, take `z_p` with `p = ⌈m/2⌉ = 32` (`values[31]`), compute the smooth
sensitivity `S*` of the median (§12), and add `Laplace(2·S*/ε')` with
`β = ε'/(2·ln(2/δ'))`. Cross-checked against Nissim–Raskhodnikova–Smith (2007).

**Budget (paper lines 4–6).** For `c` user aggregates, reserve `(ε_η, δ_η)` for the
partition-selection count and give each aggregate cell
`ε' = ε/((c+1)·C_u)`, `δ' = δ/((c+1)·C_u)`.

**Contribution bounding (paper line 2).** `ApplyMaxGroupsContributed` keeps at most
`C_u` (pu, group) pairs per PU; per-PU value contribution is clipped to the public
`[L, Λ]` domain.

**Releases.** `dp_sass_release='median'` → smooth-sensitivity Laplace, (ε,δ), needs
`dp_delta`. `dp_sass_release='average'` → **GUPT** mean (Mohan et al. 2012):
`mean = (Σ lanes)/m` + `Laplace(sample_lanes·(Λ−L)/(m·ε))`, pure ε, no δ — valid
**ungrouped** (grouped needs δ for partition selection).

**Partition selection** is automatic for grouped queries (§7).

---

## 12. Smooth sensitivity — the two computations

**Median (`dp_sass`).** Two code paths, in `src/aggregates/dp_laplace_noise.cpp`:
- **Exact** — `SmoothMedianSensitivityExact`: the Nissim–Raskhodnikova–Smith
  divide-and-conquer (`JList`) over the 64 sorted values bracketed by `[L, Λ]`
  sentinels, `O(m log m)`. Used on the bounded path (always, in practice).
- **Approximate** — `SmoothMedianSensitivity`: a capped scan, used only on the legacy
  unbounded path.

For `sample_lanes > 1` the envelope is reweighted with `beta_eff = β / sample_lanes`.
This is (a) β-smooth in PU distance and (b) a **conservative upper bound** on the true
PU-level smooth sensitivity (the PU terms are the `k = sample_lanes·d` subset of the
element-level max, and maxing over all `k` only grows it), so it over-noises if
anything. Full derivation: `SmoothMedianSensitivityExact` in
`src/aggregates/dp_laplace_noise.cpp`.

**Elastic (`dp_elastic`).** `ComputeSmoothElasticSensitivity` — the FLEX
`max_k ∏(mf_i+k)·e^(−βk)` search described in §10. Different object from the median
smooth sensitivity, same NRS `β`.

---

## 13. Per-aggregate handling

| aggregate | `dp_standard` / `dp_elastic` | `dp_sass` |
|---|---|---|
| `COUNT(*)` | clip per-PU row count (join: `dp_count_bound`), Laplace | per-lane count, median/mean of 64 |
| `COUNT(DISTINCT x)` | per-PU distinct count, clip, sum, Laplace | dedicated per-lane distinct path (bit-mask of distinct values per lane), median/mean |
| `SUM(x)` | clip `x` to `dp_sum_bound`, sum, Laplace | per-lane sum, median/mean |
| `AVG(x)` | decomposed → `SUM(clip x)` + `COUNT`; released as `noised_sum / max(noised_count, 1)` (Google bounded-mean shape) | per-lane average, **median/mean of the 64 lane averages** |
| `MIN(x)` / `MAX(x)` | not supported | per-lane min/max, median/mean; empty-lane identity = Λ (min) / L (max) |

`AVG` correctness is an open question (§16). Aggregate `FILTER (WHERE …)` is folded
into the value (`CASE WHEN cond THEN x END`) rather than kept as a separate filter, so
it survives the per-PU rewrite (`FoldFilterIntoValue`).

---

## 14. NULL handling

- **No data-dependent NULL from sparse groups.** A `dp_sass` aggregate produces 64
  lane answers; an *empty* subsample would naturally be NULL (for `AVG`/`MIN`/`MAX`)
  or 0 (for `COUNT`/`SUM`). Because the number of populated lanes is data-dependent,
  emitting NULL for "too few populated lanes" would leak group support. Instead every
  empty lane is filled with a **public, in-range default** (per aggregate kind:
  `COUNT`/`SUM` → 0, `MIN` → Λ, `MAX` → L, `AVG` → midpoint), so the populated-lane
  count is a data-independent 64 and the release is always a numeric value. This is
  the GUPT "fixed number of blocks, bounded block outputs" property.
- **Suppressed groups** (noised `η' < τ`, §7) are **dropped** from the result — they
  do not appear as NULL rows.
- **SQL NULLs in the data** follow normal aggregate semantics (e.g. `SUM` skips NULL
  inputs) before clipping/sampling.
- A **legacy unbounded** median path still returns NULL when fewer than 32 lanes are
  valid; it is not reachable from the compiler (which always supplies bounds) and is
  kept only for the raw scalar function.

---

## 15. Code flow

```
optimizer hook (pre_optimize_function, src/core/privacy_optimizer.cpp)
  └─ dispatch by privacy_mode
       └─ CompilePrivQuery (src/compiler/privacy_compiler.cpp)
            ├─ dp_standard → CompileDPStandardQuery ─┐
            ├─ dp_elastic  → CompileDPElasticQuery  ─┤→ CompileDPLaplaceQuery
            │                                         │   • ExtractDPFKChain
            │                                         │   • CheckDPAggregates / RewriteAvgAggregates
            │                                         │   • ClipAndComputeSensitivities (per-PU clip / elastic)
            │                                         │   • FinalizeDPLaplace (partition selection τ, budget split,
            │                                         │       WrapAggregateWithLaplace, AVG ratio projection)
            └─ dp_sass     → CompileDPSampleMedianQuery
                                • PU hashing / sample lanes
                                • RewriteAggregateToSampleMedian (per-PU pre-agg, C_u cap, sample terminals)
                                • partition-selection τ (ApplySupportFilter)
                                • WrapSampleMedianProjection (dp_smooth_median_noise / dp_gupt_mean_noise)
```

The hook runs **before** DuckDB's optimizers, so filters are still separate `FILTER`
nodes and `LIMIT` is a separate root; `ResolveOperatorTypes()` must be called before
reading projection types. Noise is added by the `dp_noise` scalar / the sample-median
terminals; a non-finite noise scale (sensitivity/ε overflow) **throws** rather than
releasing the raw value; `privacy_noise = false` zeroes noise for deterministic tests.

---

## 16. Open questions

1. **Is `N_PU` public?** We report benchmarks at `δ = 1/N_PU`. `δ` must be a fixed
   public parameter, so this is only sound if the dataset scale is public. Is it OK to
   assume `N_PU` is public? If not, we should use a conservative public upper bound on
   `N`, or spend a small separate budget to estimate it privately.
2. **Is our `AVG` correct?** `dp_standard`/`dp_elastic` release a **ratio of two
   independently-noised values** (`noised_sum / noised_count`), which is biased and
   whose (ε,δ) comes from composing the SUM and COUNT budgets; `dp_sass` releases the
   **median of 64 per-lane averages**. Is this the intended accounting for both?
3. **Non-disjoint lanes for `sample_lanes > 1`** (§11) — there is no mechanism to force
   disjoint subsamples; the SAA analysis assumes disjointness. Should `sample_lanes > 1`
   be restricted, or the overlap analysis stated explicitly?
4. **User-level FLEX** — extending `dp_elastic` from row-level to user-level needs new
   per-user statistics + a sensitivity analysis and proof (future work).
5. **τ under tiny δ** — a very small `δ_η` drives τ → ∞, suppressing all groups. Safe
   (graceful empty) but worth surfacing to analysts.
6. **Fail-open side tables** — an unannotated private table is treated as public
   (§5.6); this relies on analyst discipline.
