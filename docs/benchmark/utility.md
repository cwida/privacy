# DP Utility Evaluation

`dp_benchmark_runner` measures runtime and utility for `dp_standard`, `dp_elastic`, and `dp_sass`. Paper configs live
in `benchmark/configs/utility/`; focused checks live in `benchmark/configs/sanity/`.

## Run the Main Experiment

```bash
build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_utility.json
```

The runner uses two passes. The first pass times private queries and checkpoints the CSV. The second pass computes
exact answers, non-private SAA estimators, errors, recall, precision, and noise diagnostics. Diagnostic work is not
included in `time_ms`.

## Bounds

The mechanisms consume different public bounds:

| Setting | Bounded DP | Elastic DP | SAA |
|---|---|---|---|
| `dp_count_bound` | Per-PU COUNT contribution | Not used for elastic sensitivity | Per-PU sample contribution |
| `dp_sum_bound(s)` | Per-PU SUM contribution | Input clipping before FLEX analysis | Per-PU sample contribution |
| `dp_max_groups_contributed` | Groups per privacy unit | Not used | Groups per privacy unit |
| `dp_sass_*_output_*bounds` | Not used | Not used | Public lane-output domain |
| `dp_avg_lower_bounds`, `dp_avg_upper_bounds` | Public AVG domain | Public AVG domain | Public AVG domain |

Each mechanism's multiplier is applied around its own base bound. The paper's SAA base output domains cover the
central 75% of non-private lane outputs. Bound derivation is offline and excluded from runtime.

## Result Columns

A result row is identified by its dataset, query, mode, release, run, bound multiplier, `m`, rescaling choice, and
seed. Important metric families are:

| Columns | Meaning |
|---|---|
| `time_ms`, `runtime_success`, `runtime_error` | Timed private query |
| `metrics_success`, `metrics_error` | Untimed diagnostics |
| `median_error_pct`, `recall`, `precision` | Private result against the exact full-data answer |
| `saa_estimator_*` | Private SAA result against its non-private estimator |
| `saa_sampling_*` | Non-private SAA estimator against the full-data answer |
| `saa_noise_scale_*` | SAA noise diagnostics |
| `q1_*`, `saa_*_q1_*` | Separate Q01 SUM, AVG, and COUNT metrics |

`median_error_pct` is the median absolute relative error across matched numeric output cells, multiplied by 100.

## Sanity Checks

Inspect non-private lane values and central-75 output domains with:

```bash
python3 benchmark/dp/verify_tpch_saa_bounds.py \
  --duckdb build/release/duckdb \
  --db tpch_sf30_graviton.db \
  --config benchmark/configs/utility/dp_tpch_sf30_utility.json \
  --out-dir benchmark/results/sanity/bounds
```

Measure lane stability with `benchmark/dp/run_tpch_sass_stability.py`. Stability is the population standard deviation
of non-private lane outputs divided by the absolute mean. These checks do not contribute to reported runtime.

See [repro.md](../../repro.md) for complete commands and plotting inputs.
