# SIMD stochastic aggregates

This is the mode-agnostic core of the extension: the SIMD-accelerated stochastic
aggregation (SIMD-ASA) engine. A single query execution maintains **64 parallel
counters** per aggregate — one per sub-sample ("world") — represented as a
`LIST<FLOAT>` of length 64. Every privacy mode (PAC and the DP modes) is built on top
of this representation; the noise/terminal layer that collapses the 64 counters into a
released scalar is mode-specific and documented in `../pac/` and `../dp/`.

Under normal use, standard SQL aggregates (`SUM`, `COUNT`, …) are rewritten onto these
functions by the compiler (see `../pac/query_operators.md`). They can also be called
directly. Source: `src/aggregates/` (`as_count.cpp`, `as_sum.cpp`, `as_min_max.cpp`,
`as_clip_*.cpp`, `as_finalize.cpp`, `as_aggregate.cpp`).

> **Naming.** The SIMD aggregate functions are prefixed `as_` (accelerated stochastic).
> Counter aggregates return `LIST<FLOAT>`; `as_noised_*` / `dp_*` terminals return a
> scalar and belong to the privacy layer (`../pac/`, `../dp/`).

## The 64-counter representation

Each privacy unit (PU) is mapped to a 64-bit **sub-sample mask** by hashing its
`PRIVACY_KEY`. Bit `j` set means "this PU belongs to sub-sample `j`." An aggregate
keeps 64 running counters; an incoming row updates counter `j` only when bit `j` of the
PU's hash is set. The 64 counters are therefore 64 aggregates over 64 overlapping
sub-samples of the same query — the variance across them is what the privacy layer uses
to calibrate noise.

### Sub-sample assignment — `priv_hash`

`priv_hash(UBIGINT) → UBIGINT`

XORs the input key hash with a per-query hash derived from `privacy_seed`, so the same
PU gets a fresh sub-sample assignment on every query (analogous to a full resampling of
the possible worlds). When `pac_hash_repair` is set (default), the result is repaired to
have **exactly 32 bits set**, so each PU lands in exactly half the sub-samples — a
uniform 50% inclusion prior and maximum entropy across worlds. The repair applies up to
16 rounds of multiplicative hashing with a 64-bit prime; the fallback (all rounds fail)
is `0xAAAAAAAAAAAAAAAA`. The hash is always computed in a projection above the scan,
never inside the aggregate.

## Counter aggregates

Each returns a `LIST<FLOAT>` of 64 per-sub-sample values.

### `as_count`

```
as_count(UBIGINT key_hash) → LIST<FLOAT>
as_count(UBIGINT key_hash, ANY value) → LIST<FLOAT>
```

Stochastic COUNT: counter `j` is incremented when bit `j` of `key_hash` is set.
Implemented with **SWAR** (SIMD Within A Register): 8 packed `uint8_t` counters per
`uint64_t` lane, cascaded into 64-bit totals every 255 updates to avoid overflow. State
allocation is deferred and updates are buffered in small groups to cut cache misses
(important when there are very many groups).

### `as_sum`

```
as_sum(UBIGINT key_hash, ANY value) → LIST<FLOAT>
```

Stochastic SUM: adds `value` to counter `j` when bit `j` is set, using predication
rather than branching. Values are routed by magnitude into cascading **16-bit levels**
(4-bit level shift), and negative values are routed to a separate two-sided counter
array so mixed-sign data does not cancel prematurely. The approximate counters give a
worst-case relative error around 0.024% — negligible next to the privacy noise.
`HUGEINT` inputs bypass cascading and accumulate directly into a 128-bit total.

### `as_min` / `as_max`

```
as_min(UBIGINT key_hash, ANY value) → LIST<FLOAT>
as_max(UBIGINT key_hash, ANY value) → LIST<FLOAT>
```

Stochastic MIN/MAX: each counter holds the running extreme, updated by predication
(`value·bit + extreme·(1−bit)` then min/max). A pruning optimisation tracks the worst
extreme across all 64 counters and skips incoming values that cannot improve any
counter, refreshing the bound periodically.

