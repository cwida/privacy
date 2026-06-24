# DP Benchmark Runner

`dp_benchmark_runner` is the DP-only utility benchmark orchestrator. PAC benchmark
runners are intentionally separate.

Build:

```bash
cmake --build build/release --target dp_benchmark_runner
```

Run a config:

```bash
build/release/extension/privacy/dp_benchmark_runner --config benchmark/dp/configs/tpch_smoke.json
```

Validate config/query discovery without running queries:

```bash
build/release/extension/privacy/dp_benchmark_runner --config benchmark/dp/configs/sqlstorm_smoke.json --dry-run
```

The JSON config accepts scalar or array sweeps for:

- `modes`: `dp_standard`, `dp_elastic`, `dp_sass`, or `all`
- `epsilon` / `epsilons`
- `delta` / `deltas`
- `count_bound` / `count_bounds`
- `sum_bound` / `sum_bounds`
- `group_bound` / `group_bounds`
- `c_u`
- `bound_multiplier` / `bound_multipliers`
- `sample_lanes`
- `sass_releases`: `median` or `average`

CSV rows include the concrete values used for each run, including `epsilon`,
`delta`, `sf`, `c_u`, bounds, sample lanes, utility metrics, and timing metadata.
