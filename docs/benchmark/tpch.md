# TPC-H Benchmarks

TPC-H supplies the paper's end-to-end AS performance and DP utility workloads. The AS and DP experiments use separate
standalone executables.

## AS Performance

The main benchmark compares DuckDB, SIMD-AS at `m=64`, a naive sampling implementation, and a simple-hash control.

```bash
cmake --build build/release --target as_tpch_benchmark

build/release/extension/privacy/as_tpch_benchmark \
  --config benchmark/configs/utility/as_tpch_sf30.json --dry-run

build/release/extension/privacy/as_tpch_benchmark \
  --config benchmark/configs/utility/as_tpch_sf30.json
```

The runner generates the configured TPC-H database when it is absent. Handwritten query variants live in:

- `benchmark/tpch/tpch_as_queries/`
- `benchmark/tpch/tpch_as_simple_hash_queries/`
- `benchmark/tpch/tpch_naive_as_queries/`

The variable-`m` experiment uses the same runner and database:

```bash
build/release/extension/privacy/as_tpch_benchmark \
  --config benchmark/configs/utility/as_tpch_sf30_m_sweep.json
```

Each query and mode receives one untimed warmup immediately before its measured executions.

## DP Utility and Runtime

`dp_benchmark_runner` evaluates DuckDB, bounded DP, elastic DP, and SAA over Q01, Q05, Q06, Q14, and Q19.

```bash
cmake --build build/release --target dp_benchmark_runner

build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_utility.json --dry-run

build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_utility.json
```

The customer is the privacy unit. `lineitem` reaches it through `orders`. The runner first records private-query
runtime, then computes exact references and diagnostics in an untimed pass. Timing rows are checkpointed after each
dataset entry.

The Q01 AVG configurations isolate output columns and grouping choices:

```bash
build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_q01_avg_columns.json

build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_q01_avg_groups.json
```

## Plot Results

```bash
Rscript --vanilla benchmark/tpch/plot_tpch_results.R \
  benchmark/results/utility/as_tpch_sf30.csv benchmark/results/figures

Rscript --vanilla benchmark/tpch/plot_tpch_as_m_sweep.R \
  benchmark/results/utility/as_tpch_sf30.csv \
  benchmark/results/utility/as_tpch_sf30_m_sweep.csv \
  benchmark/results/figures
```

See [Utility evaluation](utility.md) for bound and metric definitions. See [repro.md](../../repro.md) for every paper
config and plotting command.
