# DP SASS Optimizations

This note records the `dp_sass` execution changes made for the sampling benchmark
work. The goal is to remove avoidable full-table work while preserving the user-level
DP proof obligations: stable sample membership, bounded per-PU contribution, bounded
cross-group contribution, and private partition selection for grouped queries.

## Summary

The main change is to stop assigning SASS lanes with a global rank:

```sql
ROW_NUMBER() OVER (ORDER BY hash(pu_key)) % 64
```

`dp_sass` now builds the sample key from a stable hash of the privacy-unit key. For
`dp_sample_lanes = 1`, each privacy unit maps to exactly one of the 64 lanes:

```text
lane = hash(pu_key) mod 64
```

The hash is computed after ordinary filters have been pushed toward scans. This removes
the global `WINDOW`/sort pipeline breaker from filtered queries and avoids hashing rows
that do not contribute to the query result.

## What Changed

### Stable Hash Lane Assignment

Before this change, the planner inserted a helper projection above the PU scan or the
PU-adjacent linked table. That helper used `ROW_NUMBER` or `DENSE_RANK` ordered by a
hash of the key columns, then encoded the rank into the low bits consumed by the SASS
aggregate functions.

That gave exact balance on the current dataset, but it had two problems:

- It forced a global window/sort over the input.
- It was not stable under neighboring changes unless the proof assumed a fixed public
  universe and fixed public ordering.

The compiler now propagates the raw PU key column through filters, projections, and
joins to the aggregate input. It then builds:

```text
dp_sass_lane_key = hash(propagated_pu_key)
```

The existing aggregate update path still calls `DpSampleHash(dp_sass_lane_key,
dp_sample_lanes)`. For `dp_sample_lanes = 1`, `DpSampleHash` reads only one 6-bit lane
index and sets exactly one bit. Therefore each PU contributes to exactly one lane.

For `dp_sample_lanes > 1`, the existing semantics remain overlapping: one PU can set up
to `dp_sample_lanes` lane bits. The smooth-sensitivity and GUPT mean noise paths already
charge this spread. The benchmark path uses `dp_sample_lanes = 1`.

### Filter-First Execution

The old rank-based lane assignment had to run before filters because the lane was a
synthetic column produced near the scan. The new path propagates the PU key itself and
computes the hash later, so DuckDB can push ordinary query filters into scans before the
SASS lane hash is evaluated.

A filtered single-table plan now has this shape:

```text
SEQ_SCAN users
  Filters: ((uid % 2) = 0)
PROJECTION dp_sass_lane_key
HASH_GROUP_BY by dp_sass_lane_key
UNGROUPED_AGGREGATE as_sample_sum(...)
PROJECTION dp_smooth_median_noise(...)
```

There is no global `WINDOW` operator in this path.

### No PAC Hash Fallback

`dp_sass` no longer falls back to the generic PAC hash builder. The accepted path is:

```text
build stable PU-key hash -> continue
cannot build stable PU-key hash -> reject query
```

This keeps the SASS proof story simple. Every accepted `dp_sass` query uses stable
sample membership rather than PAC's hash-projection machinery.

### Removed Dead Balanced-Lane Helper

The old `InsertBalancedSampleLaneProjectionAboveGet` and
`GetOrInsertBalancedSampleLaneProjection` helpers were removed. They were the only
source of the global SASS lane `ROW_NUMBER`/`DENSE_RANK` window.

Contribution-bounding windows remain. Those are different and still required for DP:

```sql
ROW_NUMBER() OVER (
    PARTITION BY pu_key
    ORDER BY hash(pu_key, group_key)
) <= dp_max_groups_contributed
```

This per-PU cap enforces `C_u`, the maximum number of output groups one PU may affect.

## Why This Preserves DP

The DP argument depends on bounded influence of one privacy unit. Stable hash assignment
preserves that invariant:

- Removing one PU removes that PU from its assigned lane.
- All other PUs keep the same lane.
- With `dp_sample_lanes = 1`, one PU affects at most one lane answer.
- With `dp_sample_lanes = s`, one PU affects at most `s` lane answers.

