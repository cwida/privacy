# DP SASS Smooth Sensitivity

`privacy_mode = 'dp_sass'` implements a sample-and-aggregate style mechanism for
SQL aggregates. The compiler computes 64 sample answers, releases the median of
those answers, and adds Laplace noise calibrated by a smooth-sensitivity
calculation over the sorted sample answers.

This note explains what the implementation does and how it differs from the
smooth-sensitivity median algorithms in the literature and Dandan's note.

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

The query is rewritten into sample aggregates. Each result cell is a list of 64
sample answers. The final projection clips those 64 answers, sorts them, takes
the median, computes smooth sensitivity around that median, and adds noise.

## Mechanism Summary

The implementation protects a privacy unit (PU), not a single tuple.

1. The planner finds a private aggregate query over a reachable PU.
2. The compiler pre-aggregates by PU below the private aggregate.
3. Each PU is hashed into `dp_sample_lanes` of 64 sample lanes.
4. Each sample lane computes a rescaled aggregate answer.
5. The final scalar receives the 64 sample answers as `LIST<FLOAT>`.
6. It clips every sample answer to an explicit bound.
7. It sorts the 64 clipped answers and takes `values[31]` as the median.
8. It computes a smooth-sensitivity scale from rank windows around that median.
9. It releases `median + Laplace(0, 2 * smooth_sensitivity / epsilon)`.

The default is:

```text
64 total sample lanes
dp_sample_lanes = 1
```

`dp_sample_lanes` is not the number of samples used by the median. The median
always uses the 64 lane answers. `dp_sample_lanes` is the maximum number of lane
answers that one PU can influence. It is the implementation's version of the
`s` parameter in the PU-level smooth-sensitivity note.

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
`1` through `8`.

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

For one changed input affecting one sample answer, Dandan's note gives the exact
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

The note also describes an `O(n log n)` exact algorithm. That matters when `n`
is the full dataset size. In this implementation `n` is fixed at 64 sample
answers, so even an exact `O(n^2)` scan would be cheap.

## What We Do Today

The bounded path implements the **exact** smooth sensitivity of the median using
the Nissim–Raskhodnikova–Smith divide-and-conquer algorithm (Algorithm 1
`SmoothSensitivityMedian` + Algorithm 2 `JList`).

Setup over the 64 clipped, sorted sample answers `x_1 ≤ … ≤ x_64`:

```text
n = 64
p = ceil(n / 2) = 32                  # 1-indexed median rank; released median = values[31]
x_0    = lower_bound                  # domain sentinels bracket the bounded range
x_{65} = upper_bound
beta   = epsilon / (2 * log(2 / delta))
s      = dp_sample_lanes
beta_eff = beta / s
SCORE(i, j) = (x_j - x_i) * exp(-beta_eff * (j - i - 1))   # over pairs i <= p <= j
```

The algorithm calls `JList(0, p, p, n+1)`, which fills the optimal `j*(b)` for
every `b in [0, p]` by exploiting the monotonicity of `j*` in `b` (each recursion
halves the row range and narrows the `[L, U]` column range to `[L, j*(b)]` and
`[j*(b), U]`). The released sensitivity is

```text
S* = max over i in [0, p] of SCORE(i, j*(i))
```

This is the exact NRS median smooth-sensitivity envelope: it considers every
interval bracketing the median, so it cannot miss an off-center gap. With `n = 64`
the cost is negligible.

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

## Difference From Dandan's Note

The implementation now matches the note's exact sensitivity formula. The
remaining differences are structural:

1. **Fixed 64 sample answers.**

   Dandan's note describes median smooth sensitivity over arbitrary `n`. Our
   `n` is always 64, because the extension already has a 64-counter execution
   model.

2. **Exact `O(n log n)` algorithm.**

   The bounded path now uses the divide-and-conquer exact algorithm (Algorithm 1
   + `JList`). It computes the exact NRS smooth-sensitivity envelope over all
   intervals bracketing the median, so it does not miss off-center gaps. (With
   `n = 64` even an `O(n^2)` scan would be cheap; the divide-and-conquer form is
   used to follow the source algorithm directly.)

3. **Explicit sample-output clipping.**

   The scalar clips the 64 sample answers before sorting and before computing
   smooth sensitivity. This enforces the bounded domain needed for finite smooth
   sensitivity. The bounds are user settings, not inferred from the data.

## Current Coherence Assessment

The mechanism is coherent as a systems implementation of sample-and-aggregate
over SQL using 64 vectorized sample lanes, and the bounded sensitivity routine
now computes the exact NRS median smooth-sensitivity envelope via the published
`SmoothSensitivityMedian` / `JList` divide-and-conquer algorithm
(`src/aggregates/dp_laplace_noise.cpp`, `SmoothMedianSensitivityExact`).

Because the exact envelope considers every interval bracketing the median, it can
only preserve or increase the noise scale relative to the old centered window.
The change improves the proof story and literature alignment; it does not improve
utility.

## Implementation Flow

The implementation has three separate jobs: collect privacy metadata, rewrite
the query, and collapse the 64 sample answers to one noised scalar.

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

The bounds are enforced in the terminal scalar by clipping the 64 sample answers
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

The upper aggregate computes 64 sample answers. Each PU is assigned to
`dp_sample_lanes` of the 64 lanes.

```text
default dp_sample_lanes = 1
maximum dp_sample_lanes = 8
```

If `dp_sample_lanes = 1`, one PU can affect one sample answer. If
`dp_sample_lanes = 8`, one PU can affect eight sample answers. This is the
implementation's `s` parameter for PU-level smooth sensitivity.

The upper aggregate returns:

```text
[sample_0_answer, sample_1_answer, ..., sample_63_answer]
```

Each sample answer is rescaled to compensate for the lane inclusion probability.

### Median And Noise

The terminal scalar receives the 64 sample answers and does:

1. Clip each sample answer to the configured domain.
2. Sort the 64 clipped answers.
3. Take `values[31]` as the median.
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
SQL aggregate -> 64 sample aggregate answers -> median -> smooth sensitivity
```

The systems twist is that the 64 sample answers are computed through the
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
- `privacy_min_group_count`, when set, is treated as the admin-provided support
  threshold for DP grouped queries.
- Grouped `COUNT(DISTINCT)` under `dp_sass` applies `privacy_min_group_count`
  to contributing privacy units, not to the number of distinct values.
