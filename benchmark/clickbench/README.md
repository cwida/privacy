# ClickBench AS Benchmark

`as_clickbench_benchmark` compares plain DuckDB with handwritten stochastic-aggregation forms of the aggregate
ClickBench queries. Queries without an aggregate are skipped.

```bash
cmake --build build/release --target as_clickbench_benchmark
build/release/extension/privacy/as_clickbench_benchmark \
  --config benchmark/configs/utility/as_clickbench_graviton.json --dry-run
build/release/extension/privacy/as_clickbench_benchmark \
  --config benchmark/configs/utility/as_clickbench_graviton.json
```

If the configured database does not exist, the runner downloads `hits.parquet` and loads `hits`. It records the median
of five hot runs in `query,mode,m,median_ms` format. Use `benchmark/configs/sanity/as_clickbench_m_sweep.json` for a
focused `m=64,512` check.

See [repro.md](../../repro.md) for the reported machine settings and plotting command.
