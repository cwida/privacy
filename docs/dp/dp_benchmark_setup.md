# DP benchmark setup

This page records the benchmark setup used for the SF30 TPC-H/JCC-H DP utility
experiments. It is an experiment audit document: use it to check whether the
datasets, bounds, skew generation, and metrics match the setup we want to
describe in the paper.

The mechanism details live in [dp_mechanisms.md](dp_mechanisms.md). This page
only describes how the experiments are configured.

## Workload

The benchmark uses the built-in `tpch_stock_dp` and `jcch_stock_dp` query set in
`benchmark/dp/dp_benchmark_runner.cpp`. It includes five customer-linked TPC-H
queries that all DP modes can run:

| Query | Shape | Result keys |
|---|---|---:|
| `q01` | pricing summary with `SUM`, `AVG`, and `COUNT` | 2 |
| `q05` | grouped revenue by nation | 1 |
| `q06` | scalar revenue | 0 |
| `q14` | scalar promotion revenue ratio | 0 |
| `q19` | scalar discount revenue | 0 |

The privacy unit is `customer`. The benchmark installs this metadata before
running private modes:

```sql
PRAGMA clear_privacy_metadata;
ALTER TABLE customer ADD PRIVACY_KEY (c_custkey);
ALTER TABLE customer SET PU;
ALTER TABLE orders ADD PRIVACY_LINK (o_custkey) REFERENCES customer(c_custkey);
ALTER TABLE lineitem ADD PRIVACY_LINK (l_orderkey) REFERENCES orders(o_orderkey);
```

## Dataset variants

The unskewed baseline is TPC-H SF30. The current Graviton runs use
`tpch_sf30_graviton.db`.

The skewed datasets are controlled JCC-H-style variants generated from the same
SF30 base database. The skew script copies the base database, remaps a fraction
of `orders.o_custkey`, reapplies privacy metadata, validates that all customer
references are valid, and then runs the benchmark.

```sql
UPDATE orders
SET o_custkey = 1 + (o_orderkey % <n_whales>)
WHERE o_orderkey % 100 < <remap_percent>;
```

The controlled sweep uses this grid:

| Parameter | Values |
|---|---|
| `remap_percent` | `10`, `30`, `60` |
| `whales_per_sf` | `50`, `150`, `450` |
| `n_whales` at SF30 | `1500`, `4500`, `13500` |

This changes which customers own orders. It does not change the public TPC-H
value domains for `lineitem` columns such as `l_quantity`, `l_extendedprice`, or
`l_discount`.

The skew script writes metadata to
`benchmark/dp/skew_sweep/controlled_skew_sf30_metadata.csv`. It records the
variant name, database path, remap percentage, whale count, total order rows,
distinct order customers, max orders per customer, and p99 orders per customer.

## Benchmark parameters

The canonical config for this sweep is:

```text
benchmark/dp/configs/tpch_jcch_sf30_all5_full_sass_m64_m128_m256_m512_3run.json
```

The controlled skew wrapper is:

```text
benchmark/dp/generate_controlled_skew_sweep.py
```

The active parameter grid is:

| Parameter | Value |
|---|---|
| `runs` | `3` |
| `threads` | `4` |
| `generate_threads` | `16` |
| `epsilon` | `1.0` |
| `delta` | `2.2222222222222222e-7` |
| `bound_multipliers` | `0.01`, `0.1`, `1`, `10`, `100` |
| `sample_lanes` | `1` |
| `dp_sass_m` | `64`, `128`, `256`, `512` |
| `dp_sass_release` | `median`, `average` |
| `seed_base` | `1000003` |

`delta = 2.2222222222222222e-7` is `1 / 4,500,000`, using the public TPC-H SF30
customer scale.

The runner derives a deterministic `privacy_seed` for each row from `seed_base`,
the run number, the query index, and the size of the run-point grid. The emitted
CSV records the concrete seed used for each row.

The benchmark modes are:

| Mode | Role in the benchmark |
|---|---|
| `duckdb` | exact non-private baseline, only for the unskewed baseline dataset |
| `dp_standard` | user-level bounded-contribution DP baseline |
| `dp_elastic` | row-level FLEX baseline |
| `dp_sass` | SAA mechanism, run with both `median` and `average` release |