### `as_avg`

```
as_avg(UBIGINT key_hash, ANY value) → LIST<FLOAT>
```

AVG is not maintained directly — averaging across sub-samples needs independent
numerator and denominator counter lists. A post-processing pass
(`src/query_processing/pac_avg_rewriter.cpp`, `RewritePacAvgToDiv`) rewrites `as_avg` (and
the noised terminal `as_noised_avg`) into a division of an `as_sum` counter list by an
`as_count` counter list. This applies to both compiler-generated and hand-written
`as_avg()` SQL.

### `priv_div`

```
priv_div(LIST<FLOAT>, LIST<FLOAT>) → LIST<FLOAT>
```

Element-wise division of two counter lists — the fused C++ equivalent of
`list_transform(list_zip(s,c), z -> z[1]/z[2])`. It is what `as_avg` decomposes into; the
noised terminal `as_noised_div` (privacy layer) fuses this division with noise.

## Clipping variants

```
as_clip_count / as_clip_sum / as_clip_min / as_clip_max → LIST<FLOAT>
```

Same counter aggregates, but with per-sub-sample support clipping applied during
accumulation (bounding how much a single PU can move any counter). They are emitted when
a contribution bound is in effect (`priv_clip_support` / the DP `dp_*_bound` settings);
the per-PU pre-aggregation and bound wiring is described in the mode docs. `as_clip_sum`
requires a positive support setting or it throws.

## Sample (sample-and-aggregate) variants

```
as_sample_count / as_sample_sum / as_sample_min / as_sample_max / as_sample_avg → LIST<FLOAT>
as_sample_count_mask(UBIGINT key_hash) → LIST<FLOAT>
as_sample_clip_sum(UBIGINT key_hash, DOUBLE value) → LIST<FLOAT>
```

These compute one aggregate answer **per sub-sample lane** rather than 64 overlapping
counters — the sample-and-aggregate primitive. Each PU is assigned to lanes by the
membership hash (disjoint for a single lane per PU; overlapping otherwise). This is the
engine layer beneath the `dp_sass` mechanism, which takes the median or mean of the
per-lane answers; see `../dp/dp_mechanisms.md` and `../dp/dp_sass_smooth_sensitivity.md`.
`as_sample_count_mask` produces the per-lane distinct-value bitmask used for
`COUNT(DISTINCT)`.

## Re-aggregation overloads

Every counter aggregate has a `LIST<FLOAT>` overload so counter lists can be combined
across subqueries or nested aggregations:

```
as_count(LIST<FLOAT>) → LIST<FLOAT>
as_sum(LIST<FLOAT>)   → LIST<FLOAT>
as_min(LIST<FLOAT>)   → LIST<FLOAT>
as_max(LIST<FLOAT>)   → LIST<FLOAT>
```

```sql
-- Inner query produces per-nation counter lists; outer re-aggregates them
SELECT as_sum(nation_counters) FROM (
    SELECT as_sum(priv_hash(hash(c_custkey)), c_acctbal) AS nation_counters
    FROM customer GROUP BY c_nationkey
);
```

## Terminals (privacy layer)

The functions that consume a `LIST<FLOAT>` and emit a released scalar are **not** part of
the mode-agnostic engine — they apply the privacy mechanism:

- **PAC**: `as_noised`, `as_noised_count/sum/avg/min/max/div`, `as_noised_clip_*`, and the
  categorical terminals (`priv_select`/`priv_filter` families). See
  `../pac/query_operators.md`.
- **DP**: `dp_noise` (Laplace), `dp_smooth_median_noise`, `dp_gupt_mean_noise`,
  `dp_sample_median`, `dp_sample_mask`. See `../dp/dp_mechanisms.md`.

Example — the counter list is the boundary; the terminal collapses it:

```sql
-- Raw 64 counters (engine) …
SELECT as_sum(priv_hash(hash(c_custkey)), c_acctbal) FROM customer;
-- … a PAC-noised scalar (privacy layer)
SELECT as_noised_sum(priv_hash(hash(c_custkey)), c_acctbal) FROM customer;
```
