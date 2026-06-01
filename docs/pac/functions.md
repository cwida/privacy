# PAC Functions

Scalar and aggregate functions introduced by the PAC extension. These implement the SIMD-PAC-DB approach: a single query execution maintaining 64 stochastic counters (as `LIST<FLOAT>`) that implicitly capture 64 sub-samples.

Under normal usage, standard SQL aggregates (`SUM`, `COUNT`, etc.) are automatically rewritten into the PAC functions below — see [query_operators.md](query_operators.md) for details. They may also be called directly.

## Counter Aggregates

The core PAC aggregates. Each returns a `LIST<FLOAT>` containing 64 per-sub-sample counter values.

### priv_count

`priv_count(UBIGINT key_hash) → LIST<FLOAT>`
`priv_count(UBIGINT key_hash, ANY value) → LIST<FLOAT>`

Stochastic COUNT. Maintains 64 counters, each incremented when the corresponding bit in `key_hash` is set. Internally uses SWAR (SIMD Within A Register): 8 packed `uint8_t` counters per `uint64_t` lane, cascading into 64-bit totals every 255 updates to avoid overflow. State allocation is deferred and values are buffered in groups of 4 to reduce cache misses.

### priv_sum

`priv_sum(UBIGINT key_hash, ANY value) → LIST<FLOAT>`

Stochastic SUM. Adds `value` to counter `j` when bit `j` of `key_hash` is set, using predication rather than branching. Internally maintains separate positive and negative counter arrays (two-sided sum) to prevent cancellation with mixed-sign data. Uses approximate 16-bit counters across 25 lazily allocated cascading levels, yielding a worst-case relative error of ~0.024% — negligible compared to PAC noise.

### priv_min / priv_max

`priv_min(UBIGINT key_hash, ANY value) → LIST<FLOAT>`
`priv_max(UBIGINT key_hash, ANY value) → LIST<FLOAT>`

Stochastic MIN and MAX. Maintains 64 extreme values per counter, updated via predication: `value * bit + extreme * (1 - bit)` followed by min/max. A pruning optimization tracks a running global bound (the worst extreme across all 64 counters) and skips incoming values that cannot improve any counter; the bound is refreshed every 2048 updates.

### Example

```sql
-- Returns a list of 64 counter values
SELECT priv_sum(priv_hash(hash(c_custkey)), c_acctbal) FROM customer;
-- → [1234.5, 1180.2, 1312.1, ...]
```

## Re-aggregation Overloads

Each counter aggregate has a `LIST<FLOAT>` overload for combining counter lists across subqueries or nested aggregations.

`priv_count(LIST<FLOAT>) → LIST<FLOAT>`

`priv_sum(LIST<FLOAT>) → LIST<FLOAT>`

`priv_min(LIST<FLOAT>) → LIST<FLOAT>`

`priv_max(LIST<FLOAT>) → LIST<FLOAT>`

### Example

```sql
-- Inner query produces per-nation counter lists; outer re-aggregates them
SELECT priv_sum(nation_counters) FROM (
    SELECT priv_sum(priv_hash(hash(c_custkey)), c_acctbal) AS nation_counters
    FROM customer GROUP BY c_nationkey
);
```

## Terminal Wrappers

These functions consume `LIST<FLOAT>` counter lists and produce scalar results. They are used at the boundary where PAC's 64-world representation is collapsed to a single answer.

### priv_noised

`priv_noised(LIST<FLOAT>) → FLOAT`

Applies the PAC noise mechanism to a counter list: selects counter `J`, computes variance across all counters, and adds calibrated Gaussian noise.

```sql
-- Evaluate an expression across all 64 worlds via list_transform, then noise
SELECT priv_noised(
    list_transform(priv_sum(priv_hash(hash(c_custkey)), c_acctbal),
                   x -> CAST(x AS DECIMAL(18,2))))
FROM customer;
```

### priv_select

`priv_select(UBIGINT hash, LIST<BOOL>) → UBIGINT`