This matches the sensitivity assumptions used by the SASS release functions.

The following DP-critical pieces are unchanged:

- Per-PU contribution clipping for `COUNT`, `SUM`, `AVG`, `MIN`, and `MAX`.
- Cross-group contribution bounding with `dp_max_groups_contributed = C_u`.
- Noisy support thresholding for grouped `dp_sass` queries.
- Budget split between value releases and partition selection.
- Public output-domain bounds for smooth-median releases.

The old exact-balanced assignment was a utility optimization, not a privacy
requirement. Exact balance is not needed for DP. Stability under neighboring changes is
the property the proof needs.

## Probabilistic Balance

With stable hash assignment and `dp_sample_lanes = 1`, lane sizes follow the standard
balls-into-bins distribution:

```text
lane_count_j ~ Binomial(N_PU, 1/64)
mean         = N_PU / 64
stddev       = sqrt(N_PU * (1/64) * (63/64))
rel_stddev   = sqrt(63 / N_PU)
```

For ClickBench, the benchmark database had about 17.63 million distinct `UserID` values:

```text
N_PU             = 17.63M
mean per lane    = 275,484 PUs
stddev per lane  = 521 PUs
relative stddev  = 0.189%
```

Using a normal approximation with a union bound over 64 lanes:

```text
P(any lane differs by more than 0.5%)  ~= 5.2e-1
P(any lane differs by more than 1.0%)  ~= 7.8e-6
P(any lane differs by more than 2.0%)  ~= 2.4e-24
```

So small imbalance is normal, but large imbalance is negligible at ClickBench scale.
For tiny tests, balance can be visibly noisy. For example, at `N_PU = 128`, the mean is
only two PUs per lane and the relative standard deviation is about 70%. Tests should not
assert exact lane balance under stable hashing.

## Public Bounds

The optimization work also clarified how bounds should be treated.

These settings are mechanism parameters:

```sql
SET dp_count_bound = ...;
SET dp_sum_bound = ...;
SET dp_max_groups_contributed = ...;
SET dp_sass_count_output_bound = ...;
SET dp_sass_sum_output_bound = ...;
```

They must be public, policy-provided, or computed with their own DP budget. The mechanism
must not silently infer them from the private database during query compilation.

For benchmarks it is acceptable to compute oracle bounds outside the timed query path and
set them explicitly. The paper should describe those as public/oracle benchmark
parameters. This isolates mechanism overhead without making private bound inference part
of the DP claim.

The SASS aggregate finalizers rescale each sampled lane by `DpSampleRescale(sample_lanes)`
before `dp_smooth_median_noise` / `dp_gupt_mean_noise` clamps to the public output
domain. Public SASS output-domain bounds must therefore cover the rescaled full-answer
estimator, not the raw per-lane sample partial:

```text
dp_sass_count_output_bound = dp_count_bound * ceil(N_PU)
dp_sass_sum_output_bound   = dp_sum_bound   * ceil(N_PU)
```

This derivation is valid when `N_PU`, `dp_count_bound`, and `dp_sum_bound` are treated
as public.

## Validation

The focused validation for this change was:

```bash
cmake --build build/release --parallel 8
build/release/test/unittest "test/sql/dp_sample_median.test"
build/release/test/unittest "test/sql/dp_sass_stability_query.test"
```

A real `EXPLAIN` check on a filtered SASS query confirmed the intended plan shape:

```sql
EXPLAIN SELECT COUNT(*) FROM users WHERE uid % 2 = 0;
```

The resulting plan pushes the filter into the scan and computes `dp_sass_lane_key` above
the filtered scan. It contains no global SASS lane `WINDOW`.

## Remaining Limitations

- `dp_sass` still supports only query shapes accepted by the DP compatibility checker.
- Contribution-bounding windows remain and are DP-critical.
- `dp_sample_lanes > 1` keeps overlapping-subsample semantics. The benchmark and the
  clean disjoint-subsample story use `dp_sample_lanes = 1`.
- Exact per-lane balance is no longer guaranteed. At large `N_PU`, hash balance is tight
  with high probability.
