# Benchmarks

The repository contains standalone performance and utility benchmarks for stochastic aggregation and differential
privacy. The exact paper environment, experiment matrix, bounds, result columns, and plotting commands are documented
in [repro.md](../../repro.md).

Run commands from the repository root. Build the paper executables with:

```bash
cmake --build build/release --target \
  as_tpch_benchmark as_clickbench_benchmark \
  dp_benchmark_runner
```

## Paper Benchmarks

| Workload | Executable | Configuration |
|---|---|---|
| TPC-H AS | `as_tpch_benchmark` | `benchmark/configs/utility/as_tpch_sf30.json` |
| TPC-H variable `m` | `as_tpch_benchmark` | `benchmark/configs/utility/as_tpch_sf30_m_sweep.json` |
| ClickBench AS | `as_clickbench_benchmark` | `benchmark/configs/utility/as_clickbench_graviton.json` |
| TPC-H DP utility | `dp_benchmark_runner` | `benchmark/configs/utility/dp_tpch_sf30_utility.json` |
| TPC-H Q01 AVG | `dp_benchmark_runner` | `benchmark/configs/utility/dp_tpch_sf30_q01_avg_*.json` |

The runners accept `--config <path> --dry-run` to validate a JSON configuration without opening a database. Generated
databases and results are ignored by Git and written below `benchmark/results/` unless a config selects another path.

## Additional Benchmarks

- [Aggregation microbenchmarks](microbenchmarks.md) compare aggregate kernel implementations.
- [IMDb/JOB](imdb.md) exercises person-level privacy over a multi-table workload.
- [SQLStorm](sqlstorm.md) provides optional query-coverage smoke tests.
- [Utility evaluation](utility.md) documents DP bounds, metrics, and diagnostic tools.

## Workload Guides

- [TPC-H](tpch.md)
- [ClickBench](clickbench.md)
- [IMDb/JOB](imdb.md)
- [SQLStorm](sqlstorm.md)
- [Microbenchmarks](microbenchmarks.md)
- [Utility evaluation](utility.md)
