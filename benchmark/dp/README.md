# DP Benchmarks

`dp_benchmark_runner` is the standalone utility and runtime runner for `dp_standard`, `dp_elastic`, and `dp_sass`.
It also accepts `duckdb` as an exact benchmark baseline.

```bash
cmake --build build/release --target dp_benchmark_runner
build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_utility.json --dry-run
build/release/extension/privacy/dp_benchmark_runner \
  --config benchmark/configs/utility/dp_tpch_sf30_utility.json
```

Paper configurations live in `benchmark/configs/utility/`. Optional validation configurations live in
`benchmark/configs/sanity/`. The runner times private queries first and computes reference answers and diagnostics in
a separate untimed pass. It checkpoints completed timing rows after every dataset entry.

The built-in TPC-H workload contains Q01, Q05, Q06, Q14, and Q19. Generic SQL workloads can instead provide query
files and setup SQL in JSON. Use `--runs N` and `--out PATH` for explicit command-line overrides.

See [repro.md](../../repro.md) for the exact paper matrix, bound semantics, CSV columns, plotting commands, and
optional lane-stability and bound-verification checks.
