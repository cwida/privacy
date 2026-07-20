# Reproducing the Experiments

This guide reproduces the experiments for **SIMD-Accelerated Stochastic Aggregation**. The paper configurations are
versioned under `benchmark/configs/utility/`; optional validation configurations live under
`benchmark/configs/sanity/`. Generated data and results are written below `benchmark/results/` and are not source
files.

Run all commands from the repository root.

## Environment

The reported experiments use DuckDB v1.5.4 and Clang 22.1.0. Build the repository with its submodules, but do not
modify the `duckdb/` submodule.

```bash
git clone --recurse-submodules git@github.com:cwida/privacy.git
cd privacy
export CC=clang
export CXX=clang++
GEN=ninja make
```

To rebuild only the paper benchmark executables:

```bash
cmake --build build/release --target \
  as_tpch_benchmark as_clickbench_benchmark \
  dp_benchmark_runner dp_target_metric_runner
```

The paper uses the following machines:

| Machine | CPU | RAM | Storage |
|---|---|---:|---|
| MacBook Pro | 12-core Apple M2 Max | 32 GB | SSD |
| `c8gd.4xlarge` | 16-core AWS Graviton4 | 32 GB | instance SSD |
| `c8i.4xlarge` | 16-core Intel Granite Rapids | 32 GB | EBS io2 |
| `c8a.4xlarge` | 16-core AMD EPYC 9R45 | 32 GB | EBS io2 |

The full TPC-H and DP configurations use 16 DuckDB threads. Keep the database and DuckDB temporary directory on the
same storage class used for the reported machine.

## Aggregation Microbenchmarks

The aggregation microbenchmarks under `benchmark/pac_microbench/` compare seven implementations over synthetic data.
They cover 10M, 100M, and 1B rows; several value distributions and domains; and grouped and ungrouped execution.

```bash
cd benchmark/pac_microbench
./build_variants.sh
./create_test_db.sh
./run_all.sh
```

The seven variants are `default`, `nobuffering`, `noboundopt`, `signedsum`, `exactsum`, `nocascading`, and `nosimd`.
Results and the microbenchmark plotting programs remain self-contained in that directory.

## Standalone Benchmarks

Each paper benchmark is a standalone C++ executable. JSON files contain the complete run setup and can be checked
without opening a database:

```bash
build/release/extension/privacy/as_tpch_benchmark \
  --config benchmark/configs/utility/as_tpch_sf30.json --dry-run

build/release/extension/privacy/as_clickbench_benchmark \
  --config benchmark/configs/utility/as_clickbench_graviton.json --dry-run

build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_utility.json --dry-run
```

The runners create the configured output parent directories. Existing result files are replaced only after input
discovery and database initialization succeed.

### TPC-H AS performance

The main SF30 benchmark measures DuckDB, SIMD-AS with `m=64`, and the naive sampling implementation. It creates
`tpch_sf30_graviton.db` with DuckDB's TPC-H generator when the database is absent. Each reported runtime is the median
of five hot executions after one warm execution.

```bash
build/release/extension/privacy/as_tpch_benchmark \
  --config benchmark/configs/utility/as_tpch_sf30.json
```

The variable-`m` configuration measures the separate aggregate path for `m=128,256,512`. It reuses the same database
and skips the naive implementation.

```bash
build/release/extension/privacy/as_tpch_benchmark \
  --config benchmark/configs/utility/as_tpch_sf30_m_sweep.json
```

Outputs:

- `benchmark/results/utility/as_tpch_sf30.csv`
- `benchmark/results/utility/as_tpch_sf30_m_sweep.csv`

### ClickBench AS performance

This benchmark runs the handwritten AS forms of all aggregate ClickBench queries. Queries without an aggregate are
skipped. If `clickbench.db` is absent, the runner downloads `hits.parquet` and builds the database. The reported setup
uses 16 threads and a 16 GB DuckDB memory limit.

```bash
build/release/extension/privacy/as_clickbench_benchmark \
  --config benchmark/configs/utility/as_clickbench_graviton.json
```

Output: `benchmark/results/utility/as_clickbench_graviton.csv`.

### DP utility and runtime

The DP benchmark runs TPC-H SF30 queries Q01, Q05, Q06, Q14, and Q19 with the same customer privacy unit and FK chain:

