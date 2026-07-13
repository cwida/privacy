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
- `sass_m` / `sass_ms` (default `64`; larger values use the experimental variable-m SASS path)
- `sass_releases`: `median` or `average`
- `query_names`: optional built-in query-name filter, e.g., `["q01", "q05"]`

`bound_multiplier` scales each mechanism's configured base bounds. For
`dp_standard` and `dp_elastic`, this means the contribution or value-clipping
bounds. For `dp_sass`, configured `sass_count_output_bounds` and
`sass_sum_output_bounds` are SAA output-domain base bounds and are scaled by the
same multiplier. If a SASS output bound is omitted, the runner derives a
conservative fallback from the scaled contribution bound and the privacy-unit
count.

CSV rows include the concrete values used for each run, including `epsilon`,
`delta`, `sf`, `c_u`, bounds, sample lanes, utility metrics, and timing metadata.
The main `utility`/`median_error_pct` columns compare the private output against
the full-data DuckDB answer. For `dp_sass`, the runner also emits:

- `saa_estimator_*`: private SAA output compared with the same SAA estimator run with
  `privacy_noise=false` (DP error around the estimator).
- `saa_sampling_*`: no-noise SAA estimator compared with the full-data DuckDB answer
  (sampling/clipping/thresholding error relative to the full answer).