For skew variants, the wrapper skips `duckdb` rows. This avoids rerunning the
standalone DuckDB timing rows for every skew setting. Private rows still compare
against a non-private reference for the same skewed dataset.

## Bounds

All bounds in this benchmark are public, admin-chosen constants. They are not
privately estimated from the benchmark database.

`dp_count_bound` and `dp_sum_bound` are contribution bounds. The benchmark
multiplies them by each `bound_multiplier`. The multipliers test tight and loose
public clipping settings. They do not scale `dp_sass_*_output_bound`, and they
do not scale the public AVG value domains.

| Query | Dataset config | `dp_count_bound` | `dp_sum_bound` | `c_u` | `dp_sass_count_output_bound` | `dp_sass_sum_output_bound` |
|---|---|---:|---:|---:|---:|---:|
| `q01` | TPC-H/JCC-H | `1000` | `1000000` | `5` | `100000000` | `10000000000000` |
| `q05` | TPC-H/JCC-H | `1` | `1000000` | `5` | unset | `10000000000` |
| `q06` | TPC-H | `1` | `100000` | `1` | unset | `4000000000` |
| `q06` | JCC-H/skew | `1` | `1000000` | `1` | unset | `4000000000` |
| `q14` | TPC-H/JCC-H | `1` | `1000000` | `1` | unset | `100000000000` |
| `q19` | TPC-H | `1` | `100000` | `1` | unset | `100000000` |
| `q19` | JCC-H/skew | `1` | `1000000` | `1` | unset | `100000000` |

For `q01`, the AVG aggregates use public TPC-H value-domain bounds:

| AVG aggregate | Lower bound | Upper bound |
|---|---:|---:|
| `avg(l_quantity)` | `0` | `50` |
| `avg(l_extendedprice)` | `0` | `105000` |
| `avg(l_discount)` | `0` | `0.1` |

These are value-domain bounds, not output oracle bounds. They remain valid for
the controlled skew sweep because the skew only changes `orders.o_custkey`.

### How bounds are chosen

Contribution bounds are fixed in the JSON config. The runner reads them, applies
the `bound_multiplier`, and sets:

```text
dp_count_bound = configured_count_bound * bound_multiplier
dp_sum_bound   = configured_sum_bound * bound_multiplier
```

The runner also sets `dp_max_groups_contributed` from `c_u`. These values are
treated as public experiment parameters, not as private estimates.

For `q01`, the AVG domains come from public TPC-H value ranges. We use `[0, 50]`
for `l_quantity`, `[0, 105000]` for `l_extendedprice`, and `[0, 0.1]` for
`l_discount`. The lower bounds use `0` to keep the public domains simple and
valid.

SASS output bounds are also public. When a config entry supplies
`sass_count_output_bounds` or `sass_sum_output_bounds`, the runner uses that
value directly. If one is omitted, the runner derives a fallback from the public
contribution bound and the public privacy-unit count:

```text
derived_output_bound = max(1, per_pu_bound * ceil(number_of_privacy_units))
```

This bound is on the rescaled full-output scale. It is not the raw per-lane
sample scale, because SASS finalizers rescale lane outputs before clamping and
adding noise.

`dp_elastic` is the exception. It computes FLEX max-frequency statistics from
the database during query rewriting. Those statistics are part of the elastic
sensitivity mechanism, not configured public bounds.

### Runtime accounting

The benchmark timer starts after the runner applies privacy metadata, reads the
privacy-unit count, derives any missing SASS output bound, and sets all DP
options.

The reported `time_ms` does not include:

- reading the JSON config;
- applying `PRIVACY_KEY` / `PRIVACY_LINK` metadata;
- reading the privacy-unit count for default `delta` or derived SASS bounds;
- deriving SASS output-bound fallbacks.

The reported `time_ms` does include the benchmarked `con.Query(...)` call and
fetching all result chunks. It also includes rewrite-time work triggered by that
query. For `dp_elastic`, this means `time_ms` includes the auxiliary
max-frequency queries used to compute FLEX sensitivity.