```text
lineitem.l_orderkey -> orders.o_orderkey
orders.o_custkey    -> customer.c_custkey (privacy unit)
```

All mechanisms use `epsilon=1` and `delta=1e-6`. The benchmark performs three runs and evaluates bound multipliers
`0.01,0.1,1,10,100`. It compares user-level bounded DP, row-level elastic DP, and SAA-average with
`m=64,128,256,512`; DuckDB supplies the exact runtime and utility reference. `sample_lanes=1`, so SAA lanes are
disjoint. SUM and COUNT outputs are rescaled to the full-data scale; AVG, MIN, and MAX are not rescaled.

```bash
build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_utility.json
```

Output: `benchmark/results/utility/dp_tpch_sf30_utility.csv`.

The runner deliberately uses two passes. It first times every private query and checkpoints the CSV after each
dataset entry. It then computes exact references, SAA estimators, error decompositions, and noise diagnostics without
including those diagnostics in `time_ms`. Timings include extension rewriting and execution. For elastic DP they also
include auxiliary max-frequency queries; for example, the maximum number of `lineitem` rows per `l_orderkey` used in
the FLEX sensitivity calculation is part of the measured query.

### Q01 AVG study

Two configurations isolate the three Q01 AVG columns and then vary the grouping keys. Both use three runs, the same
privacy parameters as the main utility benchmark, and compare bounded DP, elastic DP, and SAA-average at `m=64` and
`m=512`.

```bash
build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_q01_avg_columns.json

build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_q01_avg_groups.json
```

Outputs:

- `benchmark/results/utility/dp_tpch_sf30_q01_avg_columns.csv`
- `benchmark/results/utility/dp_tpch_sf30_q01_avg_groups.csv`

## DP Setup

The modes require different public bounds:

| Setting | Bounded DP | Elastic DP | SAA |
|---|---|---|---|
| `dp_count_bound` | per-PU COUNT and AVG-count contribution | not used for elastic sensitivity | per-PU sample contribution |
| `dp_sum_bound(s)` | per-PU SUM and AVG-sum contribution | input clipping before FLEX analysis | per-PU sample contribution |
| `dp_max_groups_contributed` (`C_u`) | maximum groups per privacy unit | not used | maximum groups per privacy unit |
| `dp_sass_*_output_*bounds` | not used | not used | public lane-output domains |
| `dp_avg_lower_bounds`, `dp_avg_upper_bounds` | public AVG domains | public AVG domains | public AVG domains |

Each mechanism is multiplied around its own base bound. The `1` point is the mechanism-specific oracle-like bound;
smaller and larger multipliers intentionally test under- and over-estimation. Bound derivation is offline and is not
included in runtime.

SAA output domains are selected independently from bounded-DP contribution bounds. For every query, aggregate, and
`m`, the base interval covers the central 75% of non-private lane outputs, following the approximate-range strategy
used by GUPT. The interval is then widened across output cells of the same aggregate. These are evaluation bounds:
they show mechanism behavior under comparable mechanism-specific tuning, not a private production range estimator.

For grouped bounded DP and SAA, the runner reserves privacy budget for private partition selection. AVG implemented as
a noisy SUM divided by a noisy COUNT splits its value budget equally between the two components. `C_u` is the maximum
number of output groups one privacy unit may affect.

## Result Columns

A DP result row is identified by `dataset`, `workload`, `query`, `mode`, `release`, `run`, `bound_multiplier`,
`dp_sass_m`, `dp_sass_rescale`, and `seed`. The query's result-key columns only match grouped private and exact rows;
they are not privacy keys.

Important result families are:

| Columns | Meaning |
|---|---|
| `time_ms`, `runtime_success`, `runtime_error` | private-query timing pass |
| `metrics_success`, `metrics_error`, `success` | untimed diagnostic status and combined status |
| `median_error_pct`, `recall`, `precision` | private result versus the exact full-data answer |
| `saa_estimator_*` | private SAA result versus the same non-private SAA estimator |
| `saa_sampling_*` | non-private SAA estimator versus the exact full-data answer |
| `saa_noise_scale_*` | released SAA noise-scale diagnostics |
| `q1_*`, `saa_*_q1_*` | Q01 SUM, AVG, and COUNT metrics reported separately |

