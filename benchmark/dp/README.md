# DP Benchmark Runner

`dp_benchmark_runner` is the DP-only utility benchmark orchestrator. PAC benchmark
runners are intentionally separate.

Build:

```bash
cmake --build build/release --target dp_benchmark_runner
```

Run a config:

```bash
build/release/extension/privacy/dp_benchmark_runner --config benchmark/dp/configs/tpch_jcch_stock_dp_sf3_bounds.json
```

The runner has one built-in TPC-H/JCC-H DP query set:

- `tpch_stock_dp` / `jcch_stock_dp`: stock customer-linked queries supported by both `dp_standard` and
  `dp_sass` (`Q01`, `Q05`, `Q06`, `Q14`, `Q19`).

For built-in query sets, use `query_names` to run or tune only selected queries:

```json
{"name": "tpch", "workload": "tpch_stock_dp", "query_names": ["q06", "q19"]}
```

Validate config/query discovery without running queries:

```bash
build/release/extension/privacy/dp_benchmark_runner --config benchmark/dp/configs/sqlstorm_smoke.json --dry-run
```

The JSON config accepts scalar or array sweeps for:

- `modes`: `dp_standard`, `dp_elastic`, `dp_sass`, or `all`
  - `duckdb` is also accepted as a benchmark-only exact baseline. It runs the query with privacy rewriting
    disabled and emits perfect utility metrics.
  - `dp_sass_bounded_ratio` is a benchmark-only Q14 mechanism that releases the lane-wise sampled
    promotion-revenue percentage in the public range `[0,100]`.
- `epsilon` / `epsilons`
- `delta` / `deltas`
- `count_bound` / `count_bounds`
- `sum_bound` / `sum_bounds`
- `group_bound` / `group_bounds`
- `c_u`
- `bound_multiplier` / `bound_multipliers`
- `sample_lanes`
- `sass_releases`: `median` or `average`
- `query_names`: optional built-in query-name filter, e.g., `["q01", "q05"]`

CSV rows include the concrete values used for each run, including `epsilon`,
`delta`, `sf`, `c_u`, bounds, sample lanes, utility metrics, and timing metadata.