The `saa_estimator_*` and `saa_sampling_*` diagnostic queries run after the main
timed query. They are not included in `time_ms`, but they do increase total
wall-clock time for the full benchmark process.

## Privacy budget

The config fixes total `epsilon = 1.0` for every private row. The mechanisms
split that total budget internally.

Let `k` be the number of user-visible aggregate outputs before AVG
decomposition. AVG counts as one user-visible output, even when implemented with
SUM and COUNT components.

For `dp_standard`:

- Ungrouped queries split epsilon evenly across the `k` aggregate outputs.
- Grouped queries reserve one extra cell for private partition selection.
- The support count gets `epsilon_eta = epsilon / (k + 1)`.
- Each aggregate gets `epsilon / (k + 1)` when grouped.
- AVG components split the aggregate's share in half across SUM and COUNT.
- Grouped partition selection uses `delta_eta = delta / (k + 1)`.

For `dp_sass`:

- Ungrouped queries split epsilon across the `k` aggregate outputs.
- Grouped queries reserve `epsilon_eta = epsilon / (k + 1)` and
  `delta_eta = delta / (k + 1)` for private partition selection.
- Grouped median-release aggregate cells use
  `epsilon / ((k + 1) * C_u)` and `delta / ((k + 1) * C_u)`.
- Grouped average-release aggregate cells use the same epsilon split, but their
  value release is pure epsilon; delta is used for partition selection.
- Ungrouped queries use `C_u = 1` and do not reserve the partition-selection
  cell.
- Ratio-style AVG components split their aggregate cell budget in half.

For `dp_elastic`:

- The benchmark treats it as the row-level FLEX baseline.
- It does not use private partition selection.
- It uses the configured `delta` for smooth elastic sensitivity:
  `beta = epsilon / (2 * ln(2 / delta))`.
- The Laplace release budget is split across aggregate outputs by the shared
  Laplace pipeline.

## Delta

The current sweep fixes `delta = 2.2222222222222222e-7`, which is
`1 / 4,500,000`. This uses the public TPC-H SF30 customer scale.

If a config omits `delta`, the runner defaults to `1 / number_of_privacy_units`
for private modes. If the privacy-unit count is unavailable or invalid, it falls
back to `1e-6`.

Grouped `dp_standard` and grouped `dp_sass` need a non-zero delta because they
privately release the group-key set. `dp_sass` with median release also uses
delta for smooth sensitivity. Ungrouped pure-epsilon cases do not conceptually
need delta, but this benchmark still records the configured delta value for
consistency across modes.

## Result matrix

Each result row is one `(dataset, query, mode, release, run, bound setting, m)`
point. With the current config:

| Dataset group | Rows per run | Runs | Total rows |
|---|---:|---:|---:|
| unskewed baseline | `255` | `3` | `765` |
| one skew variant | `250` | `3` | `750` |
| nine skew variants | `250 * 9` | `3` | `6750` |

The expected combined result file has `7515` data rows plus one header row.

The row counts come from this per-query matrix:

| Dataset group | `duckdb` | `dp_standard` | `dp_elastic` | `dp_sass` |
|---|---:|---:|---:|---:|
| unskewed baseline | `1` | `5` | `5` | `5 * 4 * 2 = 40` |
| skew variant | `0` | `5` | `5` | `5 * 4 * 2 = 40` |

For `dp_sass`, the factors are five bound multipliers, four `m` values, and two
release rules.

## Metrics

The main utility columns compare the private output with the exact DuckDB answer
on the same dataset:

| Column | Meaning |
|---|---|
| `utility` | mean percent absolute error across matched output value columns |
| `median_error_pct` | median percent absolute error across matched output cells |
| `recall` | fraction of reference rows present in the private result |
| `precision` | fraction of private result rows present in the reference result |
| `time_ms` | wall-clock time for the benchmarked query execution |

For grouped queries, the first `key_cols` output columns identify result rows.
For scalar queries, the row index is the comparison key.

For `dp_sass`, the runner also records two diagnostic comparisons:

| Columns | Comparison | Interpretation |
|---|---|---|
| `saa_estimator_*` | private SAA output vs. no-noise SAA output | DP noise error around the SAA estimator |
| `saa_sampling_*` | no-noise SAA output vs. full DuckDB answer | sampling, clipping, and thresholding error vs. the full answer |