`median_error_pct` is the median absolute relative error over matched numeric output cells, multiplied by 100. Recall
and precision describe released group keys. A missing metric is distinct from a failed timing: inspect both status
families rather than filtering only on `success`.

## Plotting

Install R 4.x and these packages once: `ggplot2`, `dplyr`, `readr`, `scales`, `stringr`, `systemfonts`, `tidyr`,
`patchwork`, and `ggpattern`. Linux Libertine is used when installed; otherwise the scripts use a serif fallback.

```bash
mkdir -p benchmark/results/figures

Rscript --vanilla benchmark/tpch/plot_tpch_results.R \
  benchmark/results/utility/as_tpch_sf30.csv benchmark/results/figures

Rscript --vanilla benchmark/tpch/plot_tpch_as_m_sweep.R \
  benchmark/results/utility/as_tpch_sf30.csv \
  benchmark/results/utility/as_tpch_sf30_m_sweep.csv \
  benchmark/results/figures

Rscript --vanilla benchmark/clickbench/plot_clickbench_as_slowdown.R \
  benchmark/results/utility/as_clickbench_graviton.csv benchmark/results/figures 64

Rscript --vanilla benchmark/dp/plot_unskewed_sass_utility_views.R \
  benchmark/results/utility/dp_tpch_sf30_utility.csv benchmark/results/figures 1e-6 64 true

Rscript --vanilla benchmark/dp/plot_sass_sampled_m_1x.R \
  benchmark/results/utility/dp_tpch_sf30_utility.csv \
  benchmark/results/figures/tpch_sass_sampled_m_1x_paper.png tpch 1e-6 median mean

Rscript --vanilla benchmark/dp/plot_dp_full_utility_runtime.R \
  benchmark/results/utility/dp_tpch_sf30_utility.csv benchmark/results/figures

Rscript --vanilla benchmark/dp/plot_q01_avg_compact_grid.R \
  benchmark/results/utility/dp_tpch_sf30_q01_avg_columns.csv \
  benchmark/results/utility/dp_tpch_sf30_q01_avg_groups.csv \
  benchmark/results/figures/q01_avg_compact_grid_paper.png
```

Plot scripts fail with a direct dependency or schema error; they do not install packages or modify benchmark inputs.

## Optional Sanity Checks

These checks are not mandatory benchmark stages and are excluded from reported runtime.

### Inspect SAA bounds and lane outputs

The verifier prints non-private lane values and central-75 intervals without changing the canonical config:

```bash
python3 benchmark/dp/verify_tpch_saa_bounds.py \
  --duckdb build/release/duckdb \
  --db tpch_sf30_graviton.db \
  --config benchmark/configs/utility/dp_tpch_sf30_utility.json \
  --out-dir benchmark/results/sanity/bounds
```

Do not pass `--update-config` when reproducing the paper; that option is only for deliberately regenerating bound
files.

### Measure lane stability

For each output cell, stability is the coefficient of variation of its non-private lane outputs,
`stddev_pop(lanes) / abs(mean(lanes))`. The summary reports the median across output cells. AVG and rescaled SUM/COUNT
are collected separately because only the latter change scale.

```bash
python3 benchmark/dp/run_tpch_sass_stability.py \
  --config benchmark/configs/utility/dp_tpch_sf30_q01_stability_avg.json \
  --duckdb build/release/duckdb \
  --out benchmark/results/utility/q01_stability_avg.csv \
  --summary-out benchmark/results/utility/q01_stability_avg_summary.csv \
  --strict

python3 benchmark/dp/run_tpch_sass_stability.py \
  --config benchmark/configs/utility/dp_tpch_sf30_q01_stability_sum_count.json \
  --duckdb build/release/duckdb \
  --out benchmark/results/utility/q01_stability_sum_count.csv \
  --summary-out benchmark/results/utility/q01_stability_sum_count_summary.csv \
  --strict
```

The remaining files in `benchmark/configs/sanity/` are focused smoke, beta, support-threshold, bounds, Reddit, and
SQLStorm checks. Validate one with `dp_benchmark_runner --dry-run` before use. They are diagnostics, not part of the
paper result matrix.
