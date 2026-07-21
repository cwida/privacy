# ClickBench AS Benchmark

`as_clickbench_benchmark` compares plain DuckDB with handwritten stochastic-aggregation forms of the aggregate
ClickBench queries. Queries without an aggregate are skipped.

## Run the Paper Configuration

```bash
cmake --build build/release --target as_clickbench_benchmark

build/release/extension/privacy/as_clickbench_benchmark \
  --config benchmark/configs/utility/as_clickbench_graviton.json --dry-run

build/release/extension/privacy/as_clickbench_benchmark \
  --config benchmark/configs/utility/as_clickbench_graviton.json
```

The runner creates `clickbench.db` from `hits.parquet` when the database is absent. It downloads the Parquet file when
needed. The paper config uses 16 threads, a 16 GB memory limit, `m=64`, and five measured runs after one untimed warmup
per query and mode.

The handwritten queries are in `benchmark/clickbench/clickbench_as_queries/`. The original ClickBench setup and query
files are in `benchmark/clickbench/clickbench_queries/`.

## Variable-m Sanity Check

Use the focused config to compare `m=64` and `m=512` on a small query set:

```bash
build/release/extension/privacy/as_clickbench_benchmark \
  --config benchmark/configs/sanity/as_clickbench_m_sweep.json
```

## Plot Results

```bash
Rscript --vanilla benchmark/clickbench/plot_clickbench_as_slowdown.R \
  benchmark/results/utility/as_clickbench_graviton.csv \
  benchmark/results/figures 64
```

The CSV contains one row per query, mode, and `m`. See [repro.md](../../repro.md) for the reported machine and complete
reproduction procedure.