These diagnostics answer a different question from the main utility columns. The
main columns measure error against the full-data query answer. The SAA diagnostics
separate the private release error from the sampling error of the SAA estimator.

## Stability analysis

Lane stability is measured separately from the main utility sweep. The stability
script runs the SAA rewrite in a diagnostic mode that exposes the per-lane
non-private sample outputs before the DP release step.

For each query, output group, and aggregate output, it computes summary
statistics over the lane vector:

```text
lane_outputs = [x_1, x_2, ..., x_m]
mean          = avg(lane_outputs)
stddev_pop    = population standard deviation of lane_outputs
cv            = stddev_pop / abs(mean)
```

The main stability metric is `cv`, the coefficient of variation. It is
scale-normalized, so a SUM output around `1e12` and an AVG output around `10`
can be compared on the same rough scale. Lower `cv` means the subsample answers
are more similar to each other, so the SAA estimator is more stable.

If the lane mean is zero and all lanes are also zero, `cv` is `0`. If the lane
mean is zero but the lanes vary, `cv` is `NULL`, because the normalized ratio is
undefined.

The script also records `median`, `range`, median absolute deviation (`mad`),
and `max_abs_dev`. These help distinguish a uniformly noisy lane distribution
from one dominated by a few outlier lanes.

Use this metric to interpret the SAA utility results:

- low `cv`: sampling error should be small, so DP noise is the main remaining
  source of error;
- high `cv`: the random subsamples disagree, so SAA can have high error even
  before DP noise;
- high `max_abs_dev` with moderate `cv`: one or a few unstable lanes may be
  driving the smooth-sensitivity noise scale.

The stability scripts are:

```text
benchmark/dp/run_tpch_jcch_sass_stability.py
benchmark/dp/plot_sass_stability_quick.py
```

## Reproduce the controlled sweep

Build the runner:

```bash
cmake --build build/release --target dp_benchmark_runner
```

Run the sequential controlled-skew sweep:

```bash
python3 benchmark/dp/generate_controlled_skew_sweep.py \
    --run-sequential \
    --runs 3 \
    --template benchmark/dp/configs/tpch_jcch_sf30_all5_full_sass_m64_m128_m256_m512_3run.json \
    --threads 4 \
    --generate-threads 16
```

Sequential mode creates one skew database at a time, runs the benchmark, appends
to the combined CSV, and removes that skew database unless `--keep-skew-dbs` is
set.

Expected outputs:

```text
benchmark/dp/skew_sweep/controlled_skew_sf30_sequential_3run_results.csv
benchmark/dp/skew_sweep/controlled_skew_sf30_metadata.csv
```

## Limitations

- The bounds are public constants. The benchmark does not include a private
  bound-estimation step.
- The AVG bounds for `q01` are public TPC-H value domains, not tuned private
  outputs.
- `dp_elastic` is reported as the row-level FLEX baseline. It is not a user-level
  mechanism.
- The skew sweep changes customer ownership of orders. It does not model every
  possible kind of data skew.
- `dp_sass` diagnostic metrics run extra queries with noise disabled and with
  rewriting disabled. They are useful for error decomposition, but they add
  benchmark runtime.
- The main utility columns measure error against the full-data answer. This
  includes both SAA sampling error and DP release error for `dp_sass`.

## Review checklist

Ask reviewers to check these points before using the results in the paper:

- The customer-level privacy-unit choice matches the stated experiment.
- The controlled skew update matches the intended synthetic skew model.
- The contribution bounds and `c_u` values are acceptable public constants.
- The Q1 AVG bounds are acceptable as public TPC-H value-domain bounds.
- The paper distinguishes full-answer utility from SAA estimator-relative error.
- The paper states that `dp_elastic` is a row-level FLEX baseline.

## See also

- [DP mechanisms](dp_mechanisms.md) - Implementation reference for the three DP modes.
- [DP-SASS smooth sensitivity](dp_sass_smooth_sensitivity.md) - Smooth-sensitivity details.
- [DP benchmark runner](../../benchmark/dp/README.md) - Runner configuration and CSV columns.
