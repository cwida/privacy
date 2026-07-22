# DP SASS Smooth Sensitivity

`privacy_mode = 'dp_sass'` implements a sample-and-aggregate style mechanism for
SQL aggregates. The compiler computes `m` sample answers (`64` by default), releases
their median, and adds Laplace noise calibrated by a smooth-sensitivity
calculation over the sorted sample answers.

This note explains what the implementation does and how it differs from the
smooth-sensitivity median algorithms in the literature and an alternative derivation.

## Quick Example

```sql
CREATE PU TABLE users(uid INTEGER, amount DOUBLE, PRIVACY_KEY(uid));
INSERT INTO users VALUES (1, 10.0), (2, 20.0), (3, 1000.0);

SET privacy_mode = 'dp_sass';
SET dp_epsilon = 1.0;
SET dp_delta = 1e-6;

-- Required sample-output domain bounds.
SET dp_sum_bound = 500.0;
SET dp_count_bound = 10.0;

SELECT SUM(amount), COUNT(*) FROM users;
```

The query is rewritten into sample aggregates. Each result cell is a list of `m`
sample answers. The default is `m = 64`; `dp_sass_m` also supports `128`, `256`,
and `512`. The final projection clips those answers, sorts them, takes
the median, computes smooth sensitivity around that median, and adds noise.

## Mechanism Summary

The implementation protects a privacy unit (PU), not a single tuple.

1. The planner finds a private aggregate query over a reachable PU.
2. The compiler pre-aggregates by PU below the private aggregate.
3. Each PU is hashed into `dp_sample_lanes` of `m` sample lanes.
4. Each sample lane computes a rescaled aggregate answer.
5. The final scalar receives the `m` sample answers as `LIST<FLOAT>`.
6. It clips every sample answer to an explicit bound.
7. It sorts the clipped answers and takes the lower median.
8. It computes a smooth-sensitivity scale from rank windows around that median.
9. It releases `median + Laplace(0, 2 * smooth_sensitivity / epsilon)`.

The default is:

```text
64 total sample lanes
dp_sass_m = 64
dp_sample_lanes = 1
```

`dp_sass_m` is the number of samples used by the median. `dp_sample_lanes` is the
maximum number of lane answers that one PU can influence. It is the implementation's
version of the `s` parameter in the PU-level smooth-sensitivity note.

## Settings

`privacy_mode`

Selects the mechanism. Use `'dp_sass'` for this path.

`dp_epsilon`

Privacy budget used by the smooth-sensitivity scalar. The compiler splits this
budget across visible aggregate outputs. AVG is decomposed into SUM and COUNT,
so it receives a further component split.

`dp_delta`

Failure probability for smooth sensitivity. It is required by `dp_sass`.

`dp_sample_lanes`

Number of sample lanes a PU can affect. The default is `1`; valid values are
`1` through `8` when `dp_sass_m = 64`. Larger `m` values require `1`.

`dp_sass_m`

Number of sample answers. The default is `64`; supported values are `64`, `128`,
`256`, and `512`. The `m = 64` path uses a machine-word membership mask. Larger
values use a direct lane identifier and variable-size aggregate state.

`dp_sum_bound`

Sample-output domain bound for numeric sample answers in `dp_sass`. SUM, MIN,
MAX, and the SUM component of AVG use `[-dp_sum_bound, dp_sum_bound]`.

`dp_count_bound`

Sample-output domain bound for COUNT sample answers in `dp_sass`. COUNT and the
COUNT component of AVG use `[0, dp_count_bound]`.

## Relation To The Literature

The relevant literature pattern is Nissim, Raskhodnikova, and Smith's
sample-and-aggregate plus smooth sensitivity for the median.

The standard median setup is:

```text
x_1 <= x_2 <= ... <= x_n
m = median rank
release median(x) + noise calibrated to S*_median(x)
```

For one changed input affecting one sample answer, the alternative derivation gives the exact
local-sensitivity envelope:

```text
A(k)(x) = max_{t=0,...,k+1} (x_{m+t} - x_{m+t-k-1})
S*(x) = max_k exp(-epsilon * k) * A(k)(x)
```

For one PU affecting up to `s` sample answers, the note replaces the rank
movement by `k * s`:

```text
A_s(k)(x) = max_{t=0,...,ks+1} (x_{m+t} - x_{m+t-ks-1})
S*_s(x) = max_k exp(-epsilon * k) * A_s(k)(x)
```

