# Fixed-sample filterless aggregates

## Motivation

A database can safely run a bounded DP aggregate only when the lower and upper contribution bounds
are public or selected by a private mechanism. Deriving an exact bound after `WHERE` is unsafe: a
high-value customer's filter membership can change the bound, which changes the output's noise
scale. This prototype retains all qualifying rows plus a fixed hash sample of non-qualifying PUs.
Only that fixed sample supplies the unfiltered per-PU values used to estimate exponential clipping
bins. Qualifying PUs outside the sample affect the answer but cannot affect the selected bound.

This is a prototype, not yet a production DP mode. Unlike Peter's raw-histogram baseline, it always
privatizes histogram support with an explicit budget split whenever privacy noise is enabled.

## Running query

```sql
SELECT month,
       SUM(amount) AS revenue,
       COUNT(*) AS purchases,
       AVG(amount) AS average_purchase
FROM purchases
WHERE segment = 'private_banking'
GROUP BY month;
```

Assume `customer_id` is the privacy unit. The rewrite evaluates the predicate as `X`, hashes the
privacy unit as `h`, and retains a row when:

```sql
X OR ((h >> (64 - p)) = 0)
```

The rewrite temporarily uses bit 0 as the row's predicate marker:

```text
encoded_pu = (h & ~1) | CAST(CASE WHEN X THEN true ELSE false END AS UBIGINT)
```

There is no stored sample-lane column. The high bits of the stable PU hash define the deterministic
sample. The lower aggregate clears the marker bit and groups once by logical `(PU, group_key)`.

## Execution, step by step

1. Widen `WHERE X` to `X OR sampled(PU)`. With `p=6`, an inactive PU is retained with probability
   `1/64`; every qualifying row is retained.
2. Pre-aggregate once by every SQL group key and the logical PU. For every SUM or COUNT component,
   produce both `aggregate(value) FILTER (WHERE X)` for the answer and `aggregate(value)` over all
   retained rows for the histogram. Also count qualifying rows for partition selection.
3. If the PU belongs to the fixed sample, put its unfiltered partial in a public factor-4 magnitude
   bin and increment support by `2^p`. Ignore histogram partials from non-sampled PUs, including
   qualifying ones. A sampled PU sees all its rows because `X OR sampled(PU)` is true for that PU.
4. Independently put each qualifying PU's filtered partial in the answer channel. It does not add
   histogram support merely because it qualifies.
5. Select the highest bin whose support reaches `dp_filterless_clip_support`, separately for the
   positive and negative sides of SUM. Its upper edge is the clipping bound.
6. Clip active partials above the selected bound. They are clipped, not omitted.
7. Add Laplace noise using `selected_bound * C_u / component_epsilon`.
8. COUNT follows the same path with nonnegative contributions. AVG is decomposed into SUM and
   `COUNT(value)` and receives half of the aggregate's budget per component.

For grouped queries, the prototype retains the existing Google-style mechanisms. It limits each
logical PU to `dp_max_groups_contributed` groups, counts distinct qualifying logical PUs for
partition selection, and applies the existing noised threshold. Sample-only PUs can influence
clipping bounds but cannot cause an absent filtered group to appear.

## SQL-callable aggregates

The aggregate implementations can be called directly in a `SELECT` for debugging:

```sql
SELECT filterless_sum(pu_hash, active, filtered_sum, unfiltered_sample_sum),
       filterless_count(pu_hash, active, filtered_count, unfiltered_sample_count),
       filterless_avg(pu_hash, active,
                      filtered_sum, filtered_count,
                      unfiltered_sample_sum, unfiltered_sample_count)
FROM per_pu_partials;
```

`filterless_sum_debug`, `filterless_count_debug`, and `filterless_avg_debug` return selected bounds,
clipped values, noise scales, bin indices, and active/sample counts. The compiler uses overloads
that also receive the component epsilon, `C_u`, and a stable group nonce. The nonce includes the
normalized SQL hash, so different predicates do not accidentally reuse cancelable value, histogram,
or partition-selection noise; an identical query remains deterministic for a fixed privacy seed.

## Settings

| Setting | Meaning |
|---|---|
| `privacy_mode='dp_filterless'` | Enable the compiler rewrite. |
| `dp_filterless_sample_bits=p` | Put PUs in the fixed histogram sample at `2^-p`; default 0 means sample all PUs. |
| `dp_filterless_clip_support` | Required weighted support for a bin. There is no invented default. |
| `dp_filterless_bounds_epsilon_fraction` | Fraction of each component's epsilon used by private bound selection. |
| `dp_epsilon`, `dp_delta` | Query budget; grouped queries require delta for partition selection. |
| `dp_max_groups_contributed` | Maximum output groups affected by one logical PU. |

The factor-4 bin geometry and temporary marker bit are representation invariants shared with the
existing PAC clipping machinery, not runtime policy choices. Sampling degree, support, budget
split are DuckDB settings; histogram support is always noised when privacy noise is enabled.

## Privacy status and attacks