Combines a privacy unit hash with a per-sub-sample boolean predicate by converting the boolean list to a 64-bit mask and ANDing it with the hash. Bit `j` in the result is set only when both the sub-sample includes the row AND the predicate holds for world `j`. Used when a comparison against PAC counters feeds into an outer PAC aggregate — see [categorical queries](query_operators.md#case-3-priv_select--filters-with-a-sensitive-aggregation-above).

```sql
-- Mask hash: keep only worlds where the count exceeds 100
SELECT priv_select(priv_hash(hash(c_custkey)),
    list_transform(priv_count(priv_hash(hash(c_custkey))),
                   x -> x > 100))
FROM customer;
```

### priv_filter

`priv_filter(LIST<BOOL>) → BOOLEAN`

Probabilistic row filter for categorical queries where no PAC aggregate sits above the filter. The probability to pass is proportional to the amount of true-s in the 64 possible worlds. When `pac_mi = 0`, deterministic majority voting is used instead (passes if popcount > 32). See [categorical queries](query_operators.md#case-2-priv_filter--filters-on-non-sensitive-tuples).

```sql
-- Filter rows where the subquery count exceeds 100 across worlds
SELECT * FROM t
WHERE priv_filter(
    list_transform(subquery_counters, x -> x > 100));
```

## Fused Variants

Optimized versions that combine a comparison with `priv_select` or `priv_filter` in a single call, avoiding the `list_transform` lambda overhead. The rewriter emits these automatically when it detects simple comparisons against counter lists.

### priv_select variants

`priv_select_gt(UBIGINT hash, ANY value, LIST<FLOAT> counters) → UBIGINT`

`priv_select_gte(UBIGINT hash, ANY value, LIST<FLOAT> counters) → UBIGINT`

`priv_select_lt(UBIGINT hash, ANY value, LIST<FLOAT> counters) → UBIGINT`

`priv_select_lte(UBIGINT hash, ANY value, LIST<FLOAT> counters) → UBIGINT`

`priv_select_eq(UBIGINT hash, ANY value, LIST<FLOAT> counters) → UBIGINT`

`priv_select_neq(UBIGINT hash, ANY value, LIST<FLOAT> counters) → UBIGINT`

### priv_filter variants

`priv_filter_gt(ANY value, LIST<FLOAT> counters) → BOOLEAN`

`priv_filter_gte(ANY value, LIST<FLOAT> counters) → BOOLEAN`

`priv_filter_lt(ANY value, LIST<FLOAT> counters) → BOOLEAN`

`priv_filter_lte(ANY value, LIST<FLOAT> counters) → BOOLEAN`

`priv_filter_eq(ANY value, LIST<FLOAT> counters) → BOOLEAN`

`priv_filter_neq(ANY value, LIST<FLOAT> counters) → BOOLEAN`

### Example

```sql
-- Equivalent to priv_select + list_transform(counters, x -> value < x)
SELECT priv_select_gt(priv_hash(hash(c_custkey)), 100,
    priv_count(priv_hash(hash(c_custkey))))
FROM customer;
```
### priv_noised variants

Scalar-returning convenience functions that compose a counter aggregate with `priv_noised`. These are what standard SQL aggregates (`COUNT(*)`, `SUM(x)`, etc.) get rewritten to by the PAC compiler.

`priv_noised_count(UBIGINT hash) → BIGINT`

`priv_noised_count(UBIGINT hash, DOUBLE correction) → BIGINT`

`priv_noised_sum(UBIGINT hash, ANY value) → numeric`

`priv_noised_sum(UBIGINT hash, ANY value, DOUBLE correction) → numeric`

`priv_noised_avg(UBIGINT hash, ANY value) → DOUBLE`

`priv_noised_avg(UBIGINT hash, ANY value, DOUBLE correction) → DOUBLE`

`priv_noised_min(UBIGINT hash, ANY value) → type`

`priv_noised_min(UBIGINT hash, ANY value, DOUBLE correction) → type`

`priv_noised_max(UBIGINT hash, ANY value) → type`

`priv_noised_max(UBIGINT hash, ANY value, DOUBLE correction) → type`

The optional `correction` parameter adjusts the noise calibration. Internally, `priv_noised_avg` is rewritten to `priv_noised_div(priv_sum(...), priv_count(...))`.

```sql
-- Standard SQL rewriting: SELECT COUNT(*) FROM customer
-- becomes:
SELECT priv_noised_count(priv_hash(hash(c_custkey))) FROM customer;
```
## AVG Handling

PAC decomposes averages into sum/count pairs, since averaging across sub-samples requires independent counter lists for numerator and denominator.
It registers `priv_avg` and `priv_noised_avg` functions, but does not implement them. Rather, it rewrites them in a last pass over the plan into `priv_avg()` -> `priv_div(priv_sum(..), priv_count())` resp. `priv_noised_avg()`-> `priv_noised_div(priv_sum(..), priv_count())`. Here, `priv_div(s,c)` has the same semantics as `list_transform(list_zip(s,c), lambda x: x[1]/x[2])` but is implemented in C++ (fused) for more performance.

### priv_div

`priv_div(LIST<FLOAT>, LIST<FLOAT>) → LIST<FLOAT>`

Element-wise division of two counter lists. Used as an intermediate step when computing averages in categorical contexts (where the result feeds into `priv_select` or `priv_filter`).

### priv_noised_div

`priv_noised_div(LIST<FLOAT> sum_counters, LIST<FLOAT> count_counters) → FLOAT`

Fused division and noise application. Divides sum counters by count counters element-wise, then applies the PAC noise mechanism, i.e. `priv_noised()`. This is what `priv_noised_avg` is rewritten to internally.

### Example

```sql
-- What AVG(c_acctbal) becomes internally:
SELECT priv_noised_div(
    priv_sum(priv_hash(hash(c_custkey)), c_acctbal),
    priv_count(priv_hash(hash(c_custkey))))
FROM customer;
```

## Utility

### priv_hash

`priv_hash(UBIGINT) → UBIGINT`

XOR's the input hash with a per-query hash derived from `privacy_seed`, so the same privacy unit receives different sub-sample assignments on each query. This is analogous to a complete resampling of the possible worlds, making inference attacks across queries harder.

```sql
SELECT priv_hash(hash(id)) FROM employees;
```

When `pac_hash_repair` is set to `true` (the default), `priv_hash` additionally repairs the result to have **exactly 32 bits set**. Each bit represents one sub-sample: bit `j = 1` indicates the privacy unit is included in sub-sample `j`. The constraint of exactly 32 set bits guarantees a uniform 50% MIA prior and maximum entropy across sub-samples. The repair procedure applies up to 16 rounds of multiplicative hashing with a 64-bit prime (itself having 32 bits set), flipping bits until exactly 32 are set; if all rounds fail, returns `0xAAAAAAAAAAAAAAAA` as a fallback.