The note also describes an `O(n log n)` exact algorithm. In this implementation,
`n = dp_sass_m` and is at most 512. The bounded terminal currently uses the direct
exact `O(n²)` interval scan.

## What We Do Today

The bounded path implements the **exact** smooth sensitivity of the median using
the Nissim–Raskhodnikova–Smith interval envelope.

Setup over the `n = m` clipped, sorted sample answers `x_1 ≤ … ≤ x_n`:

```text
n = dp_sass_m
p = ceil(n / 2)                       # 1-indexed lower-median rank
x_0    = lower_bound                  # domain sentinels bracket the bounded range
x_{n+1} = upper_bound
beta   = epsilon / (2 * log(2 / delta))
s      = dp_sample_lanes
beta_eff = beta / s
SCORE(i, j) = (x_j - x_i) * exp(-beta_eff * (j - i - 1))   # over pairs i <= p <= j
```

The implementation scans every pair `i ≤ p ≤ j`. The released sensitivity is

```text
S* = max over i in [0, p], j in [p, n+1] of SCORE(i, j)
```

This is the exact NRS median smooth-sensitivity envelope: it considers every
interval bracketing the median, so it cannot miss an off-center gap.

The lower/upper sentinels follow the bounded-domain convention in the median
literature, where ranks outside the observed data map to the domain endpoints.
The image in the source note writes `x_0 ← 0` for a non-negative `[0, Λ]` domain;
we generalize the lower sentinel to `lower_bound` because SUM uses `[-B, B]`.

The `beta_eff = beta / s` rescaling handles the PU step size. One PU can affect up
to `s = dp_sample_lanes` sample answers, i.e. up to `s` element changes in the
sorted vector. A `beta/s`-smooth element-level bound is therefore `beta`-smooth in
PU distance. For the default `s = 1` this is the source algorithm verbatim; for
`s > 1` it is a sound upper bound on the PU-level smooth sensitivity (it can only
add noise, never remove it).

## Difference From the Alternative Derivation

The implementation now matches the note's exact sensitivity formula. The
remaining differences are structural:

1. **Bounded sample count.**

   The alternative derivation describes median smooth sensitivity over arbitrary `n`.
   This implementation supports `n ∈ {64, 128, 256, 512}`. The default 64-answer
   path reuses the extension's 64-counter execution model.

2. **Exact bounded algorithm.**

   The bounded path computes the exact NRS smooth-sensitivity envelope over all
   intervals bracketing the median with a direct `O(n²)` scan.

3. **Explicit sample-output clipping.**

   The scalar clips the `n` sample answers before sorting and before computing
   smooth sensitivity. This enforces the bounded domain needed for finite smooth
   sensitivity. The bounds are user settings, not inferred from the data.

## Current Coherence Assessment

The mechanism is coherent as a systems implementation of sample-and-aggregate
over SQL using a bounded number of sample lanes. The bounded sensitivity routine
computes the exact NRS median smooth-sensitivity envelope
(`src/aggregates/dp_laplace_noise.cpp`, `SmoothMedianSensitivityExact`).

Because the exact envelope considers every interval bracketing the median, it can
only preserve or increase the noise scale relative to the old centered window.
The change improves the proof story and literature alignment; it does not improve
utility.

## Implementation Flow

The implementation has three separate jobs: collect privacy metadata, rewrite
the query, and collapse the `m` sample answers to one noised scalar.

### Metadata

Privacy DDL marks a table as a privacy-unit table and records how linked tables
reach that PU. For example:

```sql
ALTER TABLE users ADD PRIVACY_KEY (uid);
ALTER TABLE users SET PU;
ALTER TABLE orders ADD PRIVACY_LINK (user_id) REFERENCES users(uid);
```

This phase does not compute sensitivity. It only tells the later rewrite which
rows belong to the same protected individual.

### Rewrite Selection

When a query runs, the optimizer reads:

```sql
SET privacy_mode = 'dp_sass';
```

If the query touches protected data and contains supported aggregates, the
privacy compiler takes over before DuckDB's normal optimizer. The shared
compatibility checks reject unsupported protected-column access, unsupported
query shapes, and aggregates outside the supported set.

### Bound Validation

`dp_sass` requires explicit sample-output bounds.

```sql
SET dp_sum_bound = 500.0;
SET dp_count_bound = 10.0;
```

The rewrite uses the bounds as follows:

```text
COUNT      -> [0, dp_count_bound]
SUM        -> [-dp_sum_bound, dp_sum_bound]
MIN / MAX  -> [-dp_sum_bound, dp_sum_bound]
AVG        -> SUM component plus COUNT component
```

The bounds are enforced in the terminal scalar by clipping the `m` sample answers
before sorting and before computing smooth sensitivity.

### PU Pre-Aggregation

The compiler first makes each PU contribute one value per aggregate. This is the
PU-level contribution step.

```text
SUM(v)   -> one SUM(v) per PU
COUNT(*) -> one COUNT(*) per PU
MIN(v)   -> one MIN(v) per PU
MAX(v)   -> one MAX(v) per PU
```

This lower aggregation is important. Without it, one PU with many rows could
affect the samples through many independent rows instead of through one
bounded PU contribution.

### Sample Aggregation

The upper aggregate computes `m` sample answers. Each PU is assigned to
`dp_sample_lanes` of those lanes.

```text
default dp_sample_lanes = 1
maximum dp_sample_lanes = 8
```

If `m = 64`, lane membership is represented as a 64-bit mask. Larger values use
`hash(PU) mod m` directly and require `dp_sample_lanes = 1`. If
`dp_sample_lanes = 1`, one PU can affect one sample answer. If
`dp_sample_lanes = 8`, one PU can affect eight sample answers. This is the
implementation's `s` parameter for PU-level smooth sensitivity.

The upper aggregate returns:

```text
[sample_0_answer, sample_1_answer, ..., sample_(m-1)_answer]
```

Each sample answer is rescaled to compensate for the lane inclusion probability.

### Median And Noise

The terminal scalar receives the `m` sample answers and does:

1. Clip each sample answer to the configured domain.
2. Sort the clipped answers.
3. Take the lower median.
4. Compute the smooth-sensitivity scale from rank windows around that median.
5. Add Laplace noise with scale `2 * smooth_sensitivity / epsilon`.

With `privacy_noise = false`, the scalar returns the clipped sample median
without adding noise. That is useful for tests and debugging only.

### AVG Handling

AVG is not sampled as a primitive. It is rewritten into two private components:

```text
AVG(v) -> noised_sample_median(SUM(v)) / noised_sample_median(COUNT(*))
```

The SUM and COUNT components each consume their own budget share. The final
division is post-processing.

## COUNT, SUM, AVG, MIN, And MAX

COUNT uses `[0, dp_count_bound]`.

SUM uses `[-dp_sum_bound, dp_sum_bound]`.

AVG is decomposed into SUM and COUNT components. Each component is noised
separately, then the final projection computes:

```text
noised_sum / noised_count
```

MIN and MAX use `[-dp_sum_bound, dp_sum_bound]`. They are formally bounded by
the configured sample-output domain, but utility can be weak because sample
extremes are unstable.

## Relation To Chorus And FLEX

This implementation is not a Chorus/FLEX elastic-sensitivity rewrite.

FLEX and Chorus derive SQL sensitivity from query structure, join frequencies,
and value-range metadata. This extension's `dp_elastic` mode is closer to that
family.

`dp_sass` instead uses a black-box sample-and-aggregate idea:

```text
SQL aggregate -> m sample aggregate answers -> median -> smooth sensitivity
```

The systems twist is that the `m` sample answers are computed through the
extension's existing counter/list infrastructure in one query plan, rather than
by running many independent SQL queries.

## Known Limitations

- The bounded sensitivity routine uses the exact NRS median envelope; the
  unbounded 4-arg `dp_smooth_median_noise` overload still uses the older centered
  window, but it is not emitted by the dp_sass rewrite (which always passes
  domain bounds) and exists only as a fallback for direct callers.
- For `dp_sample_lanes > 1` the `beta_eff = beta / s` rescaling is a sound upper
  bound on the PU-level smooth sensitivity, slightly more conservative than the
  note's discrete `A_s(k)` (multiples-of-`s`) formula.
- `dp_sum_bound` and `dp_count_bound` are required user-provided bounds.
- MIN and MAX are supported only as bounded-domain sample medians; utility may be
  poor.
- Grouped `dp_sass` queries compute an automatic noised `COUNT(DISTINCT pu)` support
  threshold from `dp_delta`, `dp_epsilon`, and `dp_max_groups_contributed`.
- Grouped `COUNT(DISTINCT)` under `dp_sass` thresholds contributing privacy units,
  not the number of distinct values.
- Nested aggregates are not supported; use one aggregate node with one or more
  aggregate expressions.