For fixed `FROM`, join, aggregate, and `GROUP BY` expressions, changing only `WHERE` does not change
the histogram: the same sampled PUs contribute the same unfiltered per-PU partials. This removes the
filter attack in which an unsampled qualifying PU moved a bin from `T-1` to `T`.

The unsafe raw-histogram prototype is nevertheless private-data-dependent. A database update
affecting a sampled PU can change its bin and the selected bound. Filter independence is therefore
not, by itself, a formal DP argument for releasing an unnoised data-dependent bound.

One sampled PU contributes weight `2^p` in each of at most `C_u` groups. Under add/remove adjacency,
the implementation therefore uses histogram L1 sensitivity:

```text
2^p * C_u
```

and splits each aggregate component's epsilon between histogram selection and the final value.

## Sampling, utility, and execution results

`attacks/filterless_three_way_benchmark.py` compares the current fixed-sample-only histogram with
full filterless execution (`p=0`) and the repository's Google-style DP simulation. The benchmark
uses a static TPC-H SF1 database, epsilon 1, delta 1e-6, support 500, 32 trials, and eight fixed hash
salts for the `p=6` sample. Google tunes `C_u` and its bound-budget fraction; the filterless arms
tune `C_u` and use an unnoised histogram. These historical results characterize the unsafe baseline,
not the extension's current always-private bound selection. Error is relative L1 against the uncapped truth.

| Query | Google | Full `p=0` | Sampled `p=6` |
|---|---:|---:|---:|
| Orders balance / month | 7.26% | 6.83% | 6.68% |
| Orders price / month | 1.38% | 10.01% | 1.36% |
| Lineitem quantity / month | 2.19% | 39.95% | 16.62% |
| Lineitem quantity / month + nation | 45.06% | 91.45% | 90.43% |
| Synthetic bounded SUM / 12 groups | 2.52% | 3.52% | 3.81% |
| Synthetic billionaire excluded | 4.43% | 4.11% | 4.13% |
| Synthetic everyone + billionaires | 11.10% | 10.91% | 10.80% |
| Synthetic COUNT / 120 groups | 68.49% | 70.49% | 71.07% |

AVG independently bounds and noises SUM and COUNT before division:

| Query | Google | Full `p=0` | Sampled `p=6` |
|---|---:|---:|---:|
| Orders price / month | 11.31% | 18.78% | 19.76% |
| Lineitem price / month | 4.85% | 75.47% | 35.64% |
| Synthetic bounded / 12 groups | 9.25% | 9.47% | 9.28% |
| Synthetic log-skew / 120 groups | 76.57% | 86.07% | 84.28% |

Sampling does not introduce a consistent utility penalty in these runs: the sampled and full arms
are close where they choose similar bounds, and either can win when the realized histogram changes
the selected bound or `C_u`. The larger result is that filterless is not uniformly better than
Google. It is competitive on coarse groups and the synthetic billionaire cases, but loses on fine
groupings and on several AVG/SUM cases where its unfiltered bound is wider.

The same harness times the relational input plans. The Google column is filtered pre-aggregation,
not the Google DP library's end-to-end runtime.

| Query | Google input | Full `p=0` | Sampled `p=6` | Sample/full | Sample/Google |
|---|---:|---:|---:|---:|---:|
| Orders balance / month | 0.236 s | 1.482 s | 0.401 s | 0.27x | 1.70x |
| Orders price / month | 0.411 s | 1.595 s | 0.682 s | 0.43x | 1.66x |
| Lineitem quantity / month | 0.915 s | 3.784 s | 1.536 s | 0.41x | 1.68x |
| Lineitem quantity / month + nation | 0.953 s | 3.898 s | 1.574 s | 0.40x | 1.65x |
| Synthetic bounded / 12 groups | 0.008 s | 0.041 s | 0.011 s | 0.25x | 1.38x |
| Synthetic COUNT / 120 groups | 0.029 s | 0.176 s | 0.052 s | 0.30x | 1.78x |

Thus `p=6` reduces the unfiltered relational work by 2.3x-4.2x on the main TPC-H cases, but remains
1.65x-1.70x slower than filtered pre-aggregation. When nearly every PU qualifies, sampling cannot
prune much: the synthetic billionaire inputs are about 1.0x full-filterless time.

`attacks/filterless_encoded_attack.py` isolates the fixed-sample knife edge. Across `p=0,2,4,6`,
changing only the filter gives 50.05%-50.39% scale-classification accuracy. In the unsafe
raw-histogram prototype, adding or removing a sampled PU at the support boundary changes the
selected bound from 128 to 134,217,728 and gives 99.95% accuracy. The extension does not expose that
prototype mode: it noises histogram support before selecting a bound.

## Current limitations

- SUM, COUNT, and AVG are supported; DISTINCT and MIN/MAX are not.
- Aggregate-local `FILTER (WHERE ...)` is rejected.
- More than one input `FILTER` operator is rejected.
- Volatile predicates are rejected because the current plan evaluates the predicate in both the
  widened filter and marker projection.
- Bounds are recomputed per query. There is no metadata table, normalized-SQL cache, or invalidation
  policy.
- No HLL is used. After per-PU pre-aggregation, exact PU support is available directly.
